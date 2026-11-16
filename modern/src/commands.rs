//! What each command line command does. Everything the user reads on the
//! terminal is printed from here.

use std::io::{self, Write};
use std::process::ExitCode;

use crate::adapters::{self, NetworkAdapter};
use crate::elevation::{self, Relaunch};
use crate::settings::apply::{self, ApplyError, PlannedChange};
use crate::settings::backup::{self, Backup};
use crate::settings::catalog::{self, CATALOG, Scope};
use crate::settings::profile::{self, Profile};
use crate::settings::reader::{self, Origin};
use crate::settings::store::{Store, SystemStore};
use crate::version;

/// Added to the command line of the copy that is restarted as administrator.
/// That copy runs in its own window, so it waits for Enter before closing.
pub const ELEVATED_FLAG: &str = "--elevated";

const RESTART_NOTE: &str = "Some changes take full effect only after Windows is restarted.";

/// Shows the Windows version, the adapters and every setting. Reads only.
pub fn scan() -> ExitCode {
    let version = version::get_windows_version();
    println!("Windows Version: {}.{}.{}", version.0, version.1, version.2);

    let adapters = load_adapters();
    print_adapters(&adapters);

    for adapter in adapters.iter().filter(|adapter| !adapter.is_loopback) {
        print_settings(&adapter.friendly_name, Scope::Adapter, Some(&adapter.guid));
    }
    print_settings("System", Scope::System, None);
    ExitCode::SUCCESS
}

/// Shows what a profile would change. Reads only.
pub fn preview(profile_id: &str) -> ExitCode {
    let Some(profile) = find_profile(profile_id) else {
        return ExitCode::from(2);
    };
    let adapters = profile_adapters();
    let plan = plan_profile(&profile, &adapters);

    print_preview(&profile, &plan, &adapters);
    println!("\nPreview only: nothing was changed.");
    ExitCode::SUCCESS
}

/// Applies a profile after showing the changes and asking for confirmation.
pub fn apply(profile_id: &str, dry_run: bool, elevated_copy: bool) -> ExitCode {
    let Some(profile) = find_profile(profile_id) else {
        return ExitCode::from(2);
    };
    let adapters = profile_adapters();
    let plan = plan_profile(&profile, &adapters);
    let needed = plan.iter().filter(|change| change.is_needed()).count();

    print_preview(&profile, &plan, &adapters);

    if dry_run {
        println!("\nDry run: nothing was changed.");
        return ExitCode::SUCCESS;
    }
    if needed == 0 {
        println!("\nNothing to do: every setting already matches this profile.");
        return ExitCode::SUCCESS;
    }
    if let Err(reason) = apply::check(&plan) {
        eprintln!("\nThis profile cannot be applied: {reason}.\nNothing was changed.");
        return ExitCode::FAILURE;
    }
    if !elevation::is_elevated() {
        return continue_elevated(&["apply", &profile.id], elevated_copy);
    }
    if !confirm(&format!("Apply {}? A backup is saved first.", count(needed, "change"))) {
        println!("Cancelled. Nothing was changed.");
        return ExitCode::SUCCESS;
    }

    let store = SystemStore::system();
    let applied =
        apply::apply(&store, &backup::default_dir(), &profile.id, &backup::timestamp_now(), &plan);

    match applied {
        Ok(Some(backup_file)) => {
            println!("\nDone: {} changed.", count(needed, "setting"));
            println!("Backup: {}", backup_file.display());
            println!("To undo: netshutup-modern revert");
            println!("{RESTART_NOTE}");
            ExitCode::SUCCESS
        }
        Ok(None) => {
            println!("\nNothing to do: every setting already matches this profile.");
            ExitCode::SUCCESS
        }
        Err(ApplyError::Refused(reason)) => {
            eprintln!("\nNot applied: {reason}.\nNothing was changed.");
            ExitCode::FAILURE
        }
        Err(ApplyError::RolledBack { cause, leftovers }) if leftovers.is_empty() => {
            eprintln!("\nNot applied: {cause}.");
            eprintln!("The changes made up to that point were undone; your settings are as before.");
            ExitCode::FAILURE
        }
        Err(ApplyError::RolledBack { cause, leftovers }) => {
            eprintln!("\nNot applied: {cause}.");
            eprintln!("These settings could not be put back automatically:");
            for leftover in &leftovers {
                eprintln!("  {leftover}");
            }
            eprintln!("The backup was kept. To try again: netshutup-modern revert");
            ExitCode::FAILURE
        }
    }
}

/// Restores the settings saved by the most recent apply.
pub fn revert(elevated_copy: bool) -> ExitCode {
    let Some(backup_file) = backup::latest(&backup::default_dir()) else {
        println!("There is no change to revert.");
        return ExitCode::SUCCESS;
    };
    let saved = match backup::load(&backup_file) {
        Ok(saved) => saved,
        Err(reason) => {
            eprintln!("The latest backup cannot be used: {reason}");
            eprintln!("File: {}", backup_file.display());
            return ExitCode::FAILURE;
        }
    };

    let store = SystemStore::system();
    println!(
        "Revert \"{}\" (applied {}) — {} will be restored\n",
        saved.label,
        saved.created,
        count(saved.entries.len(), "setting")
    );
    print_table(["Setting", "Now", "Restored to"], &revert_rows(&store, &saved, &load_adapters()));

    if !elevation::is_elevated() {
        return continue_elevated(&["revert"], elevated_copy);
    }
    if !confirm("Restore these settings?") {
        println!("Cancelled. Nothing was changed.");
        return ExitCode::SUCCESS;
    }

    let errors = apply::revert(&store, &saved);
    if !errors.is_empty() {
        eprintln!("\nThese settings could not be restored:");
        for error in &errors {
            eprintln!("  {error}");
        }
        eprintln!("The backup was kept: {}", backup_file.display());
        return ExitCode::FAILURE;
    }

    println!("\nDone: {} restored.", count(saved.entries.len(), "setting"));
    if let Err(reason) = backup::mark_reverted(&backup_file) {
        // Restoring twice is harmless, but the user should know why the
        // same backup would be offered again.
        eprintln!("Note: the backup could not be marked as used ({reason}).");
    }
    println!("{RESTART_NOTE}");
    ExitCode::SUCCESS
}

/// Keeps the window of the administrator copy open until the user has read it.
pub fn wait_for_enter() {
    print!("\nPress Enter to close this window.");
    let _ = io::stdout().flush();
    let _ = io::stdin().read_line(&mut String::new());
}

fn continue_elevated(args: &[&str], elevated_copy: bool) -> ExitCode {
    if elevated_copy {
        // Already restarted once; asking again would loop.
        eprintln!("\nAdministrator rights could not be obtained. Nothing was changed.");
        return ExitCode::FAILURE;
    }

    println!("\nChanging settings needs administrator rights.");
    println!("Windows will ask for permission; the rest continues in a new window.");

    let mut args = args.to_vec();
    args.push(ELEVATED_FLAG);
    match elevation::relaunch_elevated(&args) {
        Relaunch::Finished(0) => {
            println!("The administrator window has finished.");
            ExitCode::SUCCESS
        }
        Relaunch::Finished(code) => {
            eprintln!("The administrator window ended with an error (code {code}).");
            ExitCode::FAILURE
        }
        Relaunch::Declined => {
            println!("Permission was not given. Nothing was changed.");
            ExitCode::FAILURE
        }
        Relaunch::Failed(reason) => {
            eprintln!("{reason}. Nothing was changed.");
            ExitCode::FAILURE
        }
    }
}

fn confirm(question: &str) -> bool {
    print!("\n{question} [y/N] ");
    let _ = io::stdout().flush();

    let mut answer = String::new();
    if io::stdin().read_line(&mut answer).is_err() {
        return false;
    }
    matches!(answer.trim().to_ascii_lowercase().as_str(), "y" | "yes")
}

fn find_profile(profile_id: &str) -> Option<Profile> {
    let profiles = match profile::builtin() {
        Ok(profiles) => profiles,
        Err(reason) => {
            eprintln!("Built-in profiles could not be loaded: {reason}");
            return None;
        }
    };

    if !profiles.iter().any(|profile| profile.id.eq_ignore_ascii_case(profile_id)) {
        eprintln!("Unknown profile \"{profile_id}\". Available profiles:");
        for profile in &profiles {
            eprintln!("  {:<12} {}", profile.id, profile.description);
        }
        return None;
    }
    profiles.into_iter().find(|profile| profile.id.eq_ignore_ascii_case(profile_id))
}

fn load_adapters() -> Vec<NetworkAdapter> {
    match adapters::get_network_adapters() {
        Ok(adapters) => adapters,
        Err(code) => {
            eprintln!("GetAdaptersAddresses failed with error code {code}");
            Vec::new()
        }
    }
}

/// The adapters a profile's per-adapter settings go to.
fn profile_adapters() -> Vec<NetworkAdapter> {
    load_adapters().into_iter().filter(|adapter| adapter.takes_profile_settings).collect()
}

fn plan_profile(profile: &Profile, adapters: &[NetworkAdapter]) -> Vec<PlannedChange> {
    let guids: Vec<String> = adapters.iter().map(|adapter| adapter.guid.clone()).collect();
    apply::plan(&SystemStore::system(), profile.targets(&guids))
}

fn print_adapters(adapters: &[NetworkAdapter]) {
    println!("\nNetwork Adapters ({}):", adapters.len());
    for (i, adapter) in adapters.iter().enumerate() {
        println!("\n[{}] {}", i + 1, adapter.friendly_name);
        println!("    Description: {}", adapter.description);
        println!("    GUID:        {}", adapter.guid);
        println!("    Status:      {}", if adapter.is_up { "Up" } else { "Down" });
        println!("    IP Address:  {}", or_dash(adapter.ip_addresses.join(", ")));
        println!("    MAC Address: {}", or_dash(adapter.mac_address.clone()));
        println!("    Type:        {}", adapter.adapter_type);
    }
}

fn print_settings(title: &str, scope: Scope, adapter_guid: Option<&str>) {
    let store = SystemStore::system();
    println!("\nSettings — {title}");
    for setting in CATALOG.iter().filter(|setting| setting.scope == scope) {
        let state = store.read(setting, adapter_guid);
        let origin = match reader::classify(setting, &state) {
            Origin::Default => "(Windows default)",
            Origin::Changed => "(changed from default)",
            Origin::Unknown => "",
        };
        println!(
            "  {:<26} {:<28} {origin}",
            setting.name,
            reader::format(setting, &state)
        );
    }
}

fn print_preview(profile: &Profile, plan: &[PlannedChange], adapters: &[NetworkAdapter]) {
    let rows = preview_rows(plan, adapters);

    println!("{} — {} of {} settings will change", profile.name, rows.len(), plan.len());
    println!("{}", profile.description);

    if !rows.is_empty() {
        println!();
        print_table(["Setting", "Current", "After"], &rows);
    }
    match plan.len() - rows.len() {
        0 => {}
        1 => println!("  (1 setting already matches)"),
        unchanged => println!("  ({unchanged} settings already match)"),
    }
}

/// One `[setting, current, after]` row per setting that would change.
fn preview_rows(plan: &[PlannedChange], adapters: &[NetworkAdapter]) -> Vec<[String; 3]> {
    plan.iter()
        .filter(|change| change.is_needed())
        .map(|change| {
            let setting = change.target.setting;
            [
                row_name(setting.name, change.target.adapter.as_deref(), adapters),
                reader::format(setting, &change.current),
                reader::format_desired(setting, &change.target.desired),
            ]
        })
        .collect()
}

/// One `[setting, now, restored to]` row per setting in a backup.
fn revert_rows(store: &dyn Store, saved: &Backup, adapters: &[NetworkAdapter]) -> Vec<[String; 3]> {
    saved
        .entries
        .iter()
        .map(|entry| {
            let adapter = entry.adapter.as_deref();
            match catalog::find(&entry.id) {
                Some(setting) => [
                    row_name(setting.name, adapter, adapters),
                    reader::format(setting, &store.read(setting, adapter)),
                    reader::format(setting, &entry.previous),
                ],
                None => [entry.id.clone(), "?".to_string(), "unknown setting".to_string()],
            }
        })
        .collect()
}

/// A setting's name, with the adapter it belongs to when it is per-adapter.
fn row_name(setting_name: &str, adapter_guid: Option<&str>, adapters: &[NetworkAdapter]) -> String {
    let Some(guid) = adapter_guid else {
        return setting_name.to_string();
    };
    let adapter = adapters
        .iter()
        .find(|adapter| adapter.guid == guid)
        .map_or(guid, |adapter| adapter.friendly_name.as_str());
    format!("{setting_name} ({adapter})")
}

fn print_table(header: [&str; 3], rows: &[[String; 3]]) {
    let mut widths = header.map(|cell| cell.chars().count());
    for row in rows {
        for (width, cell) in widths.iter_mut().zip(row) {
            *width = (*width).max(cell.chars().count());
        }
    }

    let print_row = |cells: [&str; 3]| {
        println!(
            "  {:<name$}  {:<current$}  {}",
            cells[0],
            cells[1],
            cells[2],
            name = widths[0],
            current = widths[1]
        );
    };
    print_row(header);
    for row in rows {
        print_row(row.each_ref().map(String::as_str));
    }
}

/// "1 setting", "3 settings".
fn count(number: usize, noun: &str) -> String {
    if number == 1 { format!("1 {noun}") } else { format!("{number} {noun}s") }
}

fn or_dash(value: String) -> String {
    if value.is_empty() { "-".to_string() } else { value }
}
