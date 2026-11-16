//! Backup files: the previous state of every setting a change touched,
//! written to disk before anything is modified.

use std::fs;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};
use windows::Win32::System::SystemInformation::GetLocalTime;

use super::reader::State;

pub const FORMAT: u32 = 1;

const EXTENSION: &str = ".json";
const REVERTED_EXTENSION: &str = ".reverted.json";

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Backup {
    pub format: u32,
    /// Local time the backup was taken, `YYYYMMDD-HHMMSS`.
    pub created: String,
    /// What the change was, e.g. the profile name.
    pub label: String,
    pub entries: Vec<Entry>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Entry {
    /// Catalog id of the setting.
    pub id: String,
    pub adapter: Option<String>,
    pub previous: State,
    /// Container levels (registry keys) that did not exist before the change
    /// and are removed again on revert if they end up empty.
    pub missing_levels: u32,
}

/// Where backups are stored on a real system.
pub fn default_dir() -> PathBuf {
    let base = std::env::var_os("ProgramData").unwrap_or_else(|| r"C:\ProgramData".into());
    PathBuf::from(base).join("NetShutUp").join("backups")
}

/// Local time as `YYYYMMDD-HHMMSS`; sorts chronologically as text.
pub fn timestamp_now() -> String {
    let now = unsafe { GetLocalTime() };
    format!(
        "{:04}{:02}{:02}-{:02}{:02}{:02}",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond
    )
}

/// Writes the backup and verifies it by reading it back. Never overwrites an
/// existing backup.
pub fn save(dir: &Path, backup: &Backup) -> Result<PathBuf, String> {
    fs::create_dir_all(dir).map_err(|error| format!("cannot create {}: {error}", dir.display()))?;

    let mut path = dir.join(format!("{}{EXTENSION}", backup.created));
    let mut attempt = 2;
    while path.exists() || reverted_name(&path).exists() {
        path = dir.join(format!("{}-{attempt}{EXTENSION}", backup.created));
        attempt += 1;
    }

    let json = serde_json::to_string_pretty(backup).map_err(|error| error.to_string())?;
    fs::write(&path, json).map_err(|error| format!("cannot write {}: {error}", path.display()))?;

    if load(&path)? != *backup {
        let _ = fs::remove_file(&path);
        return Err(format!("backup {} did not verify after writing", path.display()));
    }
    Ok(path)
}

pub fn load(path: &Path) -> Result<Backup, String> {
    let json = fs::read_to_string(path)
        .map_err(|error| format!("cannot read {}: {error}", path.display()))?;
    let backup: Backup = serde_json::from_str(&json)
        .map_err(|error| format!("{} is not a valid backup: {error}", path.display()))?;
    if backup.format != FORMAT {
        return Err(format!("{} has unsupported format {}", path.display(), backup.format));
    }
    Ok(backup)
}

/// The newest backup that has not been reverted yet.
pub fn latest(dir: &Path) -> Option<PathBuf> {
    fs::read_dir(dir)
        .ok()?
        .filter_map(|entry| Some(entry.ok()?.path()))
        .filter(|path| {
            let name = path.file_name().unwrap_or_default().to_string_lossy();
            name.ends_with(EXTENSION) && !name.ends_with(REVERTED_EXTENSION)
        })
        .max()
}

/// Keeps the file but takes it out of `latest`, so the same change is not
/// reverted twice.
pub fn mark_reverted(path: &Path) -> Result<PathBuf, String> {
    let target = reverted_name(path);
    fs::rename(path, &target)
        .map_err(|error| format!("cannot rename {}: {error}", path.display()))?;
    Ok(target)
}

fn reverted_name(path: &Path) -> PathBuf {
    let name = path.file_name().unwrap_or_default().to_string_lossy();
    let stem = name.strip_suffix(EXTENSION).unwrap_or(&name);
    path.with_file_name(format!("{stem}{REVERTED_EXTENSION}"))
}

#[cfg(test)]
pub mod testing {
    use std::path::PathBuf;

    /// A fresh, empty temp directory for one test.
    pub fn temp_dir(name: &str) -> PathBuf {
        let dir =
            std::env::temp_dir().join(format!("netshutup-test-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        dir
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::settings::reader::Value;

    fn sample(created: &str) -> Backup {
        Backup {
            format: FORMAT,
            created: created.to_string(),
            label: "test".to_string(),
            entries: vec![
                Entry {
                    id: "mm.network_throttling".to_string(),
                    adapter: None,
                    previous: State::Value(Value::Dword(10)),
                    missing_levels: 0,
                },
                Entry {
                    id: "dns.static_servers".to_string(),
                    adapter: Some("{GUID}".to_string()),
                    previous: State::Value(Value::Text(String::new())),
                    missing_levels: 0,
                },
                Entry {
                    id: "tcp.nagle.no_delay".to_string(),
                    adapter: Some("{GUID}".to_string()),
                    previous: State::NotSet,
                    missing_levels: 2,
                },
            ],
        }
    }

    #[test]
    fn save_load_round_trip() {
        let dir = testing::temp_dir("backup-roundtrip");
        let backup = sample("20261116-120000");
        let path = save(&dir, &backup).unwrap();
        assert_eq!(load(&path).unwrap(), backup);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn save_never_overwrites() {
        let dir = testing::temp_dir("backup-no-overwrite");
        let first = save(&dir, &sample("20261116-120000")).unwrap();
        let second = save(&dir, &sample("20261116-120000")).unwrap();
        assert_ne!(first, second);
        assert!(first.exists() && second.exists());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn latest_skips_reverted_backups() {
        let dir = testing::temp_dir("backup-latest");
        assert_eq!(latest(&dir), None);

        let older = save(&dir, &sample("20261116-120000")).unwrap();
        let newer = save(&dir, &sample("20261116-130000")).unwrap();
        assert_eq!(latest(&dir), Some(newer.clone()));

        let renamed = mark_reverted(&newer).unwrap();
        assert!(renamed.exists() && !newer.exists());
        assert_eq!(latest(&dir), Some(older.clone()));

        mark_reverted(&older).unwrap();
        assert_eq!(latest(&dir), None);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn rejects_garbage_and_unknown_formats() {
        let dir = testing::temp_dir("backup-invalid");
        fs::create_dir_all(&dir).unwrap();

        let garbage = dir.join("garbage.json");
        fs::write(&garbage, "not json").unwrap();
        assert!(load(&garbage).is_err());

        let mut future = sample("20261116-120000");
        future.format = FORMAT + 1;
        let path = dir.join("future.json");
        fs::write(&path, serde_json::to_string(&future).unwrap()).unwrap();
        assert!(load(&path).is_err());
        let _ = fs::remove_dir_all(&dir);
    }
}
