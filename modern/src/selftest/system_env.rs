//! The real computer behind `Env`: the real settings, the Windows resolver,
//! the real adapters and the backup folder.

use std::cell::Cell;
use std::path::PathBuf;
use std::process::{Command, Stdio};

use crate::adapters;
use crate::dns::Resolver;
use crate::dns::resolver::SystemResolver;
use crate::elevation;
use crate::settings::backup;
use crate::settings::registry;
use crate::settings::store::{Store, SystemStore};
use crate::version;

use windows::Win32::System::Registry::HKEY_LOCAL_MACHINE;
use windows::Win32::System::Restore::{
    BEGIN_SYSTEM_CHANGE, END_SYSTEM_CHANGE, MODIFY_SETTINGS, RESTOREPOINTINFOW, SRSetRestorePointW,
    STATEMGRSTATUS,
};

use super::files::{self, Marker};
use super::mask::mask_guid;
use super::runner::{Env, EnvAdapter};
use super::safety;

pub struct SystemEnv {
    store: SystemStore,
    resolver: SystemResolver,
    /// The sequence number of the restore point that was begun, if one was.
    restore_sequence: Cell<Option<i64>>,
}

impl SystemEnv {
    pub fn new() -> Self {
        Self { store: SystemStore::system(), resolver: SystemResolver, restore_sequence: Cell::new(None) }
    }
}

impl Env for SystemEnv {
    fn store(&self) -> &dyn Store {
        &self.store
    }

    fn resolver(&self) -> &dyn Resolver {
        &self.resolver
    }

    fn adapters(&self) -> Vec<EnvAdapter> {
        adapters::get_network_adapters()
            .unwrap_or_default()
            .into_iter()
            .filter(|adapter| adapter.takes_profile_settings)
            .map(|adapter| EnvAdapter { guid: adapter.guid })
            .collect()
    }

    fn windows_build(&self) -> u32 {
        version::get_windows_version().2
    }

    fn elevated(&self) -> bool {
        elevation::is_elevated()
    }

    fn backup_dir(&self) -> PathBuf {
        backup::default_dir()
    }

    fn timestamp(&self) -> String {
        backup::timestamp_now()
    }

    fn mark(&self, marker: Marker) {
        files::write_marker(&marker);
    }

    fn doh_entries(&self) -> Vec<String> {
        let mut entries = Vec::new();
        for adapter in self.adapters() {
            let key = format!(
                r"SYSTEM\CurrentControlSet\Services\Dnscache\InterfaceSpecificParameters\{}\DohInterfaceSettings\Doh",
                adapter.guid
            );
            for server in registry::subkeys(HKEY_LOCAL_MACHINE, &key).unwrap_or_default() {
                entries.push(format!("{}/{server}", mask_guid(&adapter.guid)));
            }
        }
        entries.sort();
        entries
    }

    fn restore_point(&self) -> Result<String, String> {
        let mut info = RESTOREPOINTINFOW::default();
        info.dwEventType = BEGIN_SYSTEM_CHANGE;
        info.dwRestorePtType = MODIFY_SETTINGS;
        let mut description = [0u16; 256];
        for (slot, unit) in description.iter_mut().zip("NetShutUp self-test".encode_utf16()) {
            *slot = unit;
        }
        info.szDescription = description;
        let mut status = STATEMGRSTATUS::default();
        let ok = unsafe { SRSetRestorePointW(&info, &mut status) }.as_bool();
        let code = status.nStatus.0;
        if !ok || code != 0 {
            return Err(if code == 1058 {
                "System Protection is off (the restore service is disabled)".to_string()
            } else {
                format!(
                    "System Protection is off or a restore point was made in the last 24 hours (status {code})"
                )
            });
        }
        let sequence = status.llSequenceNumber;
        self.restore_sequence.set(Some(sequence));
        Ok("created".to_string())
    }

    fn finish_restore_point(&self) {
        let Some(sequence) = self.restore_sequence.take() else { return };
        let mut info = RESTOREPOINTINFOW::default();
        info.dwEventType = END_SYSTEM_CHANGE;
        info.dwRestorePtType = MODIFY_SETTINGS;
        info.llSequenceNumber = sequence;
        let mut status = STATEMGRSTATUS::default();
        let _ = unsafe { SRSetRestorePointW(&info, &mut status) };
    }

    fn export_registry(&self) -> Result<Vec<String>, String> {
        let base = std::env::var_os("ProgramData").unwrap_or_else(|| r"C:\ProgramData".into());
        let dir = PathBuf::from(base).join("NetShutUp").join("reg-backups").join(self.timestamp());
        std::fs::create_dir_all(&dir).map_err(|error| format!("cannot create the export folder: {error}"))?;
        let mut written = Vec::new();
        for key in safety::REGISTRY_KEYS {
            let subkey = key.strip_prefix("HKLM\\").unwrap_or(key);
            if !registry::key_exists(HKEY_LOCAL_MACHINE, subkey) {
                continue;
            }
            let name = safety::export_file_name(key);
            let status = Command::new("reg")
                .args(safety::export_args(key, &dir.join(&name)))
                .stdout(Stdio::null())
                .stderr(Stdio::null())
                .status()
                .map_err(|error| format!("cannot run reg.exe: {error}"))?;
            if !status.success() {
                return Err(format!("reg.exe could not export {name} (exit code {:?})", status.code()));
            }
            written.push(name);
        }
        Ok(written)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::selftest::diagnostics;
    use crate::selftest::render::render_markdown;
    use crate::selftest::runner;
    use crate::selftest::Status;
    use crate::settings::profile;

    /// Run by hand: `cargo test real_selftest -- --ignored --nocapture`. Without
    /// administrator rights the run only reads, so it is safe on any machine;
    /// with them it would apply profiles, so the test refuses to run.
    #[test]
    #[ignore]
    fn real_selftest_without_admin_writes_nothing() {
        let env = SystemEnv::new();
        if env.elevated() {
            println!("running elevated: skipped on purpose, this test must not write");
            return;
        }
        let mut info = diagnostics::collect_system();
        let (adapters, virtual_adapters, security, doh) =
            diagnostics::collect_network(&crate::adapters::get_network_adapters().unwrap());
        info.adapters = adapters;
        info.virtual_adapters = virtual_adapters;
        info.security_products = security;
        info.known_doh_servers = doh;

        let report = runner::run(&env, &profile::builtin().unwrap(), info, &mut |_| {});
        assert!(report.all_restored);
        assert!(report.cycles.iter().all(|c| c.checks.iter().all(|k| k.status == Status::Skip)));
        println!("{}", render_markdown(&report));
    }
}
