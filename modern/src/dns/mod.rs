//! The encrypted DNS trial: after a profile has pointed the adapters at its
//! first provider, prove that it works, move on to the next provider if not,
//! and put the original DNS back if nothing works. The logic only talks to a
//! `Store` and a `Resolver`, so it is tested with fakes.

pub mod dot;
pub mod resolver;

use std::path::Path;
use std::time::Duration;

use crate::settings::catalog::{self, Setting};
use crate::settings::profile::{Choose, DnsSpec};
use crate::settings::provider::{self, Provider};
use crate::settings::reader::{self, DnsServer, State, Value};
use crate::settings::store::Store;

/// Windows 11. Earlier Windows 10 builds have no usable encrypted DNS.
pub const FIRST_BUILD_WITH_DOH: u32 = 22000;

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
    } else if internet_works(resolver) {
        Precheck::Go
    } else {
        Precheck::NoInternet
    }
}

/// Whether DNS resolves at all right now: one of the test names is enough.
pub fn internet_works(resolver: &dyn Resolver) -> bool {
    TEST_NAMES.iter().any(|name| resolver.lookup(name).is_ok())
}

/// How encrypted DNS was reached for a provider.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Method {
    Doh,
    /// Experimental: through Windows' own encrypted DNS server list.
    Dot,
}

#[derive(Debug, PartialEq, Eq)]
pub enum Outcome {
    /// Encrypted DNS works. `skipped` are the attempts that did not, as labels
    /// like "Cloudflare (DoH)" (plain names when DoT is not in play).
    Encrypted { provider: String, method: Method, skipped: Vec<String> },
    /// None worked; DNS is back as it was. `internet_ok` is whether DNS
    /// resolves again after putting it back. `tried` holds the same labels.
    FellBack { tried: Vec<String>, internet_ok: bool },
    /// NetShutUp itself failed; DNS was put back where possible.
    ProgramError(String),
}

/// The experimental DoT path: its backend, the apply backup (the sidecar goes
/// next to it) and what the global list held before anything was touched.
struct Dot<'a> {
    backend: &'a dyn dot::DotBackend,
    backup: &'a Path,
    saved: Option<dot::Saved>,
}

impl Dot<'_> {
    /// Saves every server address of every provider, once, before the first write.
    fn ensure_saved(&mut self, providers: &[&Provider]) -> Result<(), String> {
        if self.saved.is_some() {
            return Ok(());
        }
        let addresses: Vec<String> =
            providers.iter().flat_map(|p| p.servers.iter().map(|a| a.to_string())).collect();
        let saved = dot::snapshot(self.backend, &addresses)?;
        dot::save(&dot::sidecar_path(self.backup), &saved)?;
        self.saved = Some(saved);
        Ok(())
    }

    /// Puts the entries of the given addresses back as saved; the error messages.
    fn restore(&self, addresses: &[&str]) -> Vec<String> {
        let Some(saved) = &self.saved else { return Vec::new() };
        let part = dot::Saved {
            entries: saved
                .entries
                .iter()
                .filter(|entry| addresses.contains(&entry.address.as_str()))
                .cloned()
                .collect(),
        };
        dot::restore_all(self.backend, &part)
    }

    fn restore_everything(&self) -> Vec<String> {
        match &self.saved {
            Some(saved) => dot::restore_all(self.backend, saved),
            None => Vec::new(),
        }
    }
}

fn label(provider: &Provider, method: Method, with_method: bool) -> String {
    match (with_method, method) {
        (false, _) => provider.name.to_string(),
        (true, Method::Doh) => format!("{} (DoH)", provider.name),
        (true, Method::Dot) => format!("{} (DoT)", provider.name),
    }
}

/// The provider's servers without any DoH information: Windows upgrades to DoT itself.
fn dot_state(provider: &Provider) -> State {
    State::Value(Value::Dns(
        provider
            .servers
            .iter()
            .map(|address| DnsServer { address: address.to_string(), doh: None })
            .collect(),
    ))
}

fn state_for(provider: &Provider, method: Method) -> State {
    match method {
        Method::Doh => provider.state(),
        Method::Dot => dot_state(provider),
    }
}

/// Runs the trial. The apply engine has already written the first provider of
/// `spec` to `adapters`; `originals` holds what each adapter had before. With
/// `dot` (backend and the apply backup path), a provider whose DoH failed is
/// also tried over DoT (experimental).
pub fn run(
    store: &dyn Store,
    resolver: &dyn Resolver,
    spec: &DnsSpec,
    adapters: &[String],
    originals: &[(String, State)],
    dot: Option<(&dyn dot::DotBackend, &Path)>,
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
    let with_method = dot.is_some();
    let mut dot = dot.map(|(backend, backup)| Dot { backend, backup, saved: None });

    let mut skipped: Vec<String> = Vec::new();
    let mut working: Vec<(usize, Method, Duration)> = Vec::new();
    let mut last_written = (0, Method::Doh);

    for (index, provider) in providers.iter().enumerate() {
        if index > 0 {
            if let Err(reason) = write_all(store, setting, adapters, &provider.state()) {
                return fail(store, resolver, setting, originals, dot.as_ref(), reason);
            }
            last_written = (index, Method::Doh);
        }
        resolver.flush_cache();
        say(&format!("Trying {} (encrypted DNS)...", provider.name));

        if TEST_NAMES.iter().any(|name| resolver.lookup(name).is_ok()) {
            match spec.choose {
                Choose::FirstWorking => {
                    return Outcome::Encrypted {
                        provider: provider.name.to_string(),
                        method: Method::Doh,
                        skipped,
                    };
                }
                Choose::Fastest => {
                    let time = measure(resolver).unwrap_or(Duration::MAX);
                    say(&format!("  {}: {}", provider.name, describe(time)));
                    working.push((index, Method::Doh, time));
                }
            }
            continue;
        }
        skipped.push(label(provider, Method::Doh, with_method));

        let Some(dot) = dot.as_mut() else { continue };
        let started = dot
            .ensure_saved(&providers)
            .and_then(|()| dot::add_provider(dot.backend, provider))
            .and_then(|()| write_all(store, setting, adapters, &dot_state(provider)));
        if let Err(reason) = started {
            return fail(store, resolver, setting, originals, Some(&*dot), reason);
        }
        last_written = (index, Method::Dot);
        resolver.flush_cache();
        say(&format!("Trying {} (DoT, experimental)...", provider.name));

        if TEST_NAMES.iter().any(|name| resolver.lookup(name).is_ok()) {
            match spec.choose {
                Choose::FirstWorking => {
                    return Outcome::Encrypted {
                        provider: provider.name.to_string(),
                        method: Method::Dot,
                        skipped,
                    };
                }
                Choose::Fastest => {
                    let time = measure(resolver).unwrap_or(Duration::MAX);
                    say(&format!("  {} (DoT): {}", provider.name, describe(time)));
                    working.push((index, Method::Dot, time));
                }
            }
            continue;
        }
        skipped.push(label(provider, Method::Dot, true));
        let errors = dot.restore(&provider.servers);
        if !errors.is_empty() {
            let reason = format!("DoT entries could not be put back: {}", errors.join("; "));
            return fail(store, resolver, setting, originals, Some(&*dot), reason);
        }
    }

    if let Some(&(best, method, _)) = working.iter().min_by_key(|(_, _, time)| *time) {
        if (best, method) != last_written {
            if let Err(reason) = write_all(store, setting, adapters, &state_for(providers[best], method)) {
                return fail(store, resolver, setting, originals, dot.as_ref(), reason);
            }
            resolver.flush_cache();
        }
        // DoT entries of working candidates that were not chosen go back.
        if let Some(dot) = dot.as_ref() {
            let mut errors = Vec::new();
            for &(index, other, _) in &working {
                if other == Method::Dot && (index, other) != (best, method) {
                    errors.extend(dot.restore(&providers[index].servers));
                }
            }
            if !errors.is_empty() {
                let reason = format!("DoT entries could not be put back: {}", errors.join("; "));
                return fail(store, resolver, setting, originals, Some(dot), reason);
            }
        }
        return Outcome::Encrypted {
            provider: providers[best].name.to_string(),
            method,
            skipped,
        };
    }

    match restore_everything(store, resolver, setting, originals, dot.as_ref()) {
        Ok(()) => {
            let internet_ok = internet_works(resolver);
            Outcome::FellBack { tried: skipped, internet_ok }
        }
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

/// A program error: puts the originals (and any DoT entries) back, and says so
/// if that fails too.
fn fail(
    store: &dyn Store,
    resolver: &dyn Resolver,
    setting: &Setting,
    originals: &[(String, State)],
    dot: Option<&Dot>,
    reason: String,
) -> Outcome {
    match restore_everything(store, resolver, setting, originals, dot) {
        Ok(()) => Outcome::ProgramError(reason),
        Err(left_over) => Outcome::ProgramError(format!("{reason}; {left_over}")),
    }
}

/// Adapter DNS first, then every DoT entry; all problems in one message.
fn restore_everything(
    store: &dyn Store,
    resolver: &dyn Resolver,
    setting: &Setting,
    originals: &[(String, State)],
    dot: Option<&Dot>,
) -> Result<(), String> {
    let mut problems = Vec::new();
    if let Err(reason) = restore(store, resolver, setting, originals) {
        problems.push(reason);
    }
    if let Some(dot) = dot {
        let errors = dot.restore_everything();
        if !errors.is_empty() {
            problems.push(format!("DoT entries could not be put back: {}", errors.join("; ")));
        }
    }
    if problems.is_empty() { Ok(()) } else { Err(problems.join("; ")) }
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

/// Shown after a successful trial. The trial asks the Windows DNS client, which
/// may also use IPv6 DNS servers from the router; those are not changed.
pub fn ipv6_note(outcome: &Outcome) -> Option<&'static str> {
    match outcome {
        Outcome::Encrypted { .. } => Some(
            "Note: DNS servers that your router hands out over IPv6 are not changed and may still be used without encryption.",
        ),
        _ => None,
    }
}

/// What to tell the user about the trial. Only a program error asks for a
/// report; a network that blocks encrypted DNS is not a bug.
pub fn message(outcome: &Outcome) -> String {
    match outcome {
        Outcome::Encrypted { provider, method, skipped } => {
            let on = match method {
                Method::Doh => format!("Encrypted DNS is on: {provider} (DoH)."),
                Method::Dot => format!("Encrypted DNS is on: {provider} (DoT, experimental)."),
            };
            if skipped.is_empty() {
                return on;
            }
            let instead = match method {
                Method::Doh => provider.clone(),
                Method::Dot => format!("{provider} (DoT)"),
            };
            format!("{} did not work on this network; using {instead} instead.\n{on}", skipped.join(", "))
        }
        Outcome::FellBack { internet_ok: true, .. } => "Encrypted DNS did not work on this network, so your DNS was put back as it was. Nothing is broken.".to_string(),
        Outcome::FellBack { internet_ok: false, .. } => format!(
            "Encrypted DNS did not work on this network, and your DNS was put back as it was, but the internet does not seem to work right now.\nIf it stays that way, please check your connection. If you think it is a problem in NetShutUp, report it privately: {REPORT_URL}"
        ),
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
    use std::cell::{Cell, RefCell};
    use std::collections::HashMap;

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
        /// A DoT fake: adapters without DoH resolve only when it has a working entry.
        dot: Option<&'a FakeDot>,
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
                dot: None,
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
            let (first, plain) = match self.store.read(dns_setting(), Some(A)) {
                State::Value(Value::Dns(servers)) => (servers[0].address.clone(), servers[0].doh.is_none()),
                // DHCP: the key "" in `works_for` means plain DHCP DNS works.
                State::NotSet => (String::new(), false),
                _ => return Err("no servers".to_string()),
            };
            let works = match self.dot {
                Some(dot) if plain => {
                    dot.works.contains(&first.as_str()) && dot.entries.borrow().contains_key(&first)
                }
                _ => self.works_for.contains(&first.as_str()),
            };
            if !works {
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
        run(store, resolver, spec, adapters, originals, None, &mut |_| {})
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
            None,
            &mut |line| said.push(line.to_string()),
        );

        assert_eq!(outcome, Outcome::Encrypted { method: Method::Doh, provider: "Cloudflare".into(), skipped: vec![] });
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
            Outcome::Encrypted { method: Method::Doh, provider: "Google".into(), skipped: names(&["Cloudflare"]) }
        );
        assert_eq!(store.read(dns_setting(), Some(A)), provider_state("google"));
        assert_eq!(store.read(dns_setting(), Some(B)), provider_state("google"));
    }

    #[test]
    fn all_fail_restores_originals_exactly() {
        let store = FakeStore::default();
        let (adapters, originals) =
            engine_wrote_first(&store, "cloudflare", [State::NotSet, static_dns("9.9.9.9")]);
        // Neither provider resolves, but plain DHCP DNS ("") does.
        let resolver = FakeResolver::working(&store, &[""]);

        let outcome = run_quietly(
            &store,
            &resolver,
            &spec(&["cloudflare", "google"], Choose::FirstWorking),
            &adapters,
            &originals,
        );

        assert_eq!(
            outcome,
            Outcome::FellBack { tried: names(&["Cloudflare", "Google"]), internet_ok: true }
        );
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

        assert_eq!(outcome, Outcome::Encrypted { method: Method::Doh, provider: "Google".into(), skipped: vec![] });
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

        assert_eq!(outcome, Outcome::Encrypted { method: Method::Doh, provider: "Cloudflare".into(), skipped: vec![] });
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
            Outcome::Encrypted { method: Method::Doh, provider: "Google".into(), skipped: names(&["Cloudflare"]) }
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
    fn fell_back_reports_when_the_internet_still_does_not_work() {
        let store = FakeStore::default();
        let (adapters, originals) = engine_wrote_first(&store, "cloudflare", [State::NotSet, State::NotSet]);
        // Nothing resolves, not even DHCP DNS, after the originals are back.
        let resolver = FakeResolver::working(&store, &[]);

        let outcome = run_quietly(
            &store,
            &resolver,
            &spec(&["cloudflare"], Choose::FirstWorking),
            &adapters,
            &originals,
        );

        assert_eq!(outcome, Outcome::FellBack { tried: names(&["Cloudflare"]), internet_ok: false });
        let text = message(&outcome);
        assert!(text.contains("the internet does not seem to work right now"), "{text}");
        assert!(text.contains(REPORT_URL), "{text}");
    }

    #[test]
    fn the_ipv6_note_goes_with_encrypted_outcomes_only() {
        let on = Outcome::Encrypted { method: Method::Doh, provider: "Cloudflare".into(), skipped: vec![] };
        let note = ipv6_note(&on).expect("a note after success");
        assert!(note.contains("IPv6"), "{note}");
        assert_eq!(ipv6_note(&Outcome::FellBack { tried: vec![], internet_ok: true }), None);
        assert_eq!(ipv6_note(&Outcome::ProgramError("x".into())), None);
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
            method: Method::Doh,
            provider: provider.into(),
            skipped: names(skipped),
        };
        assert_eq!(message(&encrypted("Cloudflare", &[])), "Encrypted DNS is on: Cloudflare (DoH).");
        assert_eq!(
            message(&encrypted("Google", &["Cloudflare"])),
            "Cloudflare did not work on this network; using Google instead.\nEncrypted DNS is on: Google (DoH)."
        );
        assert_eq!(
            message(&Outcome::FellBack { tried: names(&["Cloudflare"]), internet_ok: true }),
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
            message(&Outcome::Encrypted { method: Method::Doh, provider: "Google".into(), skipped: names(&["Cloudflare"]) }),
            message(&Outcome::FellBack { tried: names(&["Cloudflare", "Google"]), internet_ok: true }),
            precheck_message(&Precheck::NoInternet).unwrap(),
            precheck_message(&Precheck::Unsupported).unwrap(),
        ];
        for text in others {
            assert!(!text.contains(REPORT_URL), "{text}");
        }
    }

    // ---- DoT (experimental) ----

    #[derive(Default)]
    struct FakeDot {
        entries: RefCell<HashMap<String, dot::EntryValues>>,
        /// Servers whose DoT entry makes resolving work.
        works: Vec<&'static str>,
        log: RefCell<Vec<String>>,
        fail_add: bool,
        restore_does_nothing: bool,
        /// Looked at (and removed) on every add, to see when the sidecar appears.
        sidecar: Option<std::path::PathBuf>,
        first_add_sidecar: RefCell<Option<Result<dot::Saved, String>>>,
    }

    impl dot::DotBackend for FakeDot {
        fn read(&self, address: &str) -> Result<Option<dot::EntryValues>, String> {
            Ok(self.entries.borrow().get(address).cloned())
        }
        fn add(&self, address: &str, host: &str) -> Result<(), String> {
            self.log.borrow_mut().push(format!("add {address}"));
            if let Some(path) = &self.sidecar {
                if self.first_add_sidecar.borrow().is_none() {
                    *self.first_add_sidecar.borrow_mut() = Some(dot::load(path));
                }
                let _ = std::fs::remove_file(path);
            }
            if self.fail_add {
                return Err("netsh failed (simulated)".to_string());
            }
            self.entries
                .borrow_mut()
                .insert(address.to_string(), vec![("DotHost".to_string(), format!("REG_SZ:{host}:853"))]);
            Ok(())
        }
        fn restore(&self, address: &str, saved: &Option<dot::EntryValues>) -> Result<(), String> {
            self.log.borrow_mut().push(format!("restore {address}"));
            if self.restore_does_nothing {
                return Ok(());
            }
            match saved {
                Some(values) => self.entries.borrow_mut().insert(address.to_string(), values.clone()),
                None => self.entries.borrow_mut().remove(address),
            };
            Ok(())
        }
    }

    fn old_entry() -> dot::EntryValues {
        vec![("Template".to_string(), "REG_SZ:https://old".to_string())]
    }

    fn dot_state(id: &str) -> State {
        let servers = provider::find(id).unwrap().servers;
        State::Value(Value::Dns(
            servers.iter().map(|a| DnsServer { address: a.to_string(), doh: None }).collect(),
        ))
    }

    fn backup_path(name: &str) -> std::path::PathBuf {
        let dir = crate::settings::backup::testing::temp_dir(name);
        std::fs::create_dir_all(&dir).unwrap();
        dir.join("b.json")
    }

    fn resolver_with_dot<'a>(
        store: &'a FakeStore,
        dot: &'a FakeDot,
        works_for: &[&'static str],
    ) -> FakeResolver<'a> {
        FakeResolver { dot: Some(dot), ..FakeResolver::working(store, works_for) }
    }

    fn run_dot(
        store: &FakeStore,
        resolver: &dyn Resolver,
        spec: &DnsSpec,
        adapters: &[String],
        originals: &[(String, State)],
        backend: &FakeDot,
        backup: &std::path::Path,
    ) -> Outcome {
        run(store, resolver, spec, adapters, originals, Some((backend, backup)), &mut |_| {})
    }

    #[test]
    fn dot_is_tried_after_doh_fails_for_the_same_provider() {
        let store = FakeStore::default();
        let (adapters, originals) = engine_wrote_first(&store, "cloudflare", [State::NotSet, State::NotSet]);
        let backend = FakeDot { works: vec!["1.1.1.1"], ..Default::default() };
        let resolver = resolver_with_dot(&store, &backend, &[]);

        let outcome = run_dot(
            &store,
            &resolver,
            &spec(&["cloudflare", "google"], Choose::FirstWorking),
            &adapters,
            &originals,
            &backend,
            &backup_path("dot-same-provider"),
        );

        assert_eq!(
            outcome,
            Outcome::Encrypted {
                provider: "Cloudflare".into(),
                method: Method::Dot,
                skipped: names(&["Cloudflare (DoH)"])
            }
        );
        assert_eq!(store.read(dns_setting(), Some(A)), dot_state("cloudflare"));
        assert_eq!(store.read(dns_setting(), Some(B)), dot_state("cloudflare"));
        assert_eq!(*backend.log.borrow(), ["add 1.1.1.1", "add 1.0.0.1"]);
    }

    #[test]
    fn first_working_stops_at_dot_success_and_names_the_method() {
        let store = FakeStore::default();
        let (adapters, originals) = engine_wrote_first(&store, "cloudflare", [State::NotSet, State::NotSet]);
        let backend = FakeDot { works: vec!["8.8.8.8"], ..Default::default() };
        let resolver = resolver_with_dot(&store, &backend, &[]);

        let outcome = run_dot(
            &store,
            &resolver,
            &spec(&["cloudflare", "google", "quad9"], Choose::FirstWorking),
            &adapters,
            &originals,
            &backend,
            &backup_path("dot-stops"),
        );

        assert_eq!(
            outcome,
            Outcome::Encrypted {
                provider: "Google".into(),
                method: Method::Dot,
                skipped: names(&["Cloudflare (DoH)", "Cloudflare (DoT)", "Google (DoH)"])
            }
        );
        let log = backend.log.borrow();
        assert!(!log.iter().any(|line| line.contains("9.9.9.9")), "{log:?}");
        assert!(backend.entries.borrow().contains_key("8.8.8.8"));
        assert!(!backend.entries.borrow().contains_key("1.1.1.1"));
    }

    #[test]
    fn a_failing_dot_entry_is_restored_before_the_next_provider() {
        let store = FakeStore::default();
        let (adapters, originals) = engine_wrote_first(&store, "cloudflare", [State::NotSet, State::NotSet]);
        let backend = FakeDot::default();
        backend.entries.borrow_mut().insert("1.1.1.1".to_string(), old_entry());
        let resolver = resolver_with_dot(&store, &backend, &["8.8.8.8"]);

        let outcome = run_dot(
            &store,
            &resolver,
            &spec(&["cloudflare", "google"], Choose::FirstWorking),
            &adapters,
            &originals,
            &backend,
            &backup_path("dot-restored-before-next"),
        );

        assert_eq!(
            outcome,
            Outcome::Encrypted {
                provider: "Google".into(),
                method: Method::Doh,
                skipped: names(&["Cloudflare (DoH)", "Cloudflare (DoT)"])
            }
        );
        assert_eq!(
            *backend.log.borrow(),
            ["add 1.1.1.1", "add 1.0.0.1", "restore 1.1.1.1", "restore 1.0.0.1"]
        );
        assert_eq!(backend.entries.borrow().get("1.1.1.1"), Some(&old_entry()));
        assert!(!backend.entries.borrow().contains_key("1.0.0.1"));
    }

    #[test]
    fn fallback_restores_adapters_and_dot_entries() {
        let store = FakeStore::default();
        let (adapters, originals) =
            engine_wrote_first(&store, "cloudflare", [State::NotSet, static_dns("9.9.9.9")]);
        let backend = FakeDot::default();
        backend.entries.borrow_mut().insert("1.1.1.1".to_string(), old_entry());
        // Only plain DHCP DNS ("") resolves.
        let resolver = resolver_with_dot(&store, &backend, &[""]);

        let outcome = run_dot(
            &store,
            &resolver,
            &spec(&["cloudflare"], Choose::FirstWorking),
            &adapters,
            &originals,
            &backend,
            &backup_path("dot-fallback"),
        );

        assert_eq!(
            outcome,
            Outcome::FellBack { tried: names(&["Cloudflare (DoH)", "Cloudflare (DoT)"]), internet_ok: true }
        );
        assert_eq!(store.read(dns_setting(), Some(A)), State::NotSet);
        assert_eq!(store.read(dns_setting(), Some(B)), static_dns("9.9.9.9"));
        assert_eq!(backend.entries.borrow().get("1.1.1.1"), Some(&old_entry()));
        assert!(!backend.entries.borrow().contains_key("1.0.0.1"));
    }

    #[test]
    fn dot_write_failure_is_a_program_error_and_everything_is_restored() {
        let store = FakeStore::default();
        let (adapters, originals) =
            engine_wrote_first(&store, "cloudflare", [State::NotSet, static_dns("9.9.9.9")]);
        let backend = FakeDot { fail_add: true, ..Default::default() };
        backend.entries.borrow_mut().insert("1.1.1.1".to_string(), old_entry());
        let resolver = resolver_with_dot(&store, &backend, &[""]);

        let outcome = run_dot(
            &store,
            &resolver,
            &spec(&["cloudflare"], Choose::FirstWorking),
            &adapters,
            &originals,
            &backend,
            &backup_path("dot-write-failure"),
        );

        let Outcome::ProgramError(reason) = outcome else { panic!("expected a program error") };
        assert!(reason.contains("simulated"), "{reason}");
        assert_eq!(store.read(dns_setting(), Some(A)), State::NotSet);
        assert_eq!(store.read(dns_setting(), Some(B)), static_dns("9.9.9.9"));
        assert_eq!(backend.entries.borrow().get("1.1.1.1"), Some(&old_entry()));
    }

    #[test]
    fn restore_mismatch_is_reported_in_the_program_error() {
        let store = FakeStore::default();
        let (adapters, originals) = engine_wrote_first(&store, "cloudflare", [State::NotSet, State::NotSet]);
        let backend = FakeDot { restore_does_nothing: true, ..Default::default() };
        let resolver = resolver_with_dot(&store, &backend, &[""]);

        let outcome = run_dot(
            &store,
            &resolver,
            &spec(&["cloudflare"], Choose::FirstWorking),
            &adapters,
            &originals,
            &backend,
            &backup_path("dot-restore-mismatch"),
        );

        let Outcome::ProgramError(reason) = outcome else { panic!("expected a program error") };
        assert!(reason.contains("entry differs after restoring"), "{reason}");
        assert_eq!(store.read(dns_setting(), Some(A)), State::NotSet);
    }

    #[test]
    fn sidecar_is_written_once_before_the_first_dot_write() {
        let store = FakeStore::default();
        let (adapters, originals) = engine_wrote_first(&store, "cloudflare", [State::NotSet, State::NotSet]);
        let path = backup_path("dot-sidecar-once");
        let sidecar = dot::sidecar_path(&path);
        let backend = FakeDot { sidecar: Some(sidecar.clone()), ..Default::default() };
        backend.entries.borrow_mut().insert("8.8.8.8".to_string(), old_entry());
        let resolver = resolver_with_dot(&store, &backend, &[""]);

        run_dot(
            &store,
            &resolver,
            &spec(&["cloudflare", "google"], Choose::FirstWorking),
            &adapters,
            &originals,
            &backend,
            &path,
        );

        let saved = backend.first_add_sidecar.borrow().clone().expect("an add happened").expect("sidecar existed");
        let address = |entry: &dot::SavedEntry| entry.address.clone();
        assert_eq!(
            saved.entries.iter().map(address).collect::<Vec<_>>(),
            names(&["1.1.1.1", "1.0.0.1", "8.8.8.8", "8.8.4.4"])
        );
        assert_eq!(saved.entries[2].values, Some(old_entry()));
        // The fake deletes the file on every add; it must not have been written again.
        assert!(!sidecar.exists());
    }

    #[test]
    fn fastest_tries_dot_only_for_providers_whose_doh_failed() {
        let store = FakeStore::default();
        let (adapters, originals) = engine_wrote_first(&store, "cloudflare", [State::NotSet, State::NotSet]);
        let backend = FakeDot { works: vec!["8.8.8.8"], ..Default::default() };
        let resolver = FakeResolver {
            durations: vec![("1.1.1.1", 30), ("8.8.8.8", 10)],
            ..resolver_with_dot(&store, &backend, &["1.1.1.1"])
        };

        let outcome = run_dot(
            &store,
            &resolver,
            &spec(&["cloudflare", "google"], Choose::Fastest),
            &adapters,
            &originals,
            &backend,
            &backup_path("dot-fastest"),
        );

        assert_eq!(
            outcome,
            Outcome::Encrypted {
                provider: "Google".into(),
                method: Method::Dot,
                skipped: names(&["Google (DoH)"])
            }
        );
        assert_eq!(*backend.log.borrow(), ["add 8.8.8.8", "add 8.8.4.4"]);
        assert_eq!(store.read(dns_setting(), Some(A)), dot_state("google"));
    }

    #[test]
    fn fastest_gives_up_a_dot_entry_when_a_doh_provider_is_faster() {
        let store = FakeStore::default();
        let (adapters, originals) = engine_wrote_first(&store, "google", [State::NotSet, State::NotSet]);
        let backend = FakeDot { works: vec!["8.8.8.8"], ..Default::default() };
        let resolver = FakeResolver {
            durations: vec![("1.1.1.1", 10), ("8.8.8.8", 30)],
            ..resolver_with_dot(&store, &backend, &["1.1.1.1"])
        };

        let outcome = run_dot(
            &store,
            &resolver,
            &spec(&["google", "cloudflare"], Choose::Fastest),
            &adapters,
            &originals,
            &backend,
            &backup_path("dot-fastest-doh-wins"),
        );

        assert_eq!(
            outcome,
            Outcome::Encrypted {
                provider: "Cloudflare".into(),
                method: Method::Doh,
                skipped: names(&["Google (DoH)"])
            }
        );
        assert!(!backend.entries.borrow().contains_key("8.8.8.8"));
        assert_eq!(store.read(dns_setting(), Some(A)), provider_state("cloudflare"));
    }

    #[test]
    fn without_a_dot_backend_nothing_changes() {
        let store = FakeStore::default();
        let (adapters, originals) = engine_wrote_first(&store, "cloudflare", [State::NotSet, State::NotSet]);
        let resolver = FakeResolver::working(&store, &[""]);

        let outcome = run_quietly(
            &store,
            &resolver,
            &spec(&["cloudflare"], Choose::FirstWorking),
            &adapters,
            &originals,
        );

        assert_eq!(outcome, Outcome::FellBack { tried: names(&["Cloudflare"]), internet_ok: true });
        assert_eq!(store.read(dns_setting(), Some(A)), State::NotSet);
    }

    #[test]
    fn dot_messages_name_the_method_and_the_skips() {
        let dot_on = |skipped: &[&str]| Outcome::Encrypted {
            provider: "Cloudflare".into(),
            method: Method::Dot,
            skipped: names(skipped),
        };
        assert_eq!(message(&dot_on(&[])), "Encrypted DNS is on: Cloudflare (DoT, experimental).");
        assert_eq!(
            message(&dot_on(&["Cloudflare (DoH)"])),
            "Cloudflare (DoH) did not work on this network; using Cloudflare (DoT) instead.\nEncrypted DNS is on: Cloudflare (DoT, experimental)."
        );
        assert_eq!(
            message(&dot_on(&["Cloudflare (DoH)", "Google (DoH)"])),
            "Cloudflare (DoH), Google (DoH) did not work on this network; using Cloudflare (DoT) instead.\nEncrypted DNS is on: Cloudflare (DoT, experimental)."
        );
    }
}
