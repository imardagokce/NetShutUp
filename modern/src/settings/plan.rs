//! Building the plan of a profile, and the DNS-specific rules around it. Used
//! by the commands and by the self-test, so both plan exactly the same way.

use super::apply::{self, PlannedChange};
use super::profile::Profile;
use super::reader::State;
use super::store::Store;
use crate::dns::Precheck;

pub const DNS_SETTING: &str = "dns.resolver";

pub fn is_dns(change: &PlannedChange) -> bool {
    change.target.setting.id == DNS_SETTING
}

/// What applying `profile` to `adapters` (GUIDs) would change. The DNS stage is
/// included only when `dns_available`. Returns the adapters whose DNS could not
/// be read and was left out.
pub fn plan_profile(
    store: &dyn Store,
    profile: &Profile,
    adapters: &[String],
    dns_available: bool,
) -> (Vec<PlannedChange>, Vec<String>) {
    let mut targets = profile.targets(adapters);
    if dns_available {
        targets.extend(profile.dns_targets(adapters));
    }
    drop_unreadable_dns(apply::plan(store, targets))
}

/// An adapter whose DNS cannot be read cannot be backed up. Leave only its DNS
/// out, so the rest of the profile still applies. Returns the adapters dropped.
pub fn drop_unreadable_dns(plan: Vec<PlannedChange>) -> (Vec<PlannedChange>, Vec<String>) {
    let mut dropped = Vec::new();
    let kept = plan
        .into_iter()
        .filter(|change| {
            let unreadable = is_dns(change) && matches!(change.current, State::Unavailable(_));
            if unreadable {
                dropped.extend(change.target.adapter.clone());
            }
            !unreadable
        })
        .collect();
    (kept, dropped)
}

/// The adapters the DNS trial may switch, with what each had before. Only the
/// ones the apply engine backs up: anything else could not be put back by `revert`.
pub fn dns_trial_scope(plan: &[PlannedChange]) -> (Vec<String>, Vec<(String, State)>) {
    let scope = plan.iter().filter(|change| is_dns(change) && change.is_needed());
    let mut adapters = Vec::new();
    let mut originals = Vec::new();
    for change in scope {
        if let Some(adapter) = change.target.adapter.clone() {
            adapters.push(adapter.clone());
            originals.push((adapter, change.current.clone()));
        }
    }
    (adapters, originals)
}

/// The plan without its DNS changes unless the precheck allows them.
pub fn plan_for_precheck(plan: &[PlannedChange], precheck: &Precheck) -> Vec<PlannedChange> {
    plan.iter()
        .filter(|change| *precheck == Precheck::Go || !is_dns(change))
        .cloned()
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::settings::apply::Target;
    use crate::settings::catalog;
    use crate::settings::profile;
    use crate::settings::provider;
    use crate::settings::reader::Value;
    use crate::settings::store::testing::FakeStore;

    fn planned(id: &str, adapter: Option<&str>, desired: State) -> PlannedChange {
        PlannedChange {
            target: Target {
                setting: catalog::find(id).unwrap(),
                adapter: adapter.map(str::to_string),
                desired,
            },
            current: State::NotSet,
        }
    }

    #[test]
    fn dns_targets_are_dropped_unless_precheck_says_go() {
        let google = provider::find("google").unwrap().state();
        let plan = vec![
            planned("dns.llmnr", None, State::Value(Value::Dword(0))),
            planned("dns.resolver", Some("{A}"), google),
        ];
        let ids = |plan: &[PlannedChange]| -> Vec<&'static str> {
            plan.iter().map(|change| change.target.setting.id).collect()
        };

        assert_eq!(ids(&plan_for_precheck(&plan, &Precheck::Go)), ["dns.llmnr", "dns.resolver"]);
        assert_eq!(ids(&plan_for_precheck(&plan, &Precheck::NoInternet)), ["dns.llmnr"]);
        assert_eq!(ids(&plan_for_precheck(&plan, &Precheck::Unsupported)), ["dns.llmnr"]);
    }

    #[test]
    fn unreadable_dns_is_dropped_so_the_rest_of_the_profile_still_applies() {
        let google = provider::find("google").unwrap().state();
        let readable = planned("dns.resolver", Some("{A}"), google.clone());
        let mut unreadable = planned("dns.resolver", Some("{B}"), google);
        unreadable.current = State::Unavailable("DNS settings error 87".to_string());
        let other = planned("dns.llmnr", None, State::Value(Value::Dword(0)));

        let (plan, dropped) = drop_unreadable_dns(vec![readable, unreadable, other]);

        let kept: Vec<(&str, Option<&str>)> = plan
            .iter()
            .map(|change| (change.target.setting.id, change.target.adapter.as_deref()))
            .collect();
        assert_eq!(kept, [("dns.resolver", Some("{A}")), ("dns.llmnr", None)]);
        assert_eq!(dropped, ["{B}"]);
    }

    #[test]
    fn dns_trial_scope_covers_only_backed_up_adapters() {
        let google = provider::find("google").unwrap().state();
        // {A} already has Google, so the engine will not back it up or write it.
        let mut already = planned("dns.resolver", Some("{A}"), google.clone());
        already.current = google.clone();
        let needed = planned("dns.resolver", Some("{B}"), google);

        let (adapters, originals) = dns_trial_scope(&[already, needed]);

        assert_eq!(adapters, ["{B}"]);
        assert_eq!(originals, [("{B}".to_string(), State::NotSet)]);
    }

    fn privacy() -> profile::Profile {
        profile::builtin().unwrap().into_iter().find(|p| p.id == "privacy").unwrap()
    }

    fn dns_rows(plan: &[PlannedChange]) -> usize {
        plan.iter().filter(|change| is_dns(change)).count()
    }

    #[test]
    fn plan_profile_uses_the_given_store_and_adds_dns_targets_only_when_available() {
        let store = FakeStore::default();
        let guids = ["{A}".to_string(), "{B}".to_string()];

        let (with_dns, dropped) = plan_profile(&store, &privacy(), &guids, true);
        assert_eq!(dns_rows(&with_dns), 2);
        assert!(dropped.is_empty());

        let (without_dns, _) = plan_profile(&store, &privacy(), &guids, false);
        assert_eq!(dns_rows(&without_dns), 0);
        assert!(without_dns.len() < with_dns.len());
    }

    #[test]
    fn plan_profile_drops_the_dns_of_adapters_that_cannot_be_read() {
        let store = FakeStore { unreadable_id: Some("dns.resolver"), ..Default::default() };
        let guids = ["{A}".to_string(), "{B}".to_string()];

        let (plan, dropped) = plan_profile(&store, &privacy(), &guids, true);

        assert_eq!(dns_rows(&plan), 0);
        assert_eq!(dropped, ["{A}", "{B}"]);
        assert!(!plan.is_empty(), "the rest of the profile still applies");
    }
}
