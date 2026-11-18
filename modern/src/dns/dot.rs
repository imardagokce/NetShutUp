//! Experimental DNS over TLS (DoT) through Windows' own encrypted DNS server
//! list. Entries are added with `netsh` (judged by exit code only) and read
//! back from the registry. Before any entry is touched the previous state is
//! saved in a sidecar file next to the backup, so a revert can put it back.

use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

use serde::{Deserialize, Serialize};
use windows::Win32::System::Registry::HKEY_LOCAL_MACHINE;

use crate::selftest::mask::mask_ip;
use crate::settings::provider::Provider;
use crate::settings::registry;

const WELL_KNOWN_KEY: &str = r"SYSTEM\CurrentControlSet\Services\Dnscache\Parameters\DohWellKnownServers";

/// Every value of one entry as `(name, "<type>:<data>")`, sorted by name.
pub type EntryValues = Vec<(String, String)>;

pub trait DotBackend {
    /// `None` when Windows has no entry for this server.
    fn read(&self, address: &str) -> Result<Option<EntryValues>, String>;
    fn add(&self, address: &str, host: &str) -> Result<(), String>;
    /// Puts the entry back as saved (`None` = there was no entry).
    fn restore(&self, address: &str, saved: &Option<EntryValues>) -> Result<(), String>;
}

#[derive(Serialize, Deserialize, PartialEq, Debug, Clone)]
pub struct Saved {
    pub entries: Vec<SavedEntry>,
}

#[derive(Serialize, Deserialize, PartialEq, Debug, Clone)]
pub struct SavedEntry {
    pub address: String,
    pub values: Option<EntryValues>,
}

/// `<stem>.dot.json` next to the backup file `<stem>.json`.
pub fn sidecar_path(backup: &Path) -> PathBuf {
    let stem = backup.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
    backup.with_file_name(format!("{stem}.dot.json"))
}

/// Writes the file, then reads it back and compares, so a bad write is noticed
/// before anything is changed.
pub fn save(path: &Path, saved: &Saved) -> Result<(), String> {
    let text = serde_json::to_string_pretty(saved).map_err(|e| format!("DoT backup: {e}"))?;
    std::fs::write(path, text).map_err(|e| format!("DoT backup could not be written: {e}"))?;
    if load(path)? != *saved {
        return Err("DoT backup differs from what was written".to_string());
    }
    Ok(())
}

pub fn load(path: &Path) -> Result<Saved, String> {
    let text = std::fs::read_to_string(path).map_err(|e| format!("DoT backup could not be read: {e}"))?;
    serde_json::from_str(&text).map_err(|e| format!("DoT backup is not valid: {e}"))
}

pub fn snapshot(backend: &dyn DotBackend, addresses: &[String]) -> Result<Saved, String> {
    let mut entries = Vec::new();
    for address in addresses {
        let values =
            backend.read(address).map_err(|e| format!("{}: {e}", mask_ip(address)))?;
        entries.push(SavedEntry { address: address.clone(), values });
    }
    Ok(Saved { entries })
}

/// Adds every server of the provider with the provider's DoT host.
pub fn add_provider(backend: &dyn DotBackend, provider: &Provider) -> Result<(), String> {
    for address in provider.servers {
        backend.add(address, provider.dot_host).map_err(|e| format!("{}: {e}", mask_ip(address)))?;
    }
    Ok(())
}

/// Restores every saved entry and checks each one by reading it back. Keeps
/// going after a failure; returns the error messages.
pub fn restore_all(backend: &dyn DotBackend, saved: &Saved) -> Vec<String> {
    let mut errors = Vec::new();
    for entry in &saved.entries {
        let masked = mask_ip(&entry.address);
        if let Err(e) = backend.restore(&entry.address, &entry.values) {
            errors.push(format!("{masked}: {e}"));
            continue;
        }
        match backend.read(&entry.address) {
            Ok(now) if now == entry.values => {}
            Ok(_) => errors.push(format!("{masked}: entry differs after restoring")),
            Err(e) => errors.push(format!("{masked}: {e}")),
        }
    }
    errors
}

/// Puts back what the sidecar of `backup_path` saved. Without a sidecar there
/// is nothing to do. The sidecar is renamed to `<stem>.dot.reverted.json` only
/// after every entry was restored and read back as saved; otherwise the errors
/// are returned and the file stays for another try.
pub fn restore_sidecar(backend: &dyn DotBackend, backup_path: &Path) -> Result<Vec<String>, String> {
    let sidecar = sidecar_path(backup_path);
    if !sidecar.exists() {
        return Ok(vec![]);
    }
    let saved = load(&sidecar)?;
    let errors = restore_all(backend, &saved);
    if errors.is_empty() {
        let name = sidecar.file_name().unwrap_or_default().to_string_lossy().into_owned();
        let stem = name.strip_suffix(".json").unwrap_or(&name);
        let target = sidecar.with_file_name(format!("{stem}.reverted.json"));
        std::fs::rename(&sidecar, &target)
            .map_err(|e| format!("the DoT backup could not be marked as used: {e}"))?;
    }
    Ok(errors)
}

pub fn netsh_add_args(address: &str, host: &str) -> Vec<String> {
    ["dns", "add", "encryption"]
        .iter()
        .map(|s| s.to_string())
        .chain([
            format!("server={address}"),
            format!("dothost={host}:853"),
            "autoupgrade=yes".to_string(),
            "udpfallback=no".to_string(),
        ])
        .collect()
}

pub fn netsh_delete_args(address: &str) -> Vec<String> {
    ["dns", "delete", "encryption"]
        .iter()
        .map(|s| s.to_string())
        .chain([format!("server={address}")])
        .collect()
}

pub fn netsh_add_doh_args(address: &str, template: &str) -> Vec<String> {
    ["dns", "add", "encryption"]
        .iter()
        .map(|s| s.to_string())
        .chain([format!("server={address}"), format!("dohtemplate={template}")])
        .collect()
}

/// The real Windows list; `add` and `restore` run `netsh` for real, so tests only compile them.
pub struct SystemDotBackend;

fn entry_key(address: &str) -> String {
    format!(r"{WELL_KNOWN_KEY}\{address}")
}

fn netsh(args: &[String]) -> Result<(), String> {
    let status = Command::new("netsh")
        .args(args)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map_err(|e| format!("netsh could not be started: {e}"))?;
    if status.success() {
        Ok(())
    } else {
        Err(format!("netsh failed (exit code {})", status.code().map_or("none".to_string(), |c| c.to_string())))
    }
}

impl DotBackend for SystemDotBackend {
    fn read(&self, address: &str) -> Result<Option<EntryValues>, String> {
        let key = entry_key(address);
        if !registry::key_exists(HKEY_LOCAL_MACHINE, &key) {
            return Ok(None);
        }
        registry::values(HKEY_LOCAL_MACHINE, &key)
            .map(Some)
            .map_err(|code| format!("registry error {code}"))
    }

    fn add(&self, address: &str, host: &str) -> Result<(), String> {
        netsh(&netsh_add_args(address, host))
    }

    fn restore(&self, address: &str, saved: &Option<EntryValues>) -> Result<(), String> {
        if registry::key_exists(HKEY_LOCAL_MACHINE, &entry_key(address)) {
            netsh(&netsh_delete_args(address))?;
        }
        let template = saved.as_ref().and_then(|values| {
            values
                .iter()
                .find(|(name, _)| name == "Template")
                .and_then(|(_, data)| data.strip_prefix("REG_SZ:"))
        });
        match template {
            Some(template) => netsh(&netsh_add_doh_args(address, template)),
            None => Ok(()),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::settings::backup::testing::temp_dir;
    use crate::settings::provider;
    use std::cell::RefCell;
    use std::collections::HashMap;

    #[derive(Default)]
    struct Fake {
        entries: RefCell<HashMap<String, EntryValues>>,
        adds: RefCell<Vec<(String, String)>>,
        restore_does_nothing: bool,
    }

    fn values(text: &str) -> EntryValues {
        vec![("Template".to_string(), format!("REG_SZ:{text}"))]
    }

    impl DotBackend for Fake {
        fn read(&self, address: &str) -> Result<Option<EntryValues>, String> {
            Ok(self.entries.borrow().get(address).cloned())
        }
        fn add(&self, address: &str, host: &str) -> Result<(), String> {
            self.adds.borrow_mut().push((address.to_string(), host.to_string()));
            self.entries
                .borrow_mut()
                .insert(address.to_string(), vec![("DotHost".to_string(), format!("REG_SZ:{host}:853"))]);
            Ok(())
        }
        fn restore(&self, address: &str, saved: &Option<EntryValues>) -> Result<(), String> {
            if self.restore_does_nothing {
                return Ok(());
            }
            match saved {
                Some(v) => self.entries.borrow_mut().insert(address.to_string(), v.clone()),
                None => self.entries.borrow_mut().remove(address),
            };
            Ok(())
        }
    }

    fn strings(list: &[&str]) -> Vec<String> {
        list.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn sidecar_path_replaces_the_extension() {
        let side = |name: &str| sidecar_path(Path::new(name)).to_string_lossy().into_owned();
        assert_eq!(side("20261117-120000.json"), "20261117-120000.dot.json");
        assert_eq!(side("20261117-120000-2.json"), "20261117-120000-2.dot.json");
        assert_eq!(side("20261117-120000.reverted.json"), "20261117-120000.reverted.dot.json");
    }

    #[test]
    fn saved_round_trips_through_the_sidecar_file() {
        let dir = temp_dir("dot-sidecar");
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("b.dot.json");
        let saved = Saved {
            entries: vec![
                SavedEntry { address: "1.1.1.1".into(), values: Some(values("https://x")) },
                SavedEntry { address: "1.0.0.1".into(), values: None },
            ],
        };
        save(&path, &saved).unwrap();
        assert_eq!(load(&path), Ok(saved));
        assert!(load(&dir.join("missing.json")).is_err());
    }

    #[test]
    fn snapshot_records_absent_entries_as_none() {
        let fake = Fake::default();
        fake.entries.borrow_mut().insert("1.1.1.1".into(), values("a"));
        let saved = snapshot(&fake, &strings(&["1.1.1.1", "8.8.8.8"])).unwrap();
        assert_eq!(
            saved.entries,
            vec![
                SavedEntry { address: "1.1.1.1".into(), values: Some(values("a")) },
                SavedEntry { address: "8.8.8.8".into(), values: None },
            ]
        );
    }

    #[test]
    fn add_provider_adds_both_servers_with_the_providers_host() {
        let fake = Fake::default();
        add_provider(&fake, provider::find("cloudflare").unwrap()).unwrap();
        assert_eq!(
            *fake.adds.borrow(),
            vec![
                ("1.1.1.1".to_string(), "one.one.one.one".to_string()),
                ("1.0.0.1".to_string(), "one.one.one.one".to_string()),
            ]
        );
    }

    #[test]
    fn restore_all_puts_back_changed_and_removed_entries() {
        let fake = Fake::default();
        fake.entries.borrow_mut().insert("1.1.1.1".into(), values("a"));
        let saved = snapshot(&fake, &strings(&["1.1.1.1", "1.0.0.1"])).unwrap();
        add_provider(&fake, provider::find("cloudflare").unwrap()).unwrap();

        assert_eq!(restore_all(&fake, &saved), Vec::<String>::new());
        assert_eq!(fake.read("1.1.1.1"), Ok(Some(values("a"))));
        assert_eq!(fake.read("1.0.0.1"), Ok(None));
    }

    #[test]
    fn restore_all_reports_an_entry_that_still_differs() {
        let fake = Fake { restore_does_nothing: true, ..Fake::default() };
        let saved = Saved {
            entries: vec![SavedEntry { address: "192.168.1.5".into(), values: Some(values("a")) }],
        };

        let errors = restore_all(&fake, &saved);
        assert_eq!(errors, ["192.168.x.x: entry differs after restoring"]);
    }

    fn backup_with_saved(dir: &Path, fake: &Fake) -> PathBuf {
        std::fs::create_dir_all(dir).unwrap();
        let backup = dir.join("20261117-120000.json");
        let saved = snapshot(fake, &strings(&["1.1.1.1", "1.0.0.1"])).unwrap();
        save(&sidecar_path(&backup), &saved).unwrap();
        backup
    }

    #[test]
    fn restore_sidecar_puts_entries_back_and_renames_the_file() {
        let dir = temp_dir("dot-restore-sidecar");
        let fake = Fake::default();
        fake.entries.borrow_mut().insert("1.1.1.1".into(), values("a"));
        let backup = backup_with_saved(&dir, &fake);
        add_provider(&fake, provider::find("cloudflare").unwrap()).unwrap();

        assert_eq!(restore_sidecar(&fake, &backup), Ok(vec![]));

        assert_eq!(fake.read("1.1.1.1"), Ok(Some(values("a"))));
        assert_eq!(fake.read("1.0.0.1"), Ok(None));
        assert!(!sidecar_path(&backup).exists());
        assert!(dir.join("20261117-120000.dot.reverted.json").exists());
    }

    #[test]
    fn restore_sidecar_without_a_file_is_a_no_op() {
        let dir = temp_dir("dot-restore-none");
        std::fs::create_dir_all(&dir).unwrap();
        let fake = Fake::default();
        assert_eq!(restore_sidecar(&fake, &dir.join("x.json")), Ok(vec![]));
    }

    #[test]
    fn restore_sidecar_failure_keeps_the_file() {
        let dir = temp_dir("dot-restore-fail");
        let fake = Fake { restore_does_nothing: true, ..Fake::default() };
        fake.entries.borrow_mut().insert("1.1.1.1".into(), values("a"));
        let backup = backup_with_saved(&dir, &fake);
        add_provider(&fake, provider::find("cloudflare").unwrap()).unwrap();

        let errors = restore_sidecar(&fake, &backup).unwrap();

        assert!(!errors.is_empty());
        assert!(sidecar_path(&backup).exists());
        assert!(!dir.join("20261117-120000.dot.reverted.json").exists());
    }

    #[test]
    fn netsh_arguments_match_the_spec() {
        assert_eq!(
            netsh_add_args("1.1.1.1", "one.one.one.one"),
            strings(&[
                "dns", "add", "encryption", "server=1.1.1.1", "dothost=one.one.one.one:853",
                "autoupgrade=yes", "udpfallback=no"
            ])
        );
        assert_eq!(
            netsh_delete_args("1.1.1.1"),
            strings(&["dns", "delete", "encryption", "server=1.1.1.1"])
        );
        assert_eq!(
            netsh_add_doh_args("1.1.1.1", "https://t/dns-query"),
            strings(&["dns", "add", "encryption", "server=1.1.1.1", "dohtemplate=https://t/dns-query"])
        );
    }

    #[test]
    #[ignore = "reads the real registry; run by hand"]
    fn system_backend_reads_an_entry() {
        println!("{:?}", SystemDotBackend.read("1.1.1.1"));
    }
}
