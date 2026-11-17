//! The real computer behind `Env`: the real settings, the Windows resolver,
//! the real adapters and the backup folder.

use std::path::PathBuf;

use crate::adapters;
use crate::dns::Resolver;
use crate::dns::resolver::SystemResolver;
use crate::elevation;
use crate::settings::backup;
use crate::settings::registry;
use crate::settings::store::{Store, SystemStore};
use crate::version;

use windows::Win32::System::Registry::HKEY_LOCAL_MACHINE;

use super::files::{self, Marker};
use super::mask::mask_guid;
use super::runner::{Env, EnvAdapter};

pub struct SystemEnv {
    store: SystemStore,
    resolver: SystemResolver,
}

impl SystemEnv {
    pub fn new() -> Self {
        Self { store: SystemStore::system(), resolver: SystemResolver }
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
