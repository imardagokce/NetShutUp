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

pub fn unfinished_marker_exists() -> bool {
    marker_path().exists()
}

/// The warning the menu header shows when a self-test was interrupted.
pub fn header_marker_line(exists: bool) -> Option<String> {
    exists.then(|| {
        "A previous self-test did not finish. Choose \"Revert last change\" to put your settings back."
            .to_string()
    })
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
    fn header_warns_about_an_unfinished_selftest() {
        assert_eq!(header_marker_line(false), None);
        let line = header_marker_line(true).unwrap();
        assert!(line.contains("did not finish"), "{line}");
        assert!(line.contains("Revert last change"), "{line}");
    }
}
