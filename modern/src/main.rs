mod adapters;
mod settings;
mod version;

use std::process::ExitCode;

use adapters::NetworkAdapter;
use settings::apply::{self, PlannedChange};
use settings::catalog::{CATALOG, Scope};
use settings::profile::{self, Profile};
use settings::reader::{self, Origin, State};
use settings::store::{Store, SystemStore};

const USAGE: &str = "\
Usage:
  netshutup-modern                    Show the current network settings
  netshutup-modern preview <profile>  Show what a profile would change

Nothing is modified by either command.";

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let args: Vec<&str> = args.iter().map(String::as_str).collect();

    match args.as_slice() {
        [] => {
            scan();
            ExitCode::SUCCESS
        }
        ["preview", profile] => preview(profile),
        _ => {
            eprintln!("{USAGE}");
            ExitCode::from(2)
        }
    }
}

fn scan() {
    let version = version::get_windows_version();
    println!("Windows Version: {}.{}.{}", version.0, version.1, version.2);

    let adapters = load_adapters();
    print_adapters(&adapters);

    for adapter in adapters.iter().filter(|adapter| !adapter.is_loopback) {
        print_settings(&adapter.friendly_name, Scope::Adapter, Some(&adapter.guid));
    }
    print_settings("System", Scope::System, None);
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

/// Shows what applying a profile would change. Reads only.
fn preview(profile_id: &str) -> ExitCode {
    let profiles = match profile::builtin() {
        Ok(profiles) => profiles,
        Err(reason) => {
            eprintln!("Built-in profiles could not be loaded: {reason}");
            return ExitCode::FAILURE;
        }
    };

    let Some(profile) =
        profiles.iter().find(|profile| profile.id.eq_ignore_ascii_case(profile_id))
    else {
        eprintln!("Unknown profile \"{profile_id}\". Available profiles:");
        for profile in &profiles {
            eprintln!("  {:<12} {}", profile.id, profile.description);
        }
        return ExitCode::from(2);
    };

    let adapters: Vec<NetworkAdapter> = load_adapters()
        .into_iter()
        .filter(|adapter| adapter.takes_profile_settings)
        .collect();
    let guids: Vec<String> = adapters.iter().map(|adapter| adapter.guid.clone()).collect();

    let plan = apply::plan(&SystemStore::system(), profile.targets(&guids));
    print_preview(profile, &plan, &adapters);
    ExitCode::SUCCESS
}

fn print_preview(profile: &Profile, plan: &[PlannedChange], adapters: &[NetworkAdapter]) {
    let rows = preview_rows(plan, adapters);
    let unchanged = plan.len() - rows.len();

    println!("{} — {} of {} settings will change", profile.name, rows.len(), plan.len());
    println!("{}", profile.description);

    if !rows.is_empty() {
        let header = ["Setting".to_string(), "Current".to_string(), "After".to_string()];
        let mut widths = header.each_ref().map(|cell| cell.chars().count());
        for row in &rows {
            for (width, cell) in widths.iter_mut().zip(row) {
                *width = (*width).max(cell.chars().count());
            }
        }

        println!();
        for row in std::iter::once(&header).chain(&rows) {
            println!(
                "  {:<name$}  {:<current$}  {}",
                row[0],
                row[1],
                row[2],
                name = widths[0],
                current = widths[1]
            );
        }
    }

    match unchanged {
        0 => {}
        1 => println!("  (1 setting already matches)"),
        count => println!("  ({count} settings already match)"),
    }

    let unreadable = plan
        .iter()
        .filter(|change| matches!(change.current, State::Unavailable(_)))
        .count();
    if unreadable > 0 {
        println!(
            "\n{unreadable} settings cannot be read, so they cannot be backed up; \
             the profile would not be applied."
        );
    }

    println!("\nPreview only: nothing was changed.");
}

/// One `[setting, current, after]` row per setting that would change.
fn preview_rows(plan: &[PlannedChange], adapters: &[NetworkAdapter]) -> Vec<[String; 3]> {
    plan.iter()
        .filter(|change| change.is_needed())
        .map(|change| {
            let setting = change.target.setting;
            let adapter_name = change.target.adapter.as_deref().map(|guid| {
                adapters
                    .iter()
                    .find(|adapter| adapter.guid == guid)
                    .map_or(guid, |adapter| adapter.friendly_name.as_str())
            });
            let name = match adapter_name {
                Some(adapter) => format!("{} ({adapter})", setting.name),
                None => setting.name.to_string(),
            };
            [
                name,
                reader::format(setting, &change.current),
                reader::format_desired(setting, &change.target.desired),
            ]
        })
        .collect()
}

fn or_dash(value: String) -> String {
    if value.is_empty() { "-".to_string() } else { value }
}
