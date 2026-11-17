//! Applies setting changes and reverts them.
//!
//! Rules this module enforces:
//! - Nothing is modified until a verified backup of every affected setting
//!   is on disk.
//! - A setting whose current state cannot be read is never modified.
//! - If any write fails, everything written so far is undone.

use std::fs;
use std::path::{Path, PathBuf};

use super::backup::{self, Backup, Entry};
use super::catalog::{self, Setting};
use super::reader::{self, State};
use super::store::Store;

/// A setting and the state it should end up in.
#[derive(Clone)]
pub struct Target {
    pub setting: &'static Setting,
    pub adapter: Option<String>,
    /// `State::NotSet` means "remove the value".
    pub desired: State,
}

#[derive(Clone)]
pub struct PlannedChange {
    pub target: Target,
    pub current: State,
}

impl PlannedChange {
    /// Whether the setting differs from the desired state.
    pub fn is_needed(&self) -> bool {
        !reader::matches(&self.target.desired, &self.current)
    }
}

#[derive(Debug, PartialEq, Eq)]
pub enum ApplyError {
    /// Refused before anything was modified.
    Refused(String),
    /// A write failed and the changes made so far were undone. `leftovers`
    /// lists settings that could not be restored; when it is not empty the
    /// backup file is kept so a revert can be retried.
    RolledBack { cause: String, leftovers: Vec<String> },
}

/// Reads the current state of every target. Modifies nothing.
pub fn plan(store: &dyn Store, targets: Vec<Target>) -> Vec<PlannedChange> {
    targets
        .into_iter()
        .map(|target| {
            let current = store.read(target.setting, target.adapter.as_deref());
            PlannedChange { target, current }
        })
        .collect()
}

/// Why a plan cannot be applied, if it cannot. `apply` refuses such a plan;
/// call this first to tell the user before asking for confirmation.
pub fn check(plan: &[PlannedChange]) -> Result<(), String> {
    for change in plan.iter().filter(|change| change.is_needed()) {
        let id = change.target.setting.id;
        if !change.target.setting.is_writable() {
            return Err(format!("{id} is read-only"));
        }
        if let State::Unavailable(reason) = &change.current {
            return Err(format!("{id} cannot be backed up because it cannot be read ({reason})"));
        }
        if matches!(change.target.desired, State::Unavailable(_)) {
            return Err(format!("{id} has no valid target value"));
        }
    }
    Ok(())
}

/// Applies the needed changes of a plan. Returns the backup file, or `None`
/// when every setting already matched and nothing was touched.
pub fn apply(
    store: &dyn Store,
    backup_dir: &Path,
    label: &str,
    timestamp: &str,
    plan: &[PlannedChange],
) -> Result<Option<PathBuf>, ApplyError> {
    check(plan).map_err(ApplyError::Refused)?;

    let changes: Vec<&PlannedChange> = plan.iter().filter(|change| change.is_needed()).collect();
    if changes.is_empty() {
        return Ok(None);
    }

    let backup = Backup {
        format: backup::FORMAT,
        created: timestamp.to_string(),
        label: label.to_string(),
        entries: changes
            .iter()
            .map(|change| {
                let adapter = change.target.adapter.as_deref();
                Entry {
                    id: change.target.setting.id.to_string(),
                    adapter: change.target.adapter.clone(),
                    previous: change.current.clone(),
                    missing_levels: store.missing_container_levels(change.target.setting, adapter),
                }
            })
            .collect(),
    };
    let path = backup::save(backup_dir, &backup).map_err(ApplyError::Refused)?;

    for (index, change) in changes.iter().enumerate() {
        let Target { setting, adapter, desired } = &change.target;
        let adapter = adapter.as_deref();

        let written = store.write(setting, adapter, desired).and_then(|()| {
            let actual = store.read(setting, adapter);
            if reader::matches(desired, &actual) {
                Ok(())
            } else {
                Err(format!("value did not take effect (now {actual:?})"))
            }
        });

        if let Err(reason) = written {
            // Includes the failed entry: its write may have partly happened.
            let leftovers = restore(store, &backup.entries[..=index]);
            if leftovers.is_empty() {
                let _ = fs::remove_file(&path);
            }
            return Err(ApplyError::RolledBack {
                cause: format!("{}: {reason}", setting.id),
                leftovers,
            });
        }
    }

    Ok(Some(path))
}

/// Restores every setting in a backup to its previous state. Keeps going
/// after a failure; returns one message per setting that was not restored.
pub fn revert(store: &dyn Store, backup: &Backup) -> Vec<String> {
    restore(store, &backup.entries)
}

fn restore(store: &dyn Store, entries: &[Entry]) -> Vec<String> {
    let mut errors = Vec::new();

    for entry in entries.iter().rev() {
        let Some(setting) = catalog::find(&entry.id) else {
            errors.push(format!("{}: unknown setting", entry.id));
            continue;
        };
        let adapter = entry.adapter.as_deref();

        if let Err(reason) = store.write(setting, adapter, &entry.previous) {
            errors.push(format!("{}: {reason}", entry.id));
            continue;
        }
        if entry.previous == State::NotSet {
            store.remove_empty_containers(setting, adapter, entry.missing_levels);
        }

        let actual = store.read(setting, adapter);
        if actual != entry.previous && !reader::matches(&entry.previous, &actual) {
            errors.push(format!("{}: still {actual:?} after restoring", entry.id));
        }
    }

    errors
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::settings::backup::testing::temp_dir;
    use crate::settings::reader::{DnsServer, Doh, Value};
    use crate::settings::registry::{self, sandbox};
    use crate::settings::store::SystemStore;
    use crate::settings::store::testing::FakeStore;
    use std::cell::Cell;
    use windows::Win32::System::Registry::HKEY_CURRENT_USER as HKCU;

    const ADAPTER: &str = "{TEST-ADAPTER}";

    fn dword(number: u32) -> State {
        State::Value(Value::Dword(number))
    }

    fn text(text: &str) -> State {
        State::Value(Value::Text(text.to_string()))
    }

    fn target(id: &str, adapter: Option<&str>, desired: State) -> Target {
        Target {
            setting: catalog::find(id).unwrap(),
            adapter: adapter.map(str::to_string),
            desired,
        }
    }

    /// Throttling off, Nagle off, DNS to Cloudflare.
    fn sample_targets() -> Vec<Target> {
        vec![
            target("mm.network_throttling", None, dword(0xFFFF_FFFF)),
            target("tcp.nagle.no_delay", Some(ADAPTER), dword(1)),
            target("dns.static_servers", Some(ADAPTER), text("1.1.1.1,1.0.0.1")),
        ]
    }

    #[test]
    fn apply_then_revert_restores_exactly() {
        let dir = temp_dir("apply-revert");
        let store = FakeStore::default();
        store.seed("mm.network_throttling", None, dword(10));
        store.seed("dns.static_servers", Some(ADAPTER), text(""));
        let before = store.snapshot();

        let plan = plan(&store, sample_targets());
        let path = apply(&store, &dir, "test", "20261116-120000", &plan).unwrap().unwrap();

        let throttling = catalog::find("mm.network_throttling").unwrap();
        let no_delay = catalog::find("tcp.nagle.no_delay").unwrap();
        assert_eq!(store.read(throttling, None), dword(0xFFFF_FFFF));
        assert_eq!(store.read(no_delay, Some(ADAPTER)), dword(1));

        let saved = backup::load(&path).unwrap();
        assert_eq!(saved.entries.len(), 3);
        assert_eq!(saved.entries[1].previous, State::NotSet);

        assert_eq!(revert(&store, &saved), Vec::<String>::new());
        // "Not set" is restored as not set, and an empty string as an empty
        // string: the store is identical to how it started.
        assert_eq!(store.snapshot(), before);
        let _ = fs::remove_dir_all(&dir);
    }

    fn cloudflare_dns() -> State {
        let doh = |flags| Doh { template: "https://cloudflare-dns.com/dns-query".into(), flags };
        State::Value(Value::Dns(vec![
            DnsServer { address: "1.1.1.1".into(), doh: Some(doh(2)) },
            DnsServer { address: "1.0.0.1".into(), doh: Some(doh(2)) },
        ]))
    }

    #[test]
    fn apply_then_revert_restores_dns_exactly() {
        let dir = temp_dir("apply-revert-dns");
        let store = FakeStore::default();
        // Adapter A uses DHCP (not set); adapter B has a static server with
        // DoH flags this program does not know (5): both must come back exactly.
        let other = "{OTHER-ADAPTER}";
        let static_dns = State::Value(Value::Dns(vec![DnsServer {
            address: "9.9.9.9".into(),
            doh: Some(Doh { template: "https://dns.quad9.net/dns-query".into(), flags: 5 }),
        }]));
        store.seed("dns.resolver", Some(other), static_dns.clone());
        let before = store.snapshot();

        let targets = vec![
            target("dns.resolver", Some(ADAPTER), cloudflare_dns()),
            target("dns.resolver", Some(other), cloudflare_dns()),
        ];
        let plan = plan(&store, targets);
        let path = apply(&store, &dir, "test", "20261117-120000", &plan).unwrap().unwrap();

        let dns = catalog::find("dns.resolver").unwrap();
        assert_eq!(store.read(dns, Some(ADAPTER)), cloudflare_dns());
        assert_eq!(store.read(dns, Some(other)), cloudflare_dns());

        let saved = backup::load(&path).unwrap();
        assert_eq!(saved.entries[0].previous, State::NotSet);
        assert_eq!(saved.entries[1].previous, static_dns);

        assert_eq!(revert(&store, &saved), Vec::<String>::new());
        assert_eq!(store.snapshot(), before);
        assert_eq!(store.read(dns, Some(ADAPTER)), State::NotSet);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn matching_settings_are_left_alone() {
        let dir = temp_dir("apply-noop");
        let store = FakeStore::default();
        store.seed("mm.network_throttling", None, dword(0xFFFF_FFFF));
        store.seed("tcp.nagle.no_delay", Some(ADAPTER), dword(1));
        // Same servers, different separator: still a match.
        store.seed("dns.static_servers", Some(ADAPTER), text("1.1.1.1 1.0.0.1"));

        let plan = plan(&store, sample_targets());
        assert!(plan.iter().all(|change| !change.is_needed()));
        assert_eq!(apply(&store, &dir, "test", "20261116-120000", &plan), Ok(None));
        assert_eq!(store.writes.get(), 0);
        assert!(!dir.exists(), "no backup is written when nothing changes");
    }

    #[test]
    fn only_differing_settings_are_backed_up_and_written() {
        let dir = temp_dir("apply-partial");
        let store = FakeStore::default();
        store.seed("tcp.nagle.no_delay", Some(ADAPTER), dword(1));

        let plan = plan(&store, sample_targets());
        let path = apply(&store, &dir, "test", "20261116-120000", &plan).unwrap().unwrap();

        let ids: Vec<String> =
            backup::load(&path).unwrap().entries.into_iter().map(|entry| entry.id).collect();
        assert_eq!(ids, ["mm.network_throttling", "dns.static_servers"]);
        assert_eq!(store.writes.get(), 2);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn failed_write_rolls_everything_back() {
        let dir = temp_dir("apply-rollback");
        let store = FakeStore {
            fail_id: Some("dns.static_servers"),
            failures_left: Cell::new(1),
            ..Default::default()
        };
        store.seed("mm.network_throttling", None, dword(10));
        let before = store.snapshot();

        let plan = plan(&store, sample_targets());
        let error = apply(&store, &dir, "test", "20261116-120000", &plan).unwrap_err();

        assert!(matches!(
            &error,
            ApplyError::RolledBack { cause, leftovers }
                if cause.starts_with("dns.static_servers") && leftovers.is_empty()
        ));
        assert_eq!(store.snapshot(), before);
        assert_eq!(backup::latest(&dir), None, "a fully undone change leaves no backup");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn incomplete_rollback_keeps_the_backup() {
        let dir = temp_dir("apply-leftovers");
        // The DNS write fails, and so does restoring it afterwards.
        let store = FakeStore {
            fail_id: Some("dns.static_servers"),
            failures_left: Cell::new(2),
            ..Default::default()
        };

        let plan = plan(&store, sample_targets());
        let error = apply(&store, &dir, "test", "20261116-120000", &plan).unwrap_err();

        let ApplyError::RolledBack { leftovers, .. } = error else {
            panic!("expected a rollback");
        };
        assert_eq!(leftovers.len(), 1);
        assert!(leftovers[0].starts_with("dns.static_servers"));
        assert!(backup::latest(&dir).is_some(), "backup stays so revert can be retried");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn unreadable_setting_blocks_the_whole_change() {
        let dir = temp_dir("apply-unreadable");
        let store =
            FakeStore { unreadable_id: Some("tcp.nagle.no_delay"), ..Default::default() };

        let plan = plan(&store, sample_targets());
        let error = apply(&store, &dir, "test", "20261116-120000", &plan).unwrap_err();

        assert!(matches!(error, ApplyError::Refused(_)));
        assert_eq!(store.writes.get(), 0);
        assert!(!dir.exists());
    }

    #[test]
    fn read_only_setting_blocks_the_whole_change() {
        let dir = temp_dir("apply-read-only");
        let store = FakeStore::default();
        let mut targets = sample_targets();
        targets.push(target("tcp.autotuning", None, dword(3)));

        let plan = plan(&store, targets);
        let error = apply(&store, &dir, "test", "20261116-120000", &plan).unwrap_err();

        assert_eq!(error, ApplyError::Refused("tcp.autotuning is read-only".to_string()));
        assert_eq!(store.writes.get(), 0);
    }

    #[test]
    fn revert_reports_unknown_settings_and_continues() {
        let store = FakeStore::default();
        store.seed("mm.network_throttling", None, dword(0xFFFF_FFFF));
        let backup = Backup {
            format: backup::FORMAT,
            created: "20261116-120000".to_string(),
            label: "test".to_string(),
            entries: vec![
                Entry {
                    id: "mm.network_throttling".to_string(),
                    adapter: None,
                    previous: dword(10),
                    missing_levels: 0,
                },
                Entry {
                    id: "removed.in.a.later.version".to_string(),
                    adapter: None,
                    previous: State::NotSet,
                    missing_levels: 0,
                },
            ],
        };

        let errors = revert(&store, &backup);
        assert_eq!(errors, ["removed.in.a.later.version: unknown setting"]);
        let throttling = catalog::find("mm.network_throttling").unwrap();
        assert_eq!(store.read(throttling, None), dword(10));
    }

    /// The real registry code, end to end, inside the HKCU test sandbox.
    #[test]
    fn real_registry_apply_and_revert_in_sandbox() {
        const NAME: &str = "apply-e2e";
        sandbox::wipe(NAME);
        let dir = temp_dir(NAME);
        let store = SystemStore::sandbox(NAME);

        let throttling = catalog::find("mm.network_throttling").unwrap();
        let no_delay = catalog::find("tcp.nagle.no_delay").unwrap();
        let dns = catalog::find("dns.static_servers").unwrap();

        // Before: throttling has a value; the adapter key does not exist.
        store.write(throttling, None, &dword(10)).unwrap();
        let adapter_key = sandbox::path(&format!(
            r"{NAME}\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters\Interfaces\{ADAPTER}"
        ));
        assert!(!registry::key_exists(HKCU, &adapter_key));

        let plan = plan(&store, sample_targets());
        let path = apply(&store, &dir, "test", "20261116-120000", &plan).unwrap().unwrap();

        assert_eq!(store.read(throttling, None), dword(0xFFFF_FFFF));
        assert_eq!(store.read(no_delay, Some(ADAPTER)), dword(1));
        assert_eq!(store.read(dns, Some(ADAPTER)), text("1.1.1.1,1.0.0.1"));
        assert!(registry::key_exists(HKCU, &adapter_key));

        let saved = backup::load(&path).unwrap();
        assert_eq!(revert(&store, &saved), Vec::<String>::new());

        assert_eq!(store.read(throttling, None), dword(10));
        assert_eq!(store.read(no_delay, Some(ADAPTER)), State::NotSet);
        assert_eq!(store.read(dns, Some(ADAPTER)), State::NotSet);
        // Keys the change created are gone again, up to the first key that
        // existed before.
        assert!(!registry::key_exists(HKCU, &adapter_key));
        assert!(!registry::key_exists(HKCU, &sandbox::path(&format!(r"{NAME}\SYSTEM"))));

        sandbox::wipe(NAME);
        let _ = fs::remove_dir_all(&dir);
    }
}
