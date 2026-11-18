//! Runs the self-test: reads everything, then applies and reverts each profile
//! for real, checking at every step. It uses the same engine as `apply` and
//! `revert`, so what it proves is what the program does.
//!
//! The outside world (settings store, resolver, time, administrator rights)
//! comes in through `Env`, so the whole run is tested with fakes.

use std::path::PathBuf;
use std::time::Instant;

use crate::dns::dot::DotBackend;
use crate::dns::{self, Outcome, Precheck, Resolver};
use crate::settings::apply::{self, ApplyError};
use crate::settings::backup;
use crate::settings::catalog::{self, CATALOG, Scope};
use crate::settings::plan;
use crate::settings::profile::Profile;
use crate::settings::provider;
use crate::settings::reader::{self, State};
use crate::settings::store::Store;

use super::diagnostics::Diagnostics;
use super::files::Marker;
use super::mask::mask_guid;
use super::{Check, Cycle, Report, Status};

pub struct EnvAdapter {
    pub guid: String,
}

pub trait Env {
    fn store(&self) -> &dyn Store;
    fn resolver(&self) -> &dyn Resolver;
    /// The experimental DoT list; a fake in tests so nothing real is touched.
    fn dot_backend(&self) -> &dyn DotBackend;
    /// The Ethernet and Wi-Fi hardware adapters profiles apply to.
    fn adapters(&self) -> Vec<EnvAdapter>;
    fn windows_build(&self) -> u32;
    fn elevated(&self) -> bool;
    fn backup_dir(&self) -> PathBuf;
    /// `YYYYMMDD-HHMMSS`.
    fn timestamp(&self) -> String;
    /// Records the self-test's state in a file, so an interrupted one is noticed.
    fn mark(&self, marker: Marker);
    /// The per-adapter encrypted DNS entries Windows holds (masked), to see
    /// whether any are left behind after the run.
    fn doh_entries(&self) -> Vec<String>;
    /// Puts back the DoT entries saved next to this backup (no-op without a
    /// sidecar). Returns the problems found; an empty list means all is back.
    fn restore_dot(&self, backup_path: &std::path::Path) -> Result<Vec<String>, String>;
    /// Windows' global encrypted DNS server list for the providers' addresses,
    /// as `"<address>:<n values or absent>"` lines, to compare start and end.
    fn dot_entries(&self) -> Vec<String>;
    /// Starts a system restore point. The text says what happened.
    fn restore_point(&self) -> Result<String, String>;
    /// Exports the relevant registry keys to files; returns their names.
    fn export_registry(&self) -> Result<Vec<String>, String>;
    /// Closes the restore point `restore_point` started.
    fn finish_restore_point(&self);
}

/// The value of every catalog setting: system-wide ones once, per-adapter ones
/// for each adapter.
#[derive(Debug, Clone, PartialEq)]
pub struct Snapshot(pub(crate) Vec<(String, Option<String>, State)>);

pub fn snapshot(store: &dyn Store, adapters: &[EnvAdapter]) -> Snapshot {
    let mut entries = Vec::new();
    for setting in CATALOG {
        match setting.scope {
            Scope::System => {
                entries.push((setting.id.to_string(), None, store.read(setting, None)));
            }
            Scope::Adapter => {
                for adapter in adapters {
                    entries.push((
                        setting.id.to_string(),
                        Some(adapter.guid.clone()),
                        store.read(setting, Some(&adapter.guid)),
                    ));
                }
            }
        }
    }
    Snapshot(entries)
}

/// What differs between two snapshots, one line each. A service counts as the
/// same when its start type is, like everywhere else in the program.
pub fn differences(before: &Snapshot, after: &Snapshot) -> Vec<String> {
    if before.0.len() != after.0.len() {
        return vec!["the two snapshots cover different settings".to_string()];
    }
    before
        .0
        .iter()
        .zip(&after.0)
        .filter(|((_, _, was), (_, _, now))| was != now && !reader::matches(was, now))
        .map(|((id, adapter, was), (_, _, now))| {
            let (was, now) = match catalog::find(id) {
                Some(setting) => (reader::format(setting, was), reader::format(setting, now)),
                None => (format!("{was:?}"), format!("{now:?}")),
            };
            let place = adapter.as_deref().map_or("system".to_string(), mask_guid);
            format!("{id} ({place}): was {was}, now {now}")
        })
        .collect()
}

fn millis(since: Instant) -> u64 {
    since.elapsed().as_millis() as u64
}

fn check(id: &str, title: &str, status: Status, detail: &str, since: Instant) -> Check {
    Check::new(id, title, status, detail, millis(since))
}

fn pass_or_fail(ok: bool) -> Status {
    if ok { Status::Pass } else { Status::Fail }
}

/// Runs the whole self-test. The marker says `Running` for the duration and
/// names the backup of a profile while it is applied; if the program dies
/// mid-run it stays, which is the point.
pub fn run(
    env: &dyn Env,
    profiles: &[Profile],
    diagnostics: Diagnostics,
    say: &mut dyn FnMut(&str),
) -> Report {
    env.mark(Marker::Running);
    run_inner(env, profiles, diagnostics, say)
}

fn run_inner(
    env: &dyn Env,
    profiles: &[Profile],
    diagnostics: Diagnostics,
    say: &mut dyn FnMut(&str),
) -> Report {
    let started = Instant::now();
    let adapters = env.adapters();
    let guids: Vec<String> = adapters.iter().map(|adapter| adapter.guid.clone()).collect();
    let dns_ok = env.windows_build() >= dns::FIRST_BUILD_WITH_DOH;

    say("Checking the environment...");
    let (environment, mut can_write) = environment_checks(env, &adapters);
    let mut safety = Vec::new();
    let mut restore_began = false;
    if can_write {
        say("Saving a copy of the relevant registry keys and a restore point...");
        let (checks, exported, began) = safety_checks(env);
        safety = checks;
        can_write = exported;
        restore_began = began;
    }
    let start = snapshot(env.store(), &adapters);
    let doh_before = env.doh_entries();
    let dot_before = env.dot_entries();

    say("Checking that every setting can be read...");
    let reading = reading_checks(env, profiles, &adapters, &guids, dns_ok);

    let mut cycles = Vec::new();
    let mut created = Vec::new();
    let mut revert_failed = false;
    let mut active: Option<PathBuf> = None;
    for profile in profiles {
        if !can_write {
            cycles.push(skipped_cycle(profile, &skip_reason(&safety)));
            continue;
        }
        if revert_failed {
            break;
        }
        say(&format!("{}: applying and reverting...", profile.name));
        let result = cycle(env, profile, &adapters, &guids, dns_ok, &mut created, say);
        revert_failed = result.revert_failed;
        active = result.active;
        cycles.push(result.cycle);
    }

    say("Comparing the end state with the start...");
    let began = Instant::now();
    let differing = differences(&start, &snapshot(env.store(), &adapters));
    let mut closing = vec![check(
        "closing.snapshot",
        "Every setting is as it was at the start",
        pass_or_fail(differing.is_empty()),
        &if differing.is_empty() { "no difference".to_string() } else { differing.join("; ") },
        began,
    )];
    let began = Instant::now();
    let doh_after = env.doh_entries();
    let doh_same = doh_before == doh_after;
    closing.push(check(
        "closing.doh_entries",
        "No encrypted DNS entries are left behind",
        pass_or_fail(doh_same),
        &if doh_same {
            "same as at the start".to_string()
        } else {
            format!("before: [{}]; after: [{}]", doh_before.join(", "), doh_after.join(", "))
        },
        began,
    ));
    let began = Instant::now();
    let dot_after = env.dot_entries();
    let dot_same = dot_before == dot_after;
    closing.push(check(
        "closing.dot_entries",
        "Windows' DoT/DoH server list is as it was",
        pass_or_fail(dot_same),
        &if dot_same {
            "same as at the start".to_string()
        } else {
            format!("before: [{}]; after: [{}]", dot_before.join(", "), dot_after.join(", "))
        },
        began,
    ));
    let began = Instant::now();
    let still_active: Vec<String> = created
        .iter()
        .filter(|path| path.exists())
        .filter_map(|path| path.file_name().map(|name| name.to_string_lossy().to_string()))
        .collect();
    closing.push(check(
        "closing.backups",
        "Every backup made here is marked as reverted",
        pass_or_fail(still_active.is_empty()),
        &if still_active.is_empty() {
            format!("{} backup(s), all marked", created.len())
        } else {
            format!("still active: {}", still_active.join(", "))
        },
        began,
    ));
    let began = Instant::now();
    closing.push(check(
        "closing.internet",
        "DNS resolves at the end",
        pass_or_fail(dns::internet_works(env.resolver())),
        "",
        began,
    ));

    let all_restored = !revert_failed && differing.is_empty() && doh_same && dot_same;
    env.mark(match (all_restored, active) {
        (true, _) => Marker::Clear,
        (false, Some(path)) => Marker::Active(path),
        (false, None) => Marker::NotRestored,
    });
    if restore_began {
        env.finish_restore_point();
    }

    Report {
        schema: Report::SCHEMA,
        program_version: env!("CARGO_PKG_VERSION").to_string(),
        created: env.timestamp(),
        total_millis: millis(started),
        all_restored,
        diagnostics,
        environment,
        safety,
        reading,
        cycles,
        closing,
    }
}

/// Phase 0: registry export (must work, or nothing is written) and a restore
/// point (nice to have). Returns the checks, whether writing may go on, and
/// whether a restore point was begun (so it has to be finished).
fn safety_checks(env: &dyn Env) -> (Vec<Check>, bool, bool) {
    let mut checks = Vec::new();

    let began = Instant::now();
    let exported = env.export_registry();
    let (status, detail) = match &exported {
        Ok(files) => (Status::Pass, format!("{} file(s) saved in the reg-backups folder", files.len())),
        Err(reason) => (Status::Fail, reason.clone()),
    };
    checks.push(check("safety.registry_export", "Registry keys exported to .reg files", status, &detail, began));
    if exported.is_err() {
        checks.push(Check::new(
            "safety.restore_point",
            "System restore point created",
            Status::Skip,
            "not attempted because the registry export failed",
            0,
        ));
        return (checks, false, false);
    }

    let began = Instant::now();
    let point = env.restore_point();
    let (status, detail) = match &point {
        Ok(text) => (Status::Pass, text.clone()),
        Err(reason) => (Status::Info, reason.clone()),
    };
    checks.push(check("safety.restore_point", "System restore point created", status, &detail, began));
    (checks, true, point.is_ok())
}

/// Why the cycles are not run, for the report.
fn skip_reason(safety: &[Check]) -> String {
    match safety.iter().find(|c| c.id == "safety.registry_export" && c.status == Status::Fail) {
        Some(failed) => format!("the registry could not be saved first: {}", failed.detail),
        None => "needs administrator rights, an Ethernet or Wi-Fi adapter and a writable backup folder".to_string(),
    }
}

fn skipped_cycle(profile: &Profile, reason: &str) -> Cycle {
    Cycle {
        profile: profile.id.clone(),
        checks: vec![Check::new("cycle", "Apply and revert", Status::Skip, reason, 0)],
    }
}

/// The checks about the computer, and whether it is fit to write to.
fn environment_checks(env: &dyn Env, adapters: &[EnvAdapter]) -> (Vec<Check>, bool) {
    let mut checks = Vec::new();

    let began = Instant::now();
    let elevated = env.elevated();
    checks.push(check(
        "admin",
        "Running with administrator rights",
        pass_or_fail(elevated),
        if elevated { "yes" } else { "no: applying and reverting need it" },
        began,
    ));

    let began = Instant::now();
    checks.push(check(
        "windows",
        "Windows build",
        Status::Info,
        &format!("build {}", env.windows_build()),
        began,
    ));

    let began = Instant::now();
    checks.push(check(
        "adapters",
        "Ethernet or Wi-Fi adapters found",
        pass_or_fail(!adapters.is_empty()),
        &format!("{} adapter(s)", adapters.len()),
        began,
    ));

    let began = Instant::now();
    let writable = backup_folder_writable(&env.backup_dir());
    checks.push(check(
        "backup_dir",
        "Backup folder is writable",
        pass_or_fail(writable.is_ok()),
        &writable.clone().err().unwrap_or_else(|| "yes".to_string()),
        began,
    ));

    let began = Instant::now();
    checks.push(check(
        "internet",
        "DNS resolves now",
        pass_or_fail(dns::internet_works(env.resolver())),
        "",
        began,
    ));

    let can_write = elevated && !adapters.is_empty() && writable.is_ok();
    (checks, can_write)
}

fn backup_folder_writable(dir: &std::path::Path) -> Result<(), String> {
    std::fs::create_dir_all(dir).map_err(|error| format!("cannot create the folder: {error}"))?;
    let probe = dir.join(".selftest-write-test");
    std::fs::write(&probe, b"x").map_err(|error| format!("cannot write there: {error}"))?;
    let _ = std::fs::remove_file(&probe);
    Ok(())
}

/// Read-only checks: every setting readable, every profile plannable, and
/// planning changes nothing.
fn reading_checks(
    env: &dyn Env,
    profiles: &[Profile],
    adapters: &[EnvAdapter],
    guids: &[String],
    dns_ok: bool,
) -> Vec<Check> {
    let store = env.store();
    let before = snapshot(store, adapters);
    let mut checks = Vec::new();

    for setting in CATALOG {
        let id = format!("read.{}", setting.id);
        let title = format!("Read: {}", setting.name);
        let began = Instant::now();
        if setting.id == plan::DNS_SETTING && !dns_ok {
            checks.push(check(&id, &title, Status::Skip, "this Windows version has no encrypted DNS", began));
            continue;
        }
        let places: Vec<Option<&str>> = match setting.scope {
            Scope::System => vec![None],
            Scope::Adapter => guids.iter().map(|guid| Some(guid.as_str())).collect(),
        };
        if places.is_empty() {
            checks.push(check(&id, &title, Status::Skip, "no adapters", began));
            continue;
        }
        let unreadable: Vec<String> = places
            .iter()
            .filter_map(|place| match store.read(setting, *place) {
                State::Unavailable(reason) => {
                    Some(format!("{}: {reason}", place.map_or("system".to_string(), mask_guid)))
                }
                _ => None,
            })
            .collect();
        checks.push(check(
            &id,
            &title,
            pass_or_fail(unreadable.is_empty()),
            &if unreadable.is_empty() { "readable".to_string() } else { unreadable.join("; ") },
            began,
        ));
    }

    for profile in profiles {
        let began = Instant::now();
        let (planned, _dropped) = plan::plan_profile(store, profile, guids, dns_ok);
        let needed = planned.iter().filter(|change| change.is_needed()).count();
        let (status, detail) = match apply::check(&planned) {
            Ok(()) => (Status::Pass, format!("{needed} change(s) would be made")),
            Err(reason) => (Status::Fail, reason),
        };
        checks.push(check(&format!("plan.{}", profile.id), &format!("Plan: {}", profile.name), status, &detail, began));
    }

    let began = Instant::now();
    let after = snapshot(store, adapters);
    checks.push(check(
        "plan.writes_nothing",
        "Reading and planning changed nothing",
        pass_or_fail(before == after),
        "",
        began,
    ));
    checks
}

/// An Info check when DoT was tried and did not work (the labels end in "(DoT)").
fn push_dot_info(checks: &mut Vec<Check>, labels: &[String], began: Instant) {
    let failed: Vec<&str> = labels.iter().map(String::as_str).filter(|l| l.ends_with("(DoT)")).collect();
    if !failed.is_empty() {
        checks.push(check(
            "dns.dot",
            "DoT (experimental)",
            Status::Info,
            &format!("DoT was tried and did not work: {}", failed.join(", ")),
            began,
        ));
    }
}

struct CycleResult {
    cycle: Cycle,
    revert_failed: bool,
    /// The backup of a profile that is still applied because its revert failed.
    active: Option<PathBuf>,
}

/// One profile: apply, verify, DNS trial, internet, revert, compare.
fn cycle(
    env: &dyn Env,
    profile: &Profile,
    adapters: &[EnvAdapter],
    guids: &[String],
    dns_ok: bool,
    created: &mut Vec<PathBuf>,
    say: &mut dyn FnMut(&str),
) -> CycleResult {
    let store = env.store();
    let resolver = env.resolver();
    let mut checks: Vec<Check> = Vec::new();
    let done = |checks: Vec<Check>, revert_failed: bool| CycleResult {
        cycle: Cycle { profile: profile.id.clone(), checks },
        revert_failed,
        active: None,
    };
    let before = snapshot(store, adapters);

    let (planned, _dropped) = plan::plan_profile(store, profile, guids, dns_ok);
    let dns_needed = planned.iter().filter(|c| plan::is_dns(c) && c.is_needed()).count();
    let began = Instant::now();
    let precheck = if dns_needed > 0 {
        dns::precheck(resolver, env.windows_build())
    } else {
        Precheck::Go
    };
    if let Some(text) = dns::precheck_message(&precheck) {
        checks.push(check("dns.precheck", "DNS precheck", Status::Info, &text, began));
    }
    let to_apply = plan::plan_for_precheck(&planned, &precheck);
    let needed = to_apply.iter().filter(|change| change.is_needed()).count();
    if needed == 0 {
        checks.push(Check::new(
            "apply",
            "Apply",
            Status::Skip,
            "every setting already matches this profile",
            0,
        ));
        return done(checks, false);
    }

    let began = Instant::now();
    let applied = apply::apply(
        store,
        &env.backup_dir(),
        &format!("selftest {}", profile.id),
        &env.timestamp(),
        &to_apply,
    );
    let backup_path = match applied {
        Ok(Some(path)) => {
            created.push(path.clone());
            env.mark(Marker::Active(path.clone()));
            checks.push(check("apply", "Apply", Status::Pass, &format!("{needed} change(s) written"), began));
            path
        }
        Ok(None) => {
            checks.push(check("apply", "Apply", Status::Skip, "nothing to change", began));
            return done(checks, false);
        }
        Err(ApplyError::Refused(reason)) => {
            checks.push(check("apply", "Apply", Status::Fail, &format!("refused: {reason}"), began));
            return done(checks, false);
        }
        Err(ApplyError::RolledBack { cause, leftovers }) => {
            let detail = if leftovers.is_empty() {
                format!("{cause}; everything written was undone")
            } else {
                format!("{cause}; could NOT undo: {}", leftovers.join("; "))
            };
            checks.push(check("apply", "Apply", Status::Fail, &detail, began));
            return done(checks, !leftovers.is_empty());
        }
    };

    // The values the engine wrote read back as asked (DNS is checked below).
    let began = Instant::now();
    let wrong: Vec<&str> = to_apply
        .iter()
        .filter(|change| change.is_needed() && !plan::is_dns(change))
        .filter(|change| {
            let actual = store.read(change.target.setting, change.target.adapter.as_deref());
            !reader::matches(&change.target.desired, &actual)
        })
        .map(|change| change.target.setting.id)
        .collect();
    checks.push(check(
        "verify",
        "Written values read back as asked",
        pass_or_fail(wrong.is_empty()),
        &if wrong.is_empty() { "yes".to_string() } else { wrong.join(", ") },
        began,
    ));

    if let (true, Some(spec)) = (to_apply.iter().any(plan::is_dns), profile.dns.as_ref()) {
        let (scope, originals) = plan::dns_trial_scope(&to_apply);
        let began = Instant::now();
        let outcome = dns::run(
            store,
            resolver,
            spec,
            &scope,
            &originals,
            Some((env.dot_backend(), &backup_path)),
            &mut *say,
        );
        match &outcome {
            Outcome::Encrypted { provider, method, skipped } => {
                let detail = if skipped.is_empty() {
                    format!("{provider} works")
                } else {
                    format!("{provider} works; {} did not", skipped.join(", "))
                };
                checks.push(check("dns.trial", "Encrypted DNS trial", Status::Pass, &detail, began));
                if *method == dns::Method::Dot {
                    checks.push(check(
                        "dns.dot",
                        "DoT (experimental)",
                        Status::Pass,
                        &format!("DoT (experimental) works with {provider}"),
                        began,
                    ));
                } else {
                    push_dot_info(&mut checks, skipped, began);
                }

                let began = Instant::now();
                let want = provider::PROVIDERS
                    .iter()
                    .find(|candidate| candidate.name == provider)
                    .map(|candidate| dns::state_for(candidate, *method));
                let dns_setting = catalog::find(plan::DNS_SETTING);
                let bad: Vec<String> = match (want, dns_setting) {
                    (Some(want), Some(setting)) => scope
                        .iter()
                        .filter(|adapter| store.read(setting, Some(adapter)) != want)
                        .map(|adapter| mask_guid(adapter))
                        .collect(),
                    _ => vec!["provider unknown".to_string()],
                };
                checks.push(check(
                    "dns.readback",
                    "Encrypted DNS settings read back from Windows",
                    pass_or_fail(bad.is_empty()),
                    &if bad.is_empty() { "yes".to_string() } else { format!("differs on {}", bad.join(", ")) },
                    began,
                ));
            }
            Outcome::FellBack { tried, internet_ok } => {
                checks.push(check(
                    "dns.trial",
                    "Encrypted DNS trial",
                    Status::Info,
                    &format!("this network did not allow encrypted DNS (tried {}); DNS was put back", tried.join(", ")),
                    began,
                ));
                push_dot_info(&mut checks, tried, began);
                if !internet_ok {
                    checks.push(check(
                        "dns.internet_after_fallback",
                        "DNS resolves after the fallback",
                        Status::Fail,
                        "no",
                        began,
                    ));
                }
            }
            Outcome::ProgramError(reason) => {
                checks.push(check("dns.trial", "Encrypted DNS trial", Status::Fail, reason, began));
            }
        }
    }

    let began = Instant::now();
    checks.push(check(
        "internet.after_apply",
        "DNS resolves while the profile is applied",
        pass_or_fail(dns::internet_works(resolver)),
        "",
        began,
    ));

    // Always revert once something was written, whatever failed above.
    let began = Instant::now();
    let reverted = match backup::load(&backup_path) {
        Ok(saved) => {
            let mut errors = apply::revert(store, &saved);
            if errors.is_empty() {
                match env.restore_dot(&backup_path) {
                    Ok(problems) => errors = problems,
                    Err(reason) => errors.push(reason),
                }
            }
            if errors.is_empty() {
                let _ = backup::mark_reverted(&backup_path);
                Ok(())
            } else {
                Err(errors.join("; "))
            }
        }
        Err(reason) => Err(reason),
    };
    checks.push(check(
        "revert",
        "Revert",
        pass_or_fail(reverted.is_ok()),
        &reverted.clone().err().unwrap_or_else(|| "done".to_string()),
        began,
    ));
    if reverted.is_err() {
        let mut result = done(checks, true);
        result.active = Some(backup_path);
        return result;
    }
    env.mark(Marker::Running);

    let began = Instant::now();
    let differing = differences(&before, &snapshot(store, adapters));
    checks.push(check(
        "restored",
        "Every setting is as it was before this profile",
        pass_or_fail(differing.is_empty()),
        &if differing.is_empty() { "yes".to_string() } else { differing.join("; ") },
        began,
    ));
    let began = Instant::now();
    checks.push(check(
        "internet.after_revert",
        "DNS resolves after the revert",
        pass_or_fail(dns::internet_works(resolver)),
        "",
        began,
    ));
    done(checks, false)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::settings::backup::testing::temp_dir;
    use crate::settings::catalog::CATALOG;
    use crate::settings::profile;
    use crate::settings::reader::Value;
    use crate::selftest::files::Marker;
    use crate::settings::store::testing::FakeStore;
    use std::cell::{Cell, RefCell};
    use std::rc::Rc;
    use std::time::Duration;

    const A: &str = "{AAAAAAAA-0000-0000-0000-000000000001}";
    const B: &str = "{BBBBBBBB-0000-0000-0000-000000000002}";

    /// Resolves according to what the fake store says adapter A's DNS is.
    struct FakeResolver {
        store: Rc<FakeStore>,
        plain_works: bool,
        works_for: Vec<&'static str>,
        /// Internet "breaks" while the LLMNR policy is disabled.
        break_on_llmnr: bool,
        /// Lookups over DoH fail (plain and DoT-style entries still resolve).
        doh_broken: bool,
        on_lookup: Option<Box<dyn Fn()>>,
    }

    impl Resolver for FakeResolver {
        fn lookup(&self, name: &str) -> Result<Duration, String> {
            if let Some(hook) = &self.on_lookup {
                hook();
            }
            let llmnr = self.store.values.borrow().get(&("dns.llmnr".to_string(), None)).cloned();
            if self.break_on_llmnr && llmnr == Some(State::Value(Value::Dword(0))) {
                return Err(format!("{name}: simulated outage"));
            }
            let dns = catalog::find("dns.resolver").unwrap();
            match self.store.read(dns, Some(A)) {
                State::NotSet if self.plain_works => Ok(Duration::from_millis(10)),
                State::Value(Value::Dns(servers))
                    if self.doh_broken && servers[0].doh.is_some() =>
                {
                    Err(format!("{name}: simulated DoH failure"))
                }
                State::Value(Value::Dns(servers))
                    if self.works_for.contains(&servers[0].address.as_str()) =>
                {
                    Ok(Duration::from_millis(10))
                }
                _ => Err(format!("{name}: simulated failure")),
            }
        }

        fn flush_cache(&self) {}
    }

    struct FakeEnv {
        store: Rc<FakeStore>,
        resolver: FakeResolver,
        adapters: Vec<EnvAdapter>,
        elevated: bool,
        dir: PathBuf,
        marker: RefCell<Vec<Marker>>,
        /// What `doh_entries` returns on successive calls (empty when used up).
        doh: RefCell<std::collections::VecDeque<Vec<String>>>,
        export: Result<Vec<String>, String>,
        restore: Result<String, String>,
        finished: Cell<u32>,
        dot: FakeDot,
        /// What `dot_entries` returns on successive calls (empty when used up).
        dot_list: RefCell<std::collections::VecDeque<Vec<String>>>,
        /// Replaces the real sidecar restore, to simulate a failing one.
        dot_restore: Option<Result<Vec<String>, String>>,
    }

    /// An in-memory DoT list.
    #[derive(Default)]
    struct FakeDot(RefCell<std::collections::HashMap<String, crate::dns::dot::EntryValues>>);

    impl DotBackend for FakeDot {
        fn read(&self, address: &str) -> Result<Option<crate::dns::dot::EntryValues>, String> {
            Ok(self.0.borrow().get(address).cloned())
        }
        fn add(&self, address: &str, host: &str) -> Result<(), String> {
            self.0.borrow_mut().insert(address.to_string(), vec![("DotHost".into(), host.into())]);
            Ok(())
        }
        fn restore(&self, address: &str, saved: &Option<crate::dns::dot::EntryValues>) -> Result<(), String> {
            match saved {
                Some(values) => self.0.borrow_mut().insert(address.to_string(), values.clone()),
                None => self.0.borrow_mut().remove(address),
            };
            Ok(())
        }
    }

    impl Env for FakeEnv {
        fn store(&self) -> &dyn Store {
            &*self.store
        }
        fn resolver(&self) -> &dyn Resolver {
            &self.resolver
        }
        fn dot_backend(&self) -> &dyn DotBackend {
            &self.dot
        }
        fn adapters(&self) -> Vec<EnvAdapter> {
            self.adapters.iter().map(|a| EnvAdapter { guid: a.guid.clone() }).collect()
        }
        fn windows_build(&self) -> u32 {
            26200
        }
        fn elevated(&self) -> bool {
            self.elevated
        }
        fn backup_dir(&self) -> PathBuf {
            self.dir.clone()
        }
        fn timestamp(&self) -> String {
            "20261117-120000".to_string()
        }
        fn mark(&self, marker: Marker) {
            self.marker.borrow_mut().push(marker);
        }
        fn doh_entries(&self) -> Vec<String> {
            self.doh.borrow_mut().pop_front().unwrap_or_default()
        }
        fn dot_entries(&self) -> Vec<String> {
            self.dot_list.borrow_mut().pop_front().unwrap_or_default()
        }
        fn restore_dot(&self, backup_path: &std::path::Path) -> Result<Vec<String>, String> {
            match &self.dot_restore {
                Some(result) => result.clone(),
                None => crate::dns::dot::restore_sidecar(&self.dot, backup_path),
            }
        }
        fn restore_point(&self) -> Result<String, String> {
            self.restore.clone()
        }
        fn export_registry(&self) -> Result<Vec<String>, String> {
            self.export.clone()
        }
        fn finish_restore_point(&self) {
            self.finished.set(self.finished.get() + 1);
        }
    }

    fn env_with(name: &str, store: FakeStore, adapters: &[&str]) -> FakeEnv {
        let store = Rc::new(store);
        FakeEnv {
            resolver: FakeResolver {
                store: Rc::clone(&store),
                plain_works: true,
                works_for: vec!["9.9.9.9", "1.1.1.1", "8.8.8.8"],
                break_on_llmnr: false,
                doh_broken: false,
                on_lookup: None,
            },
            store,
            adapters: adapters
                .iter()
                .map(|guid| EnvAdapter { guid: guid.to_string() })
                .collect(),
            elevated: true,
            dir: temp_dir(name),
            marker: RefCell::new(Vec::new()),
            doh: RefCell::new(Default::default()),
            export: Ok(vec!["Dnscache.reg".to_string(); 7]),
            restore: Ok("created".to_string()),
            finished: Cell::new(0),
            dot: FakeDot::default(),
            dot_list: RefCell::new(Default::default()),
            dot_restore: None,
        }
    }

    fn env(name: &str) -> FakeEnv {
        env_with(name, FakeStore::default(), &[A, B])
    }

    fn run_it(env: &FakeEnv) -> Report {
        let profiles = profile::builtin().unwrap();
        run(env, &profiles, Diagnostics::default(), &mut |_| {})
    }

    fn cycle<'a>(report: &'a Report, profile: &str) -> &'a Cycle {
        report.cycles.iter().find(|c| c.profile == profile).expect("cycle present")
    }

    fn status(cycle: &Cycle, id: &str) -> Status {
        cycle.checks.iter().find(|c| c.id == id).unwrap_or_else(|| panic!("no check {id}")).status
    }

    fn failures(report: &Report) -> Vec<String> {
        report.all_checks().filter(|c| c.status == Status::Fail).map(|c| c.id.clone()).collect()
    }

    #[test]
    fn all_green_run_applies_and_reverts_every_profile() {
        let env = env("selftest-green");
        let before = env.store.snapshot();

        let report = run_it(&env);

        assert_eq!(report.cycles.len(), 3);
        assert!(failures(&report).is_empty(), "{:?}", failures(&report));
        assert!(report.all_restored);
        assert_eq!(env.store.snapshot(), before, "the system ends as it started");
        assert_eq!(report.schema, Report::SCHEMA);
    }

    #[test]
    fn failed_apply_is_reported_and_the_next_profile_still_runs() {
        let store = FakeStore {
            fail_id: Some("service.diagtrack"),
            failures_left: Cell::new(1),
            ..Default::default()
        };
        let env = env_with("selftest-apply-fail", store, &[A, B]);

        let report = run_it(&env);

        assert_eq!(status(cycle(&report, "privacy"), "apply"), Status::Fail);
        assert_eq!(status(cycle(&report, "performance"), "apply"), Status::Pass);
        assert_eq!(status(cycle(&report, "recommended"), "apply"), Status::Pass);
        assert!(report.all_restored);
    }

    #[test]
    fn failed_revert_stops_and_marks_nothing_restored() {
        let probe = FakeStore::default();
        let guids = [A.to_string(), B.to_string()];
        let privacy = profile::builtin().unwrap().into_iter().find(|p| p.id == "privacy").unwrap();
        let (plan, _) = plan::plan_profile(&probe, &privacy, &guids, true);
        let needed = plan.iter().filter(|change| change.is_needed()).count() as u32;

        // The apply uses exactly `needed` writes; every write after that fails.
        let store = FakeStore { fail_after_writes: Some(needed), ..Default::default() };
        let env = env_with("selftest-revert-fail", store, &[A, B]);

        let report = run_it(&env);

        assert_eq!(report.cycles.len(), 1, "later profiles are not run");
        assert_eq!(status(cycle(&report, "privacy"), "revert"), Status::Fail);
        assert!(!report.all_restored);
        assert_eq!(env.resolver.store.writes.get(), needed, "no write after the failed revert");
    }

    #[test]
    fn difference_after_revert_is_a_failure() {
        let mut env = env("selftest-difference");
        // Something outside the cycle changes during the run and is never restored.
        let store = Rc::clone(&env.store);
        // Only while a profile is applied (LLMNR is then disabled), never before
        // the start snapshot is taken.
        env.resolver.on_lookup = Some(Box::new(move || {
            let applied = store.values.borrow().contains_key(&("dns.llmnr".to_string(), None));
            if applied {
                store.seed("tcp.nagle.no_delay", Some(A), State::Value(Value::Dword(1)));
            }
        }));

        let report = run_it(&env);

        assert!(!report.all_restored);
        assert!(report.closing.iter().any(|c| c.status == Status::Fail), "{:?}", report.closing);
    }

    #[test]
    fn internet_lost_after_apply_fails_the_check_but_still_reverts() {
        let mut env = env("selftest-outage");
        env.resolver.break_on_llmnr = true;

        let report = run_it(&env);

        let privacy = cycle(&report, "privacy");
        assert_eq!(status(privacy, "internet.after_apply"), Status::Fail);
        assert_eq!(status(privacy, "revert"), Status::Pass);
        assert_eq!(status(privacy, "internet.after_revert"), Status::Pass);
        assert!(report.all_restored);
    }

    #[test]
    fn dns_fallback_is_info_not_fail() {
        let mut env = env("selftest-fallback");
        env.resolver.works_for = vec![]; // no provider resolves; plain DHCP DNS does

        let report = run_it(&env);

        for name in ["privacy", "performance", "recommended"] {
            assert_eq!(status(cycle(&report, name), "dns.trial"), Status::Info, "{name}");
        }
        assert!(failures(&report).is_empty(), "{:?}", failures(&report));
        assert!(report.all_restored);
    }

    #[test]
    fn no_adapters_skips_the_cycles_without_writing() {
        let env = env_with("selftest-none", FakeStore::default(), &[]);

        let report = run_it(&env);

        assert_eq!(env.store.writes.get(), 0);
        assert_eq!(report.cycles.len(), 3);
        assert!(report.cycles.iter().all(|c| c.checks.iter().all(|k| k.status == Status::Skip)));
        let adapters_check = report.environment.iter().find(|c| c.id == "adapters").unwrap();
        assert_eq!(adapters_check.status, Status::Fail);
    }

    #[test]
    fn not_elevated_skips_the_cycles_without_writing() {
        let mut env = env("selftest-not-admin");
        env.elevated = false;

        let report = run_it(&env);

        assert_eq!(env.store.writes.get(), 0);
        assert!(report.cycles.iter().all(|c| c.checks.iter().all(|k| k.status == Status::Skip)));
    }

    fn safety<'a>(report: &'a Report, id: &str) -> &'a Check {
        report.safety.iter().find(|c| c.id == id).unwrap_or_else(|| panic!("no check {id}"))
    }

    #[test]
    fn safety_checks_pass_when_both_work() {
        let env = env("selftest-safety-ok");

        let report = run_it(&env);

        assert_eq!(safety(&report, "safety.registry_export").status, Status::Pass);
        assert!(safety(&report, "safety.registry_export").detail.contains('7'));
        assert_eq!(safety(&report, "safety.restore_point").status, Status::Pass);
        assert!(env.store.writes.get() > 0, "cycles ran");
        assert_eq!(status(cycle(&report, "privacy"), "apply"), Status::Pass);
    }

    #[test]
    fn export_failure_blocks_all_writes() {
        let mut env = env("selftest-safety-export-fail");
        env.export = Err("reg.exe failed".to_string());

        let report = run_it(&env);

        assert_eq!(env.store.writes.get(), 0);
        assert_eq!(safety(&report, "safety.registry_export").status, Status::Fail);
        assert!(safety(&report, "safety.registry_export").detail.contains("reg.exe failed"));
        assert!(report.cycles.iter().all(|c| c.checks.iter().all(|k| k.status == Status::Skip)));
        assert!(report.all_restored);
        assert_eq!(env.finished.get(), 0);
    }

    #[test]
    fn restore_point_failure_is_info_and_does_not_block() {
        let mut env = env("selftest-safety-rp-fail");
        env.restore = Err("System Protection is off".to_string());

        let report = run_it(&env);

        let point = safety(&report, "safety.restore_point");
        assert_eq!(point.status, Status::Info);
        assert!(point.detail.contains("System Protection is off"));
        assert_eq!(status(cycle(&report, "privacy"), "apply"), Status::Pass);
        assert_eq!(env.finished.get(), 0);
    }

    #[test]
    fn restore_point_is_finished_after_the_run_only_if_it_began() {
        let env = env("selftest-safety-finish");
        run_it(&env);
        assert_eq!(env.finished.get(), 1);
    }

    #[test]
    fn nothing_is_exported_when_the_run_cannot_write_anyway() {
        let mut env = env("selftest-safety-not-admin");
        env.elevated = false;
        let report = run_it(&env);
        assert!(report.safety.is_empty());
        assert_eq!(env.finished.get(), 0);
    }

    fn privacy_writes() -> u32 {
        let probe = FakeStore::default();
        let guids = [A.to_string(), B.to_string()];
        let privacy = profile::builtin().unwrap().into_iter().find(|p| p.id == "privacy").unwrap();
        let (plan, _) = plan::plan_profile(&probe, &privacy, &guids, true);
        plan.iter().filter(|change| change.is_needed()).count() as u32
    }

    #[test]
    fn marker_is_set_during_the_run_and_cleared_after() {
        let env = env("selftest-marker");
        run_it(&env);
        let marks = env.marker.borrow();
        assert_eq!(marks.first(), Some(&Marker::Running));
        assert!(marks.iter().any(|m| matches!(m, Marker::Active(_))), "the active backup is named");
        assert_eq!(marks.last(), Some(&Marker::Clear));
    }

    #[test]
    fn marker_names_the_backup_when_the_revert_fails() {
        let store = FakeStore { fail_after_writes: Some(privacy_writes()), ..Default::default() };
        let env = env_with("selftest-marker-stop", store, &[A, B]);
        run_it(&env);
        let marks = env.marker.borrow();
        assert!(matches!(marks.last(), Some(Marker::Active(_))), "{marks:?}");
    }

    #[test]
    fn marker_says_not_restored_when_only_a_difference_remains() {
        let mut env = env("selftest-marker-diff");
        let store = Rc::clone(&env.store);
        env.resolver.on_lookup = Some(Box::new(move || {
            if store.values.borrow().contains_key(&("dns.llmnr".to_string(), None)) {
                store.seed("tcp.nagle.no_delay", Some(A), State::Value(Value::Dword(1)));
            }
        }));
        run_it(&env);
        assert_eq!(env.marker.borrow().last(), Some(&Marker::NotRestored));
    }

    #[test]
    fn closing_fails_when_doh_entries_remain_after_the_run() {
        let env = env("selftest-doh-left");
        env.doh.borrow_mut().push_back(vec![]);
        env.doh.borrow_mut().push_back(vec!["aaaaaaaa/1.1.1.1".to_string()]);

        let report = run_it(&env);

        let check = report.closing.iter().find(|c| c.id == "closing.doh_entries").unwrap();
        assert_eq!(check.status, Status::Fail);
        assert!(!report.all_restored);
    }

    #[test]
    fn closing_fails_when_dot_entries_differ() {
        let env = env("selftest-dot-left");
        env.dot_list.borrow_mut().push_back(vec!["1.1.1.1:absent".to_string()]);
        env.dot_list.borrow_mut().push_back(vec!["1.1.1.1:2 values".to_string()]);

        let report = run_it(&env);

        let check = report.closing.iter().find(|c| c.id == "closing.dot_entries").unwrap();
        assert_eq!(check.status, Status::Fail);
        assert!(!report.all_restored);
    }

    #[test]
    fn dot_restore_failure_fails_the_revert_check_and_keeps_the_marker_active() {
        let mut env = env("selftest-dot-restore-fail");
        env.resolver.doh_broken = true;
        env.dot_restore = Some(Ok(vec!["1.1.x.x: entry differs after restoring".to_string()]));

        let report = run_it(&env);

        let privacy = cycle(&report, "privacy");
        assert_eq!(status(privacy, "revert"), Status::Fail);
        assert_eq!(report.cycles.len(), 1, "later profiles are not run");
        assert!(!report.all_restored);
        assert!(matches!(env.marker.borrow().last(), Some(Marker::Active(_))), "{:?}", env.marker.borrow());
        let kept = env.dir.read_dir().unwrap().filter_map(Result::ok).any(|e| {
            let name = e.file_name().to_string_lossy().into_owned();
            name.ends_with(".json") && !name.contains("reverted") && !name.contains(".dot.")
        });
        assert!(kept, "the backup is not marked reverted");
    }

    #[test]
    fn dot_success_is_recorded_in_its_own_check() {
        let mut env = env("selftest-dot-ok");
        env.resolver.doh_broken = true;

        let report = run_it(&env);

        let privacy = cycle(&report, "privacy");
        assert_eq!(status(privacy, "dns.trial"), Status::Pass);
        let dot = privacy.checks.iter().find(|c| c.id == "dns.dot").unwrap();
        assert_eq!(dot.status, Status::Pass);
        assert!(dot.detail.contains("DoT (experimental) works with"), "{}", dot.detail);
        assert!(failures(&report).is_empty(), "{:?}", failures(&report));
        assert!(report.all_restored);
    }

    #[test]
    fn dot_that_did_not_work_is_info() {
        let mut env = env("selftest-dot-info");
        env.resolver.works_for = vec![];

        let report = run_it(&env);

        let privacy = cycle(&report, "privacy");
        let dot = privacy.checks.iter().find(|c| c.id == "dns.dot").unwrap();
        assert_eq!(dot.status, Status::Info);
        assert!(dot.detail.contains("DoT was tried and did not work"), "{}", dot.detail);
        assert!(report.all_restored);
    }

    #[test]
    fn all_green_run_still_passes_with_the_dot_hooks() {
        let env = env("selftest-dot-green");
        let report = run_it(&env);
        assert!(failures(&report).is_empty(), "{:?}", failures(&report));
        assert!(report.all_restored);
        assert!(report.closing.iter().any(|c| c.id == "closing.dot_entries" && c.status == Status::Pass));
        assert!(report.cycles.iter().all(|c| c.checks.iter().all(|k| k.id != "dns.dot")));
    }

    #[test]
    fn snapshot_covers_every_catalog_setting_per_adapter() {
        let store = FakeStore::default();
        let adapters = [
            EnvAdapter { guid: A.into() },
            EnvAdapter { guid: B.into() },
        ];
        let taken = snapshot(&store, &adapters);
        let system = CATALOG.iter().filter(|s| s.scope == catalog::Scope::System).count();
        let per_adapter = CATALOG.iter().filter(|s| s.scope == catalog::Scope::Adapter).count();
        assert_eq!(taken.0.len(), system + per_adapter * 2);
    }

    #[test]
    fn differences_ignores_a_service_running_state_change() {
        let store = FakeStore::default();
        let adapters: [EnvAdapter; 0] = [];
        let service = |state| State::Value(Value::Service { state, start_type: 4 });
        store.seed("service.diagtrack", None, service(1));
        let before = snapshot(&store, &adapters);
        store.seed("service.diagtrack", None, service(4));
        let after = snapshot(&store, &adapters);
        assert!(differences(&before, &after).is_empty());

        store.seed("service.diagtrack", None, State::Value(Value::Service { state: 4, start_type: 2 }));
        let changed = snapshot(&store, &adapters);
        let found = differences(&before, &changed);
        assert_eq!(found.len(), 1, "{found:?}");
        assert!(found[0].contains("service.diagtrack"), "{found:?}");
    }
}
