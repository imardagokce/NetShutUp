//! Where the report goes, and the marker that flags an unfinished self-test.

use std::path::{Path, PathBuf};
use std::process::Command;

use windows::Win32::System::Com::CoTaskMemFree;
use windows::Win32::UI::Shell::{FOLDERID_Desktop, KF_FLAG_DEFAULT, SHGetKnownFolderPath};

use super::Report;

const MARKER_NAME: &str = "selftest-in-progress";

fn data_dir() -> PathBuf {
    let base = std::env::var_os("ProgramData").unwrap_or_else(|| r"C:\ProgramData".into());
    PathBuf::from(base).join("NetShutUp")
}

/// A file that exists while a self-test runs. If it is still there at the
/// next start, the last self-test did not finish.
pub fn marker_path() -> PathBuf {
    data_dir().join(MARKER_NAME)
}

pub fn reports_dir() -> PathBuf {
    data_dir().join("reports")
}

/// What the marker file says about the last self-test.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Marker {
    /// No marker: the last self-test finished and everything was restored.
    Clear,
    /// A self-test is running or was cut off while nothing was applied.
    Running,
    /// A profile applied by the self-test is still in place; this is its backup.
    Active(PathBuf),
    /// The self-test ended but settings are not as they were.
    NotRestored,
}

fn format_marker(marker: &Marker) -> Option<String> {
    match marker {
        Marker::Clear => None,
        Marker::Running => Some("running".to_string()),
        Marker::NotRestored => Some("not-restored".to_string()),
        Marker::Active(path) => Some(format!("active {}", path.display())),
    }
}

fn parse_marker(text: &str) -> Marker {
    let text = text.trim();
    match text.strip_prefix("active ") {
        Some(path) => Marker::Active(PathBuf::from(path)),
        None if text == "not-restored" => Marker::NotRestored,
        None => Marker::Running,
    }
}

pub fn read_marker() -> Marker {
    match std::fs::read_to_string(marker_path()) {
        Ok(text) => parse_marker(&text),
        Err(_) => Marker::Clear,
    }
}

pub fn write_marker(marker: &Marker) {
    let path = marker_path();
    match format_marker(marker) {
        None => {
            let _ = std::fs::remove_file(&path);
        }
        Some(text) => {
            if let Some(parent) = path.parent() {
                let _ = std::fs::create_dir_all(parent);
            }
            let _ = std::fs::write(&path, text);
        }
    }
}

/// The marker after `reverted` was restored. A marker that names another backup stays.
pub fn marker_after_revert(current: &Marker, reverted: &Path) -> Marker {
    match current {
        Marker::Active(path) if path != reverted => current.clone(),
        _ => Marker::Clear,
    }
}

/// The warning the menu header shows for an unfinished self-test.
pub fn header_marker_line(marker: &Marker) -> Option<String> {
    match marker {
        Marker::Clear => None,
        Marker::Running => Some(
            "A previous self-test did not finish. It had not applied anything yet, so nothing needs undoing."
                .to_string(),
        ),
        Marker::Active(_) | Marker::NotRestored => Some(
            "A previous self-test did not leave your settings as they were. Choose \"Revert last change\" to put them back."
                .to_string(),
        ),
    }
}

/// The current user's Desktop folder, when Windows can tell.
pub fn desktop_dir() -> Option<PathBuf> {
    unsafe {
        let path = SHGetKnownFolderPath(&FOLDERID_Desktop, KF_FLAG_DEFAULT, None).ok()?;
        let text = path.to_string().ok();
        CoTaskMemFree(Some(path.as_ptr().cast_const().cast()));
        text.map(PathBuf::from)
    }
}

/// Writes `<stem>.md` and `<stem>.json` into every folder that works and
/// returns the paths written. It fails only when no folder worked.
pub fn write_report(
    report: &Report,
    markdown: &str,
    dirs: &[PathBuf],
    stem: &str,
) -> Result<Vec<PathBuf>, String> {
    let json = serde_json::to_string_pretty(report).map_err(|error| error.to_string())?;
    let mut written = Vec::new();
    let mut errors = Vec::new();

    for dir in dirs {
        let md = dir.join(format!("{stem}.md"));
        let js = dir.join(format!("{stem}.json"));
        let result = std::fs::create_dir_all(dir)
            .and_then(|()| std::fs::write(&md, markdown))
            .and_then(|()| std::fs::write(&js, &json));
        match result {
            Ok(()) => written.extend([md, js]),
            Err(error) => errors.push(format!("{}: {error}", dir.display())),
        }
    }

    if written.is_empty() { Err(errors.join("; ")) } else { Ok(written) }
}

/// Shows a folder in Explorer. Best effort.
pub fn open_folder(path: &Path) {
    let _ = Command::new("explorer.exe").arg(path).spawn();
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::selftest::diagnostics::Diagnostics;
    use crate::selftest::{Check, Report, Status};
    use crate::settings::backup::testing::temp_dir;

    fn report() -> Report {
        Report {
            schema: Report::SCHEMA,
            program_version: "0.1.0-alpha.1".into(),
            created: "20261117-150000".into(),
            total_millis: 10,
            all_restored: true,
            diagnostics: Diagnostics::default(),
            environment: vec![Check::new("admin", "Admin", Status::Pass, "yes", 1)],
            reading: vec![],
            cycles: vec![],
            closing: vec![],
        }
    }

    #[test]
    fn write_report_writes_both_files_and_valid_json() {
        let dir = temp_dir("selftest-files");
        let written = write_report(&report(), "# hello", &[dir.clone()], "stem").unwrap();

        assert_eq!(written.len(), 2);
        assert_eq!(std::fs::read_to_string(dir.join("stem.md")).unwrap(), "# hello");
        let json: serde_json::Value =
            serde_json::from_str(&std::fs::read_to_string(dir.join("stem.json")).unwrap()).unwrap();
        assert_eq!(json["schema"], 1);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn unwritable_report_dir_is_an_error_the_caller_can_handle() {
        let dir = temp_dir("selftest-files-bad");
        std::fs::create_dir_all(&dir).unwrap();
        // A folder cannot be created below a file.
        let file = dir.join("a-file");
        std::fs::write(&file, b"x").unwrap();
        let bad = file.join("reports");
        let good = dir.join("good");

        assert!(write_report(&report(), "md", &[bad.clone()], "stem").is_err());

        let written = write_report(&report(), "md", &[bad, good.clone()], "stem").unwrap();
        assert_eq!(written.len(), 2, "the good folder still gets both files");
        assert!(written.iter().all(|path| path.starts_with(&good)));
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn marker_text_round_trips() {
        for marker in [
            Marker::Running,
            Marker::NotRestored,
            Marker::Active(PathBuf::from(r"C:\ProgramData\NetShutUp\backups\20261117-1.json")),
        ] {
            let text = format_marker(&marker).unwrap();
            assert_eq!(parse_marker(&text), marker);
        }
        assert_eq!(format_marker(&Marker::Clear), None);
    }

    #[test]
    fn header_line_depends_on_the_marker() {
        assert_eq!(header_marker_line(&Marker::Clear), None);
        let running = header_marker_line(&Marker::Running).unwrap();
        assert!(running.contains("did not finish"), "{running}");
        assert!(running.contains("nothing needs undoing"), "{running}");
        for marker in [Marker::NotRestored, Marker::Active(PathBuf::from("x"))] {
            let line = header_marker_line(&marker).unwrap();
            assert!(line.contains("Revert last change"), "{line}");
        }
    }

    #[test]
    fn a_revert_clears_the_marker_unless_it_names_another_backup() {
        let mine = PathBuf::from("a.json");
        let other = PathBuf::from("b.json");
        assert_eq!(marker_after_revert(&Marker::Active(mine.clone()), &mine), Marker::Clear);
        assert_eq!(marker_after_revert(&Marker::Active(mine.clone()), &other), Marker::Active(mine));
        assert_eq!(marker_after_revert(&Marker::NotRestored, &other), Marker::Clear);
        assert_eq!(marker_after_revert(&Marker::Running, &other), Marker::Clear);
    }
}
