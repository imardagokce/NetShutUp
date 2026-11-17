//! What each command line command does. Everything the user reads on the
//! terminal is printed from here.

use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use crate::adapters::{self, NetworkAdapter};
use crate::dns::resolver::SystemResolver;
use crate::dns::{self, Outcome};
use crate::elevation::{self, Relaunch, Start};
use crate::menu::{self, Actions, ProfileChoice, Resume};
use crate::settings::apply::{self, ApplyError, PlannedChange};
use crate::settings::backup::{self, Backup};
use crate::settings::catalog::{self, CATALOG, Scope};
use crate::settings::plan::{self, DNS_SETTING, dns_trial_scope, is_dns, plan_for_precheck};
use crate::settings::profile::{self, Choose, DnsSpec, Profile};
use crate::settings::provider;
use crate::settings::reader::{self, Origin};
use crate::settings::store::{Store, SystemStore};
use crate::terminal::{self, Terminal};
use crate::selftest::diagnostics;
use crate::selftest::files;
use crate::selftest::render::render_markdown;
use crate::selftest::runner;
use crate::selftest::system_env::SystemEnv;
use crate::{style, version};

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

/// What a menu action needs from the caller.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Flow {
    Finished,
    /// Changing settings needs administrator rights and this copy has none.
    /// Nothing was printed or changed.
    NeedsAdmin,
}

#[derive(Debug, PartialEq, Eq)]
enum Route {
    /// Nothing would be written: say so, no administrator rights needed.
    Report,
    NeedsAdmin,
    Proceed,
}

fn route(nothing_to_write: bool, elevated: bool) -> Route {
    if nothing_to_write {
        Route::Report
    } else if elevated {
        Route::Proceed
    } else {
        Route::NeedsAdmin
    }
}

const UNDO_HINT_COMMAND: &str = "To undo: netshutup-modern revert";
const UNDO_HINT_MENU: &str = "To undo: choose \"Revert last change\"";

struct Prepared {
    profile: Profile,
    adapters: Vec<NetworkAdapter>,
    plan: Vec<PlannedChange>,
    needed: usize,
}

fn prepare(profile_id: &str) -> Option<Prepared> {
    let profile = find_profile(profile_id)?;
    let adapters = profile_adapters();
    let plan = plan_profile(&profile, &adapters);
    let needed = plan.iter().filter(|change| change.is_needed()).count();
    Some(Prepared { profile, adapters, plan, needed })
}

enum Nothing {
    Matches,
    Blocked(String),
}

/// Why applying would write nothing, if it would not.
fn nothing_to_write(prepared: &Prepared) -> Option<Nothing> {
    if prepared.needed == 0 {
        return Some(Nothing::Matches);
    }
    apply::check(&prepared.plan).err().map(Nothing::Blocked)
}

fn report_nothing(why: &Nothing) -> ExitCode {
    match why {
        Nothing::Matches => {
            println!("\nNothing to do: every setting already matches this profile.");
            ExitCode::SUCCESS
        }
        Nothing::Blocked(reason) => {
            eprintln!("\nThis profile cannot be applied: {reason}.\nNothing was changed.");
            ExitCode::FAILURE
        }
    }
}

fn show_preview(prepared: &Prepared) {
    print_preview(&prepared.profile, &prepared.plan, &prepared.adapters);
}

/// Applies a profile after showing the changes and asking for confirmation.
pub fn apply(profile_id: &str, dry_run: bool, elevated_copy: bool) -> ExitCode {
    let Some(prepared) = prepare(profile_id) else {
        return ExitCode::from(2);
    };

    show_preview(&prepared);

    if dry_run {
        println!("\nDry run: nothing was changed.");
        return ExitCode::SUCCESS;
    }
    if let Some(why) = nothing_to_write(&prepared) {
        return report_nothing(&why);
    }
    if !elevation::is_elevated() {
        return continue_elevated(&["apply", &prepared.profile.id], elevated_copy);
    }
    write_profile(&prepared, &mut confirm, UNDO_HINT_COMMAND)
}

/// The menu's apply. Shows the preview only when it can also go on to write;
/// without administrator rights it returns `Flow::NeedsAdmin` untouched, so the
/// copy that has them shows the preview instead.
pub fn menu_apply(profile_id: &str, ask: &mut dyn FnMut(&str) -> bool) -> Flow {
    let Some(prepared) = prepare(profile_id) else {
        return Flow::Finished;
    };
    let nothing = nothing_to_write(&prepared);

    match route(nothing.is_some(), elevation::is_elevated()) {
        Route::NeedsAdmin => Flow::NeedsAdmin,
        Route::Report => {
            show_preview(&prepared);
            if let Some(why) = &nothing {
                report_nothing(why);
            }
            Flow::Finished
        }
        Route::Proceed => {
            show_preview(&prepared);
            write_profile(&prepared, ask, UNDO_HINT_MENU);
            Flow::Finished
        }
    }
}

/// Asks for confirmation, saves the backup and writes. Needs administrator rights.
fn write_profile(
    prepared: &Prepared,
    ask: &mut dyn FnMut(&str) -> bool,
    undo_hint: &str,
) -> ExitCode {
    if !ask(&format!("Apply {}? A backup is saved first.", count(prepared.needed, "change"))) {
        println!("Cancelled. Nothing was changed.");
        return ExitCode::SUCCESS;
    }

    // Encrypted DNS is only changed when it can be proven afterwards: without
    // a working internet connection it is left alone.
    let dns_needed = prepared.plan.iter().filter(|c| is_dns(c) && c.is_needed()).count();
    let mut plan = prepared.plan.clone();
    if dns_needed > 0 {
        let precheck = dns::precheck(&SystemResolver, version::get_windows_version().2);
        if let Some(text) = dns::precheck_message(&precheck) {
            println!("\n{text}");
        }
        plan = plan_for_precheck(&plan, &precheck);
    }
    let dns_applied = dns_needed > 0 && plan.iter().any(is_dns);
    let needed = plan.iter().filter(|change| change.is_needed()).count();

    let store = SystemStore::system();
    let applied = apply::apply(
        &store,
        &backup::default_dir(),
        &prepared.profile.id,
        &backup::timestamp_now(),
        &plan,
    );

    match applied {
        Ok(Some(backup_file)) => {
            let dns_outcome = if dns_applied { run_dns(prepared) } else { None };
            let kept = matches!(dns_outcome, Some(Outcome::Encrypted { .. }));
            let changed = changed_count(needed, if dns_applied { dns_needed } else { 0 }, kept);

            println!("\nDone: {} changed.", count(changed, "setting"));
            println!("Backup: {}", backup_file.display());
            println!("{undo_hint}");
            let mut exit = ExitCode::SUCCESS;
            if let Some(outcome) = &dns_outcome {
                if matches!(outcome, Outcome::ProgramError(_)) {
                    eprintln!("\n{}", dns::message(outcome));
                    exit = ExitCode::FAILURE;
                } else {
                    println!("\n{}", dns::message(outcome));
                    if let Some(note) = dns::ipv6_note(outcome) {
                        println!("{note}");
                    }
                }
            }
            println!("{RESTART_NOTE}");
            exit
        }
        Ok(None) if dns_needed > 0 && !dns_applied => {
            println!("\nNothing else to do: the other settings already match this profile.");
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
            print_report_hint(&cause);
            eprintln!("The changes made up to that point were undone; your settings are as before.");
            ExitCode::FAILURE
        }
        Err(ApplyError::RolledBack { cause, leftovers }) => {
            eprintln!("\nNot applied: {cause}.");
            print_report_hint(&cause);
            eprintln!("These settings could not be put back automatically:");
            for leftover in &leftovers {
                eprintln!("  {leftover}");
            }
            eprintln!("The backup was kept. To try again: netshutup-modern revert");
            ExitCode::FAILURE
        }
    }
}

/// A failed DNS write is a problem on this computer or in NetShutUp, not a
/// blocked network: ask for a private report.
fn print_report_hint(cause: &str) {
    if cause.starts_with(DNS_SETTING) {
        eprintln!(
            "This looks like a problem in NetShutUp. Please report it privately: {}",
            dns::REPORT_URL
        );
    }
}

/// Runs the encrypted DNS trial for the DNS changes of a plan that was just applied.
fn run_dns(prepared: &Prepared) -> Option<Outcome> {
    let spec = prepared.profile.dns.as_ref()?;
    let (adapters, originals) = dns_trial_scope(&prepared.plan);

    println!("\nChecking encrypted DNS...");
    Some(dns::run(
        &SystemStore::system(),
        &SystemResolver,
        spec,
        &adapters,
        &originals,
        &mut |line| println!("  {line}"),
    ))
}

enum Latest {
    /// No backup exists.
    Nothing,
    /// A backup exists but cannot be read; the reason has been printed.
    Unusable,
    Found(PathBuf, Backup),
}

fn latest_backup() -> Latest {
    let Some(backup_file) = backup::latest(&backup::default_dir()) else {
        return Latest::Nothing;
    };
    match backup::load(&backup_file) {
        Ok(saved) => Latest::Found(backup_file, saved),
        Err(reason) => {
            eprintln!("The latest backup cannot be used: {reason}");
            eprintln!("File: {}", backup_file.display());
            Latest::Unusable
        }
    }
}

fn report_no_backup(latest: &Latest) -> ExitCode {
    match latest {
        Latest::Nothing => {
            println!("There is no change to revert.");
            ExitCode::SUCCESS
        }
        _ => ExitCode::FAILURE,
    }
}

fn show_revert(saved: &Backup) {
    let store = SystemStore::system();
    println!(
        "Revert \"{}\" (applied {}) — {} will be restored\n",
        saved.label,
        saved.created,
        count(saved.entries.len(), "setting")
    );
    print_table(["Setting", "Now", "Restored to"], &revert_rows(&store, saved, &load_adapters()));
}

/// Restores the settings saved by the most recent apply.
pub fn revert(elevated_copy: bool) -> ExitCode {
    let latest = latest_backup();
    let Latest::Found(backup_file, saved) = &latest else {
        return report_no_backup(&latest);
    };

    show_revert(saved);

    if !elevation::is_elevated() {
        return continue_elevated(&["revert"], elevated_copy);
    }
    restore_backup(backup_file, saved, &mut confirm)
}

/// The menu's revert; see `menu_apply` for when it shows things.
pub fn menu_revert(ask: &mut dyn FnMut(&str) -> bool) -> Flow {
    let latest = latest_backup();
    let found = matches!(latest, Latest::Found(..));

    match route(!found, elevation::is_elevated()) {
        Route::NeedsAdmin => Flow::NeedsAdmin,
        Route::Report => {
            report_no_backup(&latest);
            Flow::Finished
        }
        Route::Proceed => {
            if let Latest::Found(backup_file, saved) = &latest {
                show_revert(saved);
                restore_backup(backup_file, saved, ask);
            }
            Flow::Finished
        }
    }
}

/// Asks for confirmation and puts the saved values back. Needs administrator rights.
fn restore_backup(
    backup_file: &Path,
    saved: &Backup,
    ask: &mut dyn FnMut(&str) -> bool,
) -> ExitCode {
    if !ask("Restore these settings?") {
        println!("Cancelled. Nothing was changed.");
        return ExitCode::SUCCESS;
    }

    let errors = apply::revert(&SystemStore::system(), saved);
    if !errors.is_empty() {
        eprintln!("\nThese settings could not be restored:");
        for error in &errors {
            eprintln!("  {error}");
        }
        eprintln!("The backup was kept: {}", backup_file.display());
        return ExitCode::FAILURE;
    }

    println!("\nDone: {} restored.", count(saved.entries.len(), "setting"));
    if let Err(reason) = backup::mark_reverted(backup_file) {
        // Restoring twice is harmless, but the user should know why the
        // same backup would be offered again.
        eprintln!("Note: the backup could not be marked as used ({reason}).");
    }
    println!("{RESTART_NOTE}");
    ExitCode::SUCCESS
}

const SELFTEST_CONSENT: &str = "\
Self-test (for testers)

This applies each profile (Privacy, Performance, Recommended) one after another and undoes it
again. It takes a few minutes and your internet may drop for a moment. Everything is reverted;
the backups are kept.

It also notes diagnostic information about your hardware and network environment, without
personal identifiers. It is written only to a file on this computer; nothing is sent anywhere.";

/// Runs the self-test and writes its report. Needs administrator rights.
fn run_selftest(ask: &mut dyn FnMut(&str) -> bool) -> ExitCode {
    println!("{SELFTEST_CONSENT}");
    if !ask("Start the self-test?") {
        println!("Cancelled. Nothing was changed.");
        return ExitCode::SUCCESS;
    }

    let profiles = match profile::builtin() {
        Ok(profiles) => profiles,
        Err(reason) => {
            eprintln!("Built-in profiles could not be loaded: {reason}");
            return ExitCode::FAILURE;
        }
    };

    println!("\nCollecting diagnostic information...");
    let mut info = diagnostics::collect_system();
    let (adapters, virtual_adapters, security, doh) =
        diagnostics::collect_network(&adapters::get_network_adapters().unwrap_or_default());
    info.adapters = adapters;
    info.virtual_adapters = virtual_adapters;
    info.security_products = security;
    info.known_doh_servers = doh;

    println!();
    let report = runner::run(&SystemEnv::new(), &profiles, info, &mut |line| println!("  {line}"));
    let markdown = render_markdown(&report);

    let (pass, fail, skip, info_count) = report.counts();
    println!(
        "\nSelf-test finished: {pass} passed, {fail} failed, {skip} skipped, {info_count} notes."
    );
    if !report.all_restored {
        eprintln!("\nNOT everything was restored. Run: netshutup-modern revert");
    }

    let mut dirs = vec![files::reports_dir()];
    dirs.extend(files::desktop_dir());
    let stem = format!("NetShutUp-selftest-{}", report.created);
    match files::write_report(&report, &markdown, &dirs, &stem) {
        Ok(paths) => {
            println!("\nReport written:");
            for path in &paths {
                println!("  {}", path.display());
            }
            if let Some(folder) = paths.first().and_then(|path| path.parent()) {
                files::open_folder(folder);
            }
            println!("Please send both files (see \"How to send this report\" inside).");
        }
        Err(reason) => {
            eprintln!("\nThe report could not be written: {reason}\nHere it is instead:\n");
            println!("{markdown}");
            return ExitCode::FAILURE;
        }
    }

    if report.all_restored { ExitCode::SUCCESS } else { ExitCode::FAILURE }
}

/// `netshutup-modern selftest`: restarts as administrator when needed.
pub fn selftest(elevated_copy: bool) -> ExitCode {
    if !elevation::is_elevated() {
        return continue_elevated(&["selftest"], elevated_copy);
    }
    run_selftest(&mut confirm)
}

/// The menu's self-test; without administrator rights it returns `Flow::NeedsAdmin` untouched.
pub fn menu_selftest(ask: &mut dyn FnMut(&str) -> bool) -> Flow {
    if !elevation::is_elevated() {
        return Flow::NeedsAdmin;
    }
    run_selftest(ask);
    Flow::Finished
}

/// The title of the terminal window.
pub fn window_title(app_version: &str, elevated: bool) -> String {
    let rights = if elevated { " - Administrator" } else { "" };
    format!("NetShutUp - {app_version}{rights}")
}

/// The first line of the menu: program, Windows release, and rights.
fn header(app_version: &str, windows: (u32, u32, u32), elevated: bool) -> String {
    let (major, minor, build) = windows;
    let release = match (major, build) {
        (10, 22000..) => "Windows 11".to_string(),
        (10, _) => "Windows 10".to_string(),
        _ => format!("Windows {major}.{minor}"),
    };
    let rights = if elevated { "administrator" } else { "not elevated" };
    format!("NetShutUp {app_version} · {release} (build {build}) · {rights}")
}

/// The interactive menu. `resume` is set in the administrator copy.
pub fn menu(resume: Option<Resume>) -> ExitCode {
    let line = header(env!("CARGO_PKG_VERSION"), version::get_windows_version(), elevation::is_elevated());
    println!("{}\n", style::heading(&line));
    if let Some(warning) = files::header_marker_line(&files::read_marker()) {
        println!("{}\n", style::changed(&warning));
    }

    menu::run(&mut Terminal, &mut RealActions, resume);
    ExitCode::SUCCESS
}

struct RealActions;

impl Actions for RealActions {
    fn profiles(&self) -> Vec<ProfileChoice> {
        match profile::builtin() {
            Ok(profiles) => profiles
                .into_iter()
                .map(|profile| ProfileChoice {
                    label: format!("{:<12} {}", profile.name, profile.description),
                    id: profile.id,
                })
                .collect(),
            Err(reason) => {
                eprintln!("Built-in profiles could not be loaded: {reason}");
                Vec::new()
            }
        }
    }

    fn scan(&mut self) {
        scan();
    }

    fn apply(&mut self, profile_id: &str) -> Flow {
        menu_apply(profile_id, &mut terminal::confirm)
    }

    fn revert(&mut self) -> Flow {
        menu_revert(&mut terminal::confirm)
    }

    fn selftest(&mut self) -> Flow {
        menu_selftest(&mut terminal::confirm)
    }

    fn hand_over(&mut self, resume: &Resume) -> Start {
        let mut args = resume.args();
        args.push(ELEVATED_FLAG.to_string());
        let args: Vec<&str> = args.iter().map(String::as_str).collect();
        elevation::start_elevated(&args)
    }
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
    let (plan, unreadable) =
        plan::plan_profile(&SystemStore::system(), profile, &guids, dns_available());
    for guid in unreadable {
        let name = adapters
            .iter()
            .find(|adapter| adapter.guid == guid)
            .map_or(guid.as_str(), |adapter| adapter.friendly_name.as_str());
        eprintln!("Note: the DNS settings of {name} could not be read, so DNS will not be changed there.");
    }
    plan
}

/// Encrypted DNS needs Windows 11; on older builds the DNS stage is left out
/// of the plan altogether (the rest of the profile is unaffected).
fn dns_available() -> bool {
    version::get_windows_version().2 >= dns::FIRST_BUILD_WITH_DOH
}

/// What the After column says for the DNS rows: the provider is only
/// settled when the trial runs, so say how it will be chosen.
fn dns_after_text(spec: &DnsSpec) -> String {
    let providers: Vec<&provider::Provider> =
        spec.providers.iter().filter_map(|id| provider::find(id)).collect();
    match (spec.choose, providers.first()) {
        (Choose::FirstWorking, Some(first)) => catalog::find(DNS_SETTING)
            .map_or_else(String::new, |setting| reader::format(setting, &first.state())),
        (Choose::FirstWorking, None) => String::new(),
        (Choose::Fastest, _) => {
            let names: Vec<&str> = providers.iter().map(|provider| provider.name).collect();
            format!("fastest of {} (DoH)", names.join(", "))
        }
    }
}

/// How many settings ended up changed: DNS changes that were put back do not count.
fn changed_count(needed: usize, dns_needed: usize, dns_kept: bool) -> usize {
    if dns_kept { needed } else { needed.saturating_sub(dns_needed) }
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
        let origin = if origin == "(changed from default)" {
            style::changed(origin)
        } else {
            origin.to_string()
        };
        println!(
            "  {:<26} {:<28} {origin}",
            setting.name,
            reader::format(setting, &state)
        );
    }
}

fn print_preview(profile: &Profile, plan: &[PlannedChange], adapters: &[NetworkAdapter]) {
    let rows = preview_rows(plan, adapters, profile.dns.as_ref());

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
    if profile.dns.is_some() && !dns_available() {
        println!("  (DNS: this Windows version has no encrypted DNS, so DNS will not be changed)");
    }
}

/// One `[setting, current, after]` row per setting that would change.
fn preview_rows(
    plan: &[PlannedChange],
    adapters: &[NetworkAdapter],
    dns: Option<&DnsSpec>,
) -> Vec<[String; 3]> {
    plan.iter()
        .filter(|change| change.is_needed())
        .map(|change| {
            let setting = change.target.setting;
            let after = match dns {
                Some(spec) if is_dns(change) => dns_after_text(spec),
                _ => reader::format_desired(setting, &change.target.desired),
            };
            [
                row_name(setting.name, change.target.adapter.as_deref(), adapters),
                reader::format(setting, &change.current),
                after,
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

    // Pad first, paint after, so the escape codes never disturb the columns.
    let print_row = |cells: [&str; 3], last: String| {
        println!(
            "  {:<name$}  {:<current$}  {last}",
            cells[0],
            cells[1],
            name = widths[0],
            current = widths[1]
        );
    };
    print_row(header, style::heading(header[2]));
    for row in rows {
        print_row(row.each_ref().map(String::as_str), style::changed(&row[2]));
    }
}

/// "1 setting", "3 settings".
fn count(number: usize, noun: &str) -> String {
    if number == 1 { format!("1 {noun}") } else { format!("{number} {noun}s") }
}

fn or_dash(value: String) -> String {
    if value.is_empty() { "-".to_string() } else { value }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn nothing_to_write_is_reported_without_asking_for_admin() {
        assert_eq!(route(true, false), Route::Report);
        assert_eq!(route(true, true), Route::Report);
    }

    fn dns_spec(providers: &[&str], choose: Choose) -> DnsSpec {
        DnsSpec { providers: providers.iter().map(|id| id.to_string()).collect(), choose }
    }

    #[test]
    fn dns_after_text_for_first_working_shows_the_first_provider() {
        let spec = dns_spec(&["cloudflare", "google"], Choose::FirstWorking);
        assert_eq!(dns_after_text(&spec), "1.1.1.1, 1.0.0.1 (DoH)");
    }

    #[test]
    fn dns_after_text_for_fastest_names_the_candidates() {
        let spec = dns_spec(&["cloudflare", "google"], Choose::Fastest);
        assert_eq!(dns_after_text(&spec), "fastest of Cloudflare, Google (DoH)");
    }

    #[test]
    fn changed_count_excludes_dns_when_it_was_not_kept() {
        assert_eq!(changed_count(5, 2, false), 3);
        assert_eq!(changed_count(5, 2, true), 5);
        assert_eq!(changed_count(1, 2, false), 0);
    }

    #[test]
    fn window_title_shows_the_version_and_admin_rights() {
        assert_eq!(window_title("0.1.0-alpha.1", false), "NetShutUp - 0.1.0-alpha.1");
        assert_eq!(
            window_title("0.1.0-alpha.1", true),
            "NetShutUp - 0.1.0-alpha.1 - Administrator"
        );
    }

    #[test]
    fn header_names_the_windows_release_and_rights() {
        assert_eq!(
            header("0.1.0", (10, 0, 26200), false),
            "NetShutUp 0.1.0 · Windows 11 (build 26200) · not elevated"
        );
        assert_eq!(
            header("0.1.0", (10, 0, 19045), true),
            "NetShutUp 0.1.0 · Windows 10 (build 19045) · administrator"
        );
    }

    #[test]
    fn a_real_change_needs_admin_first() {
        assert_eq!(route(false, false), Route::NeedsAdmin);
        assert_eq!(route(false, true), Route::Proceed);
    }
}
