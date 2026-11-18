//! Building the plan of a profile, and the DNS-specific rules around it. Used
//! by the commands and by the self-test, so both plan exactly the same way.

use super::apply::{self, PlannedChange, Target};
use super::profile::{OptionalItem, Profile};
use super::reader::State;
use super::store::Store;
use crate::dns::Precheck;

pub const DNS_SETTING: &str = "dns.resolver";

pub const FIREWALL_PREFIX: &str = "firewall.";

pub fn is_firewall(change: &PlannedChange) -> bool {
    change.target.setting.id.starts_with(FIREWALL_PREFIX)
}

/// The plan without its Windows Firewall changes.
pub fn drop_firewall(plan: Vec<PlannedChange>) -> Vec<PlannedChange> {
    plan.into_iter().filter(|change| !is_firewall(change)).collect()
}

/// The first registered firewall product that is not Windows' own. Windows
/// Security Center lists Windows' firewall as "Windows ..."; an empty or
/// unreadable list means Windows Firewall is assumed.
pub fn third_party_firewall(products: &[String]) -> Option<String> {
    products
        .iter()
        .find(|name| {
            !name.trim().is_empty() && !name.trim_start().to_lowercase().starts_with("windows")
        })
        .cloned()
}

/// What the preview and apply say when the firewall settings are skipped.
pub fn third_party_note(name: &str) -> String {
    format!("Another firewall ({name}) is registered, so Windows Firewall settings were left alone.")
}

/// An optional item that would change something, with the change it would make.
#[derive(Clone)]
pub struct OptionalChange {
    pub item: OptionalItem,
    pub change: PlannedChange,
}

/// The optional items of `profile` with what each would change. None when
/// `third_party` names another registered firewall.
fn planned_optional(
    store: &dyn Store,
    profile: &Profile,
    third_party: Option<&str>,
) -> Vec<OptionalChange> {
    if third_party.is_some() {
        return Vec::new();
    }
    let (items, targets): (Vec<OptionalItem>, Vec<Target>) =
        profile.optional_targets().into_iter().unzip();
    items
        .into_iter()
        .zip(apply::plan(store, targets))
        .map(|(item, change)| OptionalChange { item, change })
        .collect()
}

/// The optional items that would change something and can be applied. Each one
/// is asked about after the main confirmation; none when a third-party firewall
/// is registered.
pub fn optional_changes(
    store: &dyn Store,
    profile: &Profile,
    third_party: Option<&str>,
) -> Vec<OptionalChange> {
    planned_optional(store, profile, third_party)
        .into_iter()
        .filter(|optional| {
            optional.change.is_needed()
                && apply::check(std::slice::from_ref(&optional.change)).is_ok()
        })
        .collect()
}

/// Every optional item as if approved, for the self-test. Empty when
/// `third_party` names another registered firewall.
pub fn all_optional_changes(
    store: &dyn Store,
    profile: &Profile,
    third_party: Option<&str>,
) -> Vec<PlannedChange> {
    planned_optional(store, profile, third_party).into_iter().map(|o| o.change).collect()
}

pub fn is_dns(change: &PlannedChange) -> bool {
    change.target.setting.id == DNS_SETTING
}

/// What applying `profile` to `adapters` (GUIDs) would change. The DNS stage is
/// included only when `dns_available`. Returns the adapters whose DNS could not
/// be read and was left out. The firewall settings are left out when
/// `third_party_firewall` names another registered firewall.
pub fn plan_profile(
    store: &dyn Store,
    profile: &Profile,
    adapters: &[String],
    dns_available: bool,
    third_party_firewall: Option<&str>,
) -> (Vec<PlannedChange>, Vec<String>) {
    let mut targets = profile.targets(adapters);
    if dns_available {
        targets.extend(profile.dns_targets(adapters));
    }
    let planned = apply::plan(store, targets);
    let planned = if third_party_firewall.is_some() { drop_firewall(planned) } else { planned };
    drop_unreadable_dns(planned)
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

        let (with_dns, dropped) = plan_profile(&store, &privacy(), &guids, true, None);
        assert_eq!(dns_rows(&with_dns), 2);
        assert!(dropped.is_empty());

        let (without_dns, _) = plan_profile(&store, &privacy(), &guids, false, None);
        assert_eq!(dns_rows(&without_dns), 0);
        assert!(without_dns.len() < with_dns.len());
    }

    #[test]
    fn plan_profile_drops_the_dns_of_adapters_that_cannot_be_read() {
        let store = FakeStore { unreadable_id: Some("dns.resolver"), ..Default::default() };
        let guids = ["{A}".to_string(), "{B}".to_string()];

        let (plan, dropped) = plan_profile(&store, &privacy(), &guids, true, None);

        assert_eq!(dns_rows(&plan), 0);
        assert_eq!(dropped, ["{A}", "{B}"]);
        assert!(!plan.is_empty(), "the rest of the profile still applies");
    }

    fn firewall_rows(plan: &[PlannedChange]) -> usize {
        plan.iter().filter(|change| is_firewall(change)).count()
    }

    #[test]
    fn third_party_firewall_ignores_windows_and_empty_lists() {
        let list = |names: &[&str]| -> Vec<String> { names.iter().map(|n| n.to_string()).collect() };
        assert_eq!(third_party_firewall(&list(&["Windows Defender Firewall"])), None);
        assert_eq!(third_party_firewall(&list(&["WINDOWS firewall"])), None);
        assert_eq!(third_party_firewall(&[]), None);
        assert_eq!(
            third_party_firewall(&list(&["Windows Defender Firewall", "Norton Security"])),
            Some("Norton Security".to_string())
        );
    }

    #[test]
    fn plan_profile_drops_firewall_changes_when_a_third_party_firewall_is_registered() {
        let store = FakeStore::default();
        let guids = ["{A}".to_string()];

        let (with, _) = plan_profile(&store, &privacy(), &guids, true, None);
        assert!(firewall_rows(&with) > 0);
        let (without, _) = plan_profile(&store, &privacy(), &guids, true, Some("Norton"));
        assert_eq!(firewall_rows(&without), 0);
        assert_eq!(without.len(), with.len() - firewall_rows(&with), "the rest is unchanged");
    }

    #[test]
    fn firewall_changes_stay_when_there_is_no_third_party_firewall() {
        let store = FakeStore::default();
        let (plan, _) = plan_profile(&store, &privacy(), &[], false, None);
        assert_eq!(firewall_rows(&plan), 6);
        assert!(plan.iter().filter(|c| is_firewall(c)).all(|c| c.is_needed()));
    }

    fn dword(value: u32) -> State {
        State::Value(Value::Dword(value))
    }

    fn optional_ids(changes: &[OptionalChange]) -> Vec<&str> {
        changes.iter().map(|change| change.item.id.as_str()).collect()
    }

    #[test]
    fn optional_changes_only_lists_items_that_would_change() {
        let store = FakeStore::default();
        store.seed("firewall.public.network_discovery", None, dword(0));
        store.seed("firewall.public.file_printer_sharing", None, dword(1));
        store.seed("firewall.public.remote_assistance", None, dword(0));

        let changes = optional_changes(&store, &privacy(), None);

        assert_eq!(optional_ids(&changes), ["firewall.rule.block_telemetry", "firewall.public.file_printer_sharing"]);
    }

    #[test]
    fn optional_changes_are_empty_with_a_third_party_firewall() {
        let store = FakeStore::default();
        assert!(optional_changes(&store, &privacy(), Some("Norton")).is_empty());
        assert!(all_optional_changes(&store, &privacy(), Some("Norton")).is_empty());
    }

    #[test]
    fn optional_changes_skip_an_item_that_cannot_be_read() {
        let store = FakeStore { unreadable_id: Some("firewall.rule.block_telemetry"), ..Default::default() };

        let changes = optional_changes(&store, &privacy(), None);

        assert_eq!(
            optional_ids(&changes),
            [
                "firewall.public.network_discovery",
                "firewall.public.file_printer_sharing",
                "firewall.public.remote_assistance",
            ]
        );
    }

    #[test]
    fn all_optional_changes_covers_every_optional_item_for_the_self_test() {
        let store = FakeStore::default();
        assert_eq!(all_optional_changes(&store, &privacy(), None).len(), 4);
    }

    #[test]
    fn the_preview_note_names_the_other_firewall() {
        assert_eq!(
            third_party_note("Norton"),
            "Another firewall (Norton) is registered, so Windows Firewall settings were left alone."
        );
    }
}
