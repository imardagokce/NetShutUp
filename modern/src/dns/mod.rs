//! The encrypted DNS trial: after a profile has pointed the adapters at its
//! first provider, prove that it works, move on to the next provider if not,
//! and put the original DNS back if nothing works. The logic only talks to a
//! `Store` and a `Resolver`, so it is tested with fakes.

pub mod resolver;

use std::time::Duration;

use crate::settings::catalog::{self, Setting};
use crate::settings::profile::{Choose, DnsSpec};
use crate::settings::provider::{self, Provider};
use crate::settings::reader::{self, State};
use crate::settings::store::Store;

/// Windows 11. Earlier Windows 10 builds have no usable encrypted DNS.
const FIRST_BUILD_WITH_DOH: u32 = 22000;

/// Names tried, in order, to see whether DNS works at all; one success is enough.
const TEST_NAMES: [&str; 2] = ["www.microsoft.com", "example.com"];

/// Names timed to compare providers: one warm-up, then five timed lookups.
const MEASURE_NAMES: [&str; 5] =
    ["www.microsoft.com", "example.com", "www.wikipedia.org", "www.apple.com", "www.cloudflare.com"];

pub const REPORT_URL: &str = "https://github.com/imardagokce/NetShutUp/security/advisories/new";

pub trait Resolver {
    /// Resolves `name`, bypassing the DNS cache and the hosts file. Returns
    /// how long it took.
    fn lookup(&self, name: &str) -> Result<Duration, String>;
    fn flush_cache(&self);
}

#[derive(Debug, PartialEq, Eq)]
pub enum Precheck {
    Go,
    /// DNS does not work right now, so nothing could be proven; do not touch it.
    NoInternet,
    /// This Windows version has no encrypted DNS.
    Unsupported,
}

/// Whether the DNS change can go ahead. Checks the Windows build first and
/// only then looks at the network.
pub fn precheck(resolver: &dyn Resolver, windows_build: u32) -> Precheck {
    if windows_build < FIRST_BUILD_WITH_DOH {
        Precheck::Unsupported
    } else if TEST_NAMES.iter().any(|name| resolver.lookup(name).is_ok()) {
        Precheck::Go
    } else {
        Precheck::NoInternet
    }
}

#[derive(Debug, PartialEq, Eq)]
pub enum Outcome {
    /// Encrypted DNS works. `skipped` are the providers that did not, by name.
    Encrypted { provider: String, skipped: Vec<String> },
    /// None worked; DNS is back as it was.
    FellBack { tried: Vec<String> },
    /// NetShutUp itself failed; DNS was put back where possible.
    ProgramError(String),
}

/// Runs the trial. The apply engine has already written the first provider of
/// `spec` to `adapters`; `originals` holds what each adapter had before.
pub fn run(
    store: &dyn Store,
    resolver: &dyn Resolver,
    spec: &DnsSpec,
    adapters: &[String],
    originals: &[(String, State)],
    say: &mut dyn FnMut(&str),
) -> Outcome {
    let Some(setting) = catalog::find("dns.resolver") else {
        return Outcome::ProgramError("dns.resolver is missing from the catalog".to_string());
    };
    let providers: Vec<&Provider> =
        spec.providers.iter().filter_map(|id| provider::find(id)).collect();
    if providers.is_empty() {
        return Outcome::ProgramError("the profile names no known DNS provider".to_string());
    }

    let mut skipped: Vec<String> = Vec::new();
    let mut working: Vec<(usize, Duration)> = Vec::new();
    let mut last_written = 0;

    for (index, provider) in providers.iter().enumerate() {
        if index > 0 {
            if let Err(reason) = write_all(store, setting, adapters, &provider.state()) {
                return fail(store, resolver, setting, originals, reason);
            }
            last_written = index;
        }
        resolver.flush_cache();
        say(&format!("Trying {} (encrypted DNS)...", provider.name));

        if !TEST_NAMES.iter().any(|name| resolver.lookup(name).is_ok()) {
            skipped.push(provider.name.to_string());
            continue;
        }
        match spec.choose {
            Choose::FirstWorking => {
                return Outcome::Encrypted { provider: provider.name.to_string(), skipped };
            }
            Choose::Fastest => {
                let time = measure(resolver).unwrap_or(Duration::MAX);
                say(&format!("  {}: {}", provider.name, describe(time)));
                working.push((index, time));
            }
        }
    }

    if let Some(&(best, _)) = working.iter().min_by_key(|(_, time)| *time) {
        if best != last_written {
            if let Err(reason) = write_all(store, setting, adapters, &providers[best].state()) {
                return fail(store, resolver, setting, originals, reason);
            }
            resolver.flush_cache();
        }
        return Outcome::Encrypted { provider: providers[best].name.to_string(), skipped };
    }

    let tried = providers.iter().map(|provider| provider.name.to_string()).collect();
    match restore(store, resolver, setting, originals) {
        Ok(()) => Outcome::FellBack { tried },
        Err(reason) => Outcome::ProgramError(reason),
    }
}

/// Writes `desired` to every adapter and checks that Windows kept it as written.
fn write_all(
    store: &dyn Store,
    setting: &Setting,
    adapters: &[String],
    desired: &State,
) -> Result<(), String> {
    for adapter in adapters {
        store
            .write(setting, Some(adapter), desired)
            .map_err(|reason| format!("{adapter}: {reason}"))?;
        let actual = store.read(setting, Some(adapter));
        if !reader::matches(desired, &actual) {
            return Err(format!("{adapter}: value did not take effect (now {actual:?})"));
        }
    }
    Ok(())
}

/// A program error: puts the originals back, and says so if that fails too.
fn fail(
    store: &dyn Store,
    resolver: &dyn Resolver,
    setting: &Setting,
    originals: &[(String, State)],
    reason: String,
) -> Outcome {
    match restore(store, resolver, setting, originals) {
        Ok(()) => Outcome::ProgramError(reason),
        Err(left_over) => Outcome::ProgramError(format!("{reason}; {left_over}")),
    }
}

/// Puts every adapter's DNS back as it was. Keeps going after a failure.
fn restore(
    store: &dyn Store,
    resolver: &dyn Resolver,
    setting: &Setting,
    originals: &[(String, State)],
) -> Result<(), String> {
    let mut left_over = Vec::new();
    for (adapter, original) in originals {
        let result = store.write(setting, Some(adapter), original).and_then(|()| {
            let actual = store.read(setting, Some(adapter));
            if reader::matches(original, &actual) {
                Ok(())
            } else {
                Err(format!("still {actual:?} after restoring"))
            }
        });
        if let Err(reason) = result {
            left_over.push(format!("{adapter}: {reason}"));
        }
    }
    resolver.flush_cache();

    if left_over.is_empty() {
        Ok(())
    } else {
        Err(format!("DNS could not be put back on {}", left_over.join("; ")))
    }
}

/// Median time of one warm-up and five timed lookups; `None` if none resolved.
fn measure(resolver: &dyn Resolver) -> Option<Duration> {
    let _ = resolver.lookup(MEASURE_NAMES[0]);
    let mut times: Vec<Duration> =
        MEASURE_NAMES.iter().filter_map(|name| resolver.lookup(name).ok()).collect();
    median(&mut times)
}

/// The middle time; the mean of the two middle ones for an even count.
pub fn median(times: &mut [Duration]) -> Option<Duration> {
    if times.is_empty() {
        return None;
    }
    times.sort();
    let middle = times.len() / 2;
    Some(if times.len() % 2 == 1 { times[middle] } else { (times[middle - 1] + times[middle]) / 2 })
}

fn describe(time: Duration) -> String {
    if time == Duration::MAX { "no timing".to_string() } else { format!("{} ms", time.as_millis()) }
}

/// What to tell the user about a precheck that stopped the DNS change.
pub fn precheck_message(precheck: &Precheck) -> Option<String> {
    match precheck {
        Precheck::Go => None,
        Precheck::NoInternet => {
            Some("No internet connection right now, so DNS was not changed.".to_string())
        }
        Precheck::Unsupported => {
            Some("This Windows version has no encrypted DNS, so DNS was not changed.".to_string())
        }
    }
}

/// What to tell the user about the trial. Only a program error asks for a
/// report; a network that blocks encrypted DNS is not a bug.
pub fn message(outcome: &Outcome) -> String {
    match outcome {
        Outcome::Encrypted { provider, skipped } if skipped.is_empty() => {
            format!("Encrypted DNS is on: {provider} (DoH).")
        }
        Outcome::Encrypted { provider, skipped } => format!(
            "{} did not work on this network; using {provider} instead.\nEncrypted DNS is on: {provider} (DoH).",
            skipped.join(", ")
        ),
        Outcome::FellBack { .. } => "Encrypted DNS did not work on this network, so your DNS was put back as it was. Nothing is broken.".to_string(),
        Outcome::ProgramError(reason) => format!(
            "DNS problem: {reason}\nThis looks like a problem in NetShutUp. Please report it privately: {REPORT_URL}"
        ),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::settings::reader::{DnsServer, Value};
    use crate::settings::store::testing::FakeStore;
    use std::cell::Cell;

    const A: &str = "{A}";
    const B: &str = "{B}";

    fn dns_setting() -> &'static Setting {
        catalog::find("dns.resolver").unwrap()
    }

    fn provider_state(id: &str) -> State {
        provider::find(id).unwrap().state()
    }

    fn static_dns(address: &str) -> State {
        State::Value(Value::Dns(vec![DnsServer { address: address.to_string(), doh: None }]))
    }

    fn spec(providers: &[&str], choose: Choose) -> DnsSpec {
        DnsSpec { providers: providers.iter().map(|id| id.to_string()).collect(), choose }
    }

    fn ms(millis: u64) -> Duration {
        Duration::from_millis(millis)
    }

    /// Does what the apply engine does before `run`: writes the first provider
    /// to both adapters. Returns the adapters and what they had before.
    fn engine_wrote_first(
        store: &FakeStore,
        first: &str,
        before: [State; 2],
    ) -> (Vec<String>, Vec<(String, State)>) {
        for adapter in [A, B] {
            store.seed("dns.resolver", Some(adapter), provider_state(first));
        }
        let [before_a, before_b] = before;
        (
            vec![A.to_string(), B.to_string()],
            vec![(A.to_string(), before_a), (B.to_string(), before_b)],
        )
    }

    /// Resolves only while adapter A's first server is one of `works_for`.
    struct FakeResolver<'a> {
        store: &'a FakeStore,
        works_for: Vec<&'static str>,
        /// Time per lookup, by first server address (default 10 ms).
        durations: Vec<(&'static str, u64)>,
        /// When set, ignores the store: every name resolves (or none does).
        all: Option<bool>,
        failing_names: Vec<&'static str>,
        lookups: Cell<u32>,
    }

    impl<'a> FakeResolver<'a> {
        fn new(store: &'a FakeStore) -> Self {
            Self {
                store,
                works_for: Vec::new(),
                durations: Vec::new(),
                all: None,
                failing_names: Vec::new(),
                lookups: Cell::new(0),
            }
        }

        fn working(store: &'a FakeStore, works_for: &[&'static str]) -> Self {
            Self { works_for: works_for.to_vec(), ..Self::new(store) }
        }
    }

    impl Resolver for FakeResolver<'_> {
        fn lookup(&self, name: &str) -> Result<Duration, String> {
            self.lookups.set(self.lookups.get() + 1);
            if let Some(all) = self.all {
                return if all && !self.failing_names.contains(&name) {
                    Ok(ms(5))
                } else {
                    Err(format!("{name}: simulated failure"))
                };
            }
            let first = match self.store.read(dns_setting(), Some(A)) {
                State::Value(Value::Dns(servers)) => servers[0].address.clone(),
                _ => return Err("no servers".to_string()),
            };
            if !self.works_for.contains(&first.as_str()) {
                return Err(format!("{name}: simulated failure"));
            }
            let millis =
                self.durations.iter().find(|(address, _)| *address == first).map_or(10, |(_, ms)| *ms);
            Ok(ms(millis))
        }

        fn flush_cache(&self) {}
    }

    fn run_quietly(
        store: &FakeStore,
        resolver: &dyn Resolver,
        spec: &DnsSpec,
        adapters: &[String],
        originals: &[(String, State)],
    ) -> Outcome {
        run(store, resolver, spec, adapters, originals, &mut |_| {})
    }

    fn names(list: &[&str]) -> Vec<String> {
        list.iter().map(|name| name.to_string()).collect()
    }

    #[test]
    fn precheck_unsupported_below_build_22000() {
        let store = FakeStore::default();
        let resolver = FakeResolver { all: Some(true), ..FakeResolver::new(&store) };
        assert_eq!(precheck(&resolver, 19045), Precheck::Unsupported);
        assert_eq!(resolver.lookups.get(), 0);
        assert_eq!(precheck(&resolver, 22000), Precheck::Go);
    }

    #[test]
    fn precheck_no_internet_when_all_names_fail() {
        let store = FakeStore::default();
        let resolver = FakeResolver { all: Some(false), ..FakeResolver::new(&store) };
        assert_eq!(precheck(&resolver, 26200), Precheck::NoInternet);
        assert_eq!(resolver.lookups.get(), 2);
    }

    #[test]
    fn precheck_go_when_any_name_resolves() {
        let store = FakeStore::default();
        let resolver = FakeResolver {
            all: Some(true),
            failing_names: vec!["www.microsoft.com"],
            ..FakeResolver::new(&store)
        };
        assert_eq!(precheck(&resolver, 26200), Precheck::Go);
    }

    #[test]
    fn first_working_stops_at_first_provider_that_resolves() {
        let store = FakeStore::default();
        let (adapters, originals) = engine_wrote_first(&store, "cloudflare", [State::NotSet, State::NotSet]);
        let resolver = FakeResolver::working(&store, &["1.1.1.1", "8.8.8.8"]);
        let mut said = Vec::new();

        let outcome = run(
            &store,
            &resolver,
            &spec(&["cloudflare", "google"], Choose::FirstWorking),
            &adapters,
            &originals,
            &mut |line| said.push(line.to_string()),
        );

        assert_eq!(outcome, Outcome::Encrypted { provider: "Cloudflare".into(), skipped: vec![] });
        assert!(store.log.borrow().is_empty(), "google must never be written");
        assert!(said.iter().any(|line| line == "Trying Cloudflare (encrypted DNS)..."), "{said:?}");
    }

    #[test]
    fn falls_to_next_provider_and_reports_the_skipped_one() {
        let store = FakeStore::default();
        let (adapters, originals) = engine_wrote_first(&store, "cloudflare", [State::NotSet, State::NotSet]);
        let resolver = FakeResolver::working(&store, &["8.8.8.8"]);

        let outcome = run_quietly(
            &store,
            &resolver,
            &spec(&["cloudflare", "google"], Choose::FirstWorking),
            &adapters,
            &originals,
        );

        assert_eq!(
            outcome,
            Outcome::Encrypted { provider: "Google".into(), skipped: names(&["Cloudflare"]) }
        );
        assert_eq!(store.read(dns_setting(), Some(A)), provider_state("google"));
        assert_eq!(store.read(dns_setting(), Some(B)), provider_state("google"));
    }

    #[test]
    fn all_fail_restores_originals_exactly() {
        let store = FakeStore::default();
        let (adapters, originals) =
            engine_wrote_first(&store, "cloudflare", [State::NotSet, static_dns("9.9.9.9")]);
        let resolver = FakeResolver::working(&store, &[]);

        let outcome = run_quietly(
            &store,
            &resolver,
            &spec(&["cloudflare", "google"], Choose::FirstWorking),
            &adapters,
            &originals,
        );

        assert_eq!(outcome, Outcome::FellBack { tried: names(&["Cloudflare", "Google"]) });
        // DHCP (not set) comes back as not set, not as an empty static list.
        assert_eq!(store.read(dns_setting(), Some(A)), State::NotSet);
        assert!(!store.snapshot().contains_key(&("dns.resolver".to_string(), Some(A.to_string()))));
        assert_eq!(store.read(dns_setting(), Some(B)), static_dns("9.9.9.9"));
    }

    #[test]
    fn fastest_keeps_the_lowest_median_and_rewrites_when_needed() {
        let store = FakeStore::default();
        let (adapters, originals) = engine_wrote_first(&store, "cloudflare", [State::NotSet, State::NotSet]);
        let resolver = FakeResolver {
            works_for: vec!["1.1.1.1", "8.8.8.8"],
            durations: vec![("1.1.1.1", 30), ("8.8.8.8", 10)],
            ..FakeResolver::new(&store)
        };

        let outcome = run_quietly(
            &store,
            &resolver,
            &spec(&["cloudflare", "google"], Choose::Fastest),
            &adapters,
            &originals,
        );

        assert_eq!(outcome, Outcome::Encrypted { provider: "Google".into(), skipped: vec![] });
        assert_eq!(store.read(dns_setting(), Some(A)), provider_state("google"));
        assert_eq!(store.read(dns_setting(), Some(B)), provider_state("google"));
    }

    #[test]
    fn fastest_rewrites_the_earlier_provider_when_it_is_the_fastest() {
        let store = FakeStore::default();
        let (adapters, originals) = engine_wrote_first(&store, "cloudflare", [State::NotSet, State::NotSet]);
        let resolver = FakeResolver {
            works_for: vec!["1.1.1.1", "8.8.8.8"],
            durations: vec![("1.1.1.1", 10), ("8.8.8.8", 30)],
            ..FakeResolver::new(&store)
        };

        let outcome = run_quietly(
            &store,
            &resolver,
            &spec(&["cloudflare", "google"], Choose::Fastest),
            &adapters,
            &originals,
        );

        assert_eq!(outcome, Outcome::Encrypted { provider: "Cloudflare".into(), skipped: vec![] });
        assert_eq!(store.read(dns_setting(), Some(A)), provider_state("cloudflare"));
        assert_eq!(store.read(dns_setting(), Some(B)), provider_state("cloudflare"));
    }

    #[test]
    fn fastest_ignores_providers_that_fail() {
        let store = FakeStore::default();
        let (adapters, originals) = engine_wrote_first(&store, "cloudflare", [State::NotSet, State::NotSet]);
        let resolver = FakeResolver::working(&store, &["8.8.8.8"]);

        let outcome = run_quietly(
            &store,
            &resolver,
            &spec(&["cloudflare", "google"], Choose::Fastest),
            &adapters,
            &originals,
        );

        assert_eq!(
            outcome,
            Outcome::Encrypted { provider: "Google".into(), skipped: names(&["Cloudflare"]) }
        );
        assert_eq!(store.read(dns_setting(), Some(A)), provider_state("google"));
    }

    #[test]
    fn write_failure_restores_originals_and_is_program_error() {
        let store = FakeStore {
            fail_id: Some("dns.resolver"),
            fail_adapter: Some(B),
            failures_left: Cell::new(1),
            ..Default::default()
        };
        let (adapters, originals) =
            engine_wrote_first(&store, "cloudflare", [State::NotSet, static_dns("9.9.9.9")]);
        let resolver = FakeResolver::working(&store, &[]);

        let outcome = run_quietly(
            &store,
            &resolver,
            &spec(&["cloudflare", "google"], Choose::FirstWorking),
            &adapters,
            &originals,
        );

        let Outcome::ProgramError(reason) = outcome else { panic!("expected a program error") };
        assert!(reason.contains("simulated failure"), "{reason}");
        // Adapter A had already been switched to Google when B failed.
        assert_eq!(store.read(dns_setting(), Some(A)), State::NotSet);
        assert_eq!(store.read(dns_setting(), Some(B)), static_dns("9.9.9.9"));
    }

    #[test]
    fn readback_mismatch_is_a_program_error() {
        fn forget_doh(state: &State) -> State {
            match state {
                State::Value(Value::Dns(servers)) => State::Value(Value::Dns(
                    servers.iter().map(|s| DnsServer { address: s.address.clone(), doh: None }).collect(),
                )),
                other => other.clone(),
            }
        }
        let store = FakeStore { mangle: Some(forget_doh), ..Default::default() };
        let (adapters, originals) = engine_wrote_first(&store, "cloudflare", [State::NotSet, State::NotSet]);
        let resolver = FakeResolver::working(&store, &[]);

        let outcome = run_quietly(
            &store,
            &resolver,
            &spec(&["cloudflare", "google"], Choose::FirstWorking),
            &adapters,
            &originals,
        );

        let Outcome::ProgramError(reason) = outcome else { panic!("expected a program error") };
        assert!(reason.contains("did not take effect"), "{reason}");
        assert_eq!(store.read(dns_setting(), Some(A)), State::NotSet);
    }

    #[test]
    fn restore_failure_is_reported_with_the_adapters_left_over() {
        let store = FakeStore {
            fail_id: Some("dns.resolver"),
            fail_adapter: Some(B),
            failures_left: Cell::new(1),
            ..Default::default()
        };
        let (adapters, originals) =
            engine_wrote_first(&store, "cloudflare", [State::NotSet, static_dns("9.9.9.9")]);
        let resolver = FakeResolver::working(&store, &[]);

        // One provider only: nothing is written until the restore, which fails for B.
        let outcome = run_quietly(
            &store,
            &resolver,
            &spec(&["cloudflare"], Choose::FirstWorking),
            &adapters,
            &originals,
        );

        let Outcome::ProgramError(reason) = outcome else { panic!("expected a program error") };
        assert!(reason.contains("could not be put back"), "{reason}");
        assert!(reason.contains(B), "{reason}");
        assert!(!reason.contains(A), "{reason}");
    }

    #[test]
    fn median_of_an_odd_and_even_count() {
        assert_eq!(median(&mut [ms(30), ms(10), ms(20)]), Some(ms(20)));
        assert_eq!(median(&mut [ms(10), ms(20), ms(30), ms(40)]), Some(ms(25)));
        assert_eq!(median(&mut []), None);
    }

    #[test]
    fn messages_match_the_design_table() {
        let encrypted = |provider: &str, skipped: &[&str]| Outcome::Encrypted {
            provider: provider.into(),
            skipped: names(skipped),
        };
        assert_eq!(message(&encrypted("Cloudflare", &[])), "Encrypted DNS is on: Cloudflare (DoH).");
        assert_eq!(
            message(&encrypted("Google", &["Cloudflare"])),
            "Cloudflare did not work on this network; using Google instead.\nEncrypted DNS is on: Google (DoH)."
        );
        assert_eq!(
            message(&Outcome::FellBack { tried: names(&["Cloudflare"]) }),
            "Encrypted DNS did not work on this network, so your DNS was put back as it was. Nothing is broken."
        );
        assert_eq!(
            precheck_message(&Precheck::NoInternet).as_deref(),
            Some("No internet connection right now, so DNS was not changed.")
        );
        assert_eq!(
            precheck_message(&Precheck::Unsupported).as_deref(),
            Some("This Windows version has no encrypted DNS, so DNS was not changed.")
        );
        assert_eq!(precheck_message(&Precheck::Go), None);
    }

    #[test]
    fn messages_only_ask_for_a_report_on_program_errors() {
        let error = message(&Outcome::ProgramError("boom".into()));
        assert!(error.starts_with("DNS problem: boom\n"), "{error}");
        assert!(error.contains("This looks like a problem in NetShutUp. Please report it privately:"));
        assert!(error.contains(REPORT_URL));

        let others = [
            message(&Outcome::Encrypted { provider: "Google".into(), skipped: names(&["Cloudflare"]) }),
            message(&Outcome::FellBack { tried: names(&["Cloudflare", "Google"]) }),
            precheck_message(&Precheck::NoInternet).unwrap(),
            precheck_message(&Precheck::Unsupported).unwrap(),
        ];
        for text in others {
            assert!(!text.contains(REPORT_URL), "{text}");
        }
    }
}
