//! Profiles: named sets of desired values for catalog settings.
//!
//! A profile is data, not code, so new ones can be added without touching the
//! engine. The built-in profiles are embedded in the executable.

use std::collections::HashSet;

use serde::Deserialize;

use super::apply::Target;
use super::catalog::{self, Scope, Source};
use super::provider;
use super::reader::{State, Value};

/// In the order they are offered to the user.
const BUILTIN: &[&str] = &[
    include_str!("../../profiles/privacy.json"),
    include_str!("../../profiles/performance.json"),
    include_str!("../../profiles/recommended.json"),
];

const SERVICE_AUTO_START: u32 = 2;
const SERVICE_DEMAND_START: u32 = 3;
const SERVICE_DISABLED: u32 = 4;

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Profile {
    pub id: String,
    pub name: String,
    pub description: String,
    pub settings: Vec<Assignment>,
    /// Which DNS providers to try; profiles that leave DNS alone omit it.
    #[serde(default)]
    pub dns: Option<DnsSpec>,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct DnsSpec {
    /// Provider ids (see `provider`), in the order they are tried.
    pub providers: Vec<String>,
    pub choose: Choose,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Choose {
    /// Keep the first provider that works.
    FirstWorking,
    /// Try them all and keep the fastest that works.
    Fastest,
}

/// One catalog setting and the value a profile gives it.
#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Assignment {
    pub id: String,
    pub set: Desired,
}

#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Desired {
    Dword(u32),
    Text(String),
    /// Service start type: 2 automatic, 3 manual, 4 disabled.
    StartType(u32),
    /// Remove the value so the Windows default applies.
    Unset,
}

impl Desired {
    fn fits(&self, source: &Source) -> bool {
        match (self, source) {
            (Desired::Dword(_), Source::RegDword { .. } | Source::Wmi { .. }) => true,
            (Desired::Text(_), Source::RegString { .. }) => true,
            (Desired::StartType(start_type), Source::Service { .. }) => {
                matches!(*start_type, SERVICE_AUTO_START | SERVICE_DEMAND_START | SERVICE_DISABLED)
            }
            (Desired::Unset, Source::RegDword { .. } | Source::RegString { .. }) => true,
            _ => false,
        }
    }

    fn to_state(&self) -> State {
        match self {
            Desired::Dword(number) => State::Value(Value::Dword(*number)),
            Desired::Text(text) => State::Value(Value::Text(text.clone())),
            // A profile decides the start type only; `state` is not compared.
            Desired::StartType(start_type) => {
                State::Value(Value::Service { state: 0, start_type: *start_type })
            }
            Desired::Unset => State::NotSet,
        }
    }
}

/// Parses a profile and checks it against the catalog.
pub fn parse(json: &str) -> Result<Profile, String> {
    let profile: Profile =
        serde_json::from_str(json).map_err(|error| format!("invalid profile: {error}"))?;
    profile.validate()?;
    Ok(profile)
}

/// The profiles shipped inside the executable.
pub fn builtin() -> Result<Vec<Profile>, String> {
    BUILTIN.iter().map(|json| parse(json)).collect()
}

impl Profile {
    fn validate(&self) -> Result<(), String> {
        let mut seen = HashSet::new();
        for assignment in &self.settings {
            let id = &assignment.id;
            let setting = catalog::find(id)
                .ok_or_else(|| format!("profile {}: unknown setting {id}", self.id))?;
            if !seen.insert(id.as_str()) {
                return Err(format!("profile {}: {id} is listed twice", self.id));
            }
            if !assignment.set.fits(&setting.source) {
                return Err(format!("profile {}: {id} has a value of the wrong kind", self.id));
            }
        }
        self.validate_dns()
    }

    fn validate_dns(&self) -> Result<(), String> {
        let Some(spec) = &self.dns else {
            return Ok(());
        };
        if spec.providers.is_empty() {
            return Err(format!("profile {}: DNS providers are empty", self.id));
        }
        let mut seen = HashSet::new();
        for name in &spec.providers {
            if provider::find(name).is_none() {
                return Err(format!("profile {}: unknown DNS provider {name}", self.id));
            }
            if !seen.insert(name.as_str()) {
                return Err(format!("profile {}: DNS provider {name} is listed twice", self.id));
            }
        }
        Ok(())
    }

    /// The DNS change this profile aims for: every one of `adapters` (GUIDs)
    /// gets the first provider. Later providers are only tried when it does
    /// not work. Empty when the profile leaves DNS alone.
    pub fn dns_targets(&self, adapters: &[String]) -> Vec<Target> {
        let Some(spec) = &self.dns else {
            return Vec::new();
        };
        let (Some(first), Some(setting)) =
            (spec.providers.first().and_then(|name| provider::find(name)), catalog::find("dns.resolver"))
        else {
            return Vec::new();
        };
        adapters
            .iter()
            .map(|guid| Target { setting, adapter: Some(guid.clone()), desired: first.state() })
            .collect()
    }

    /// What applying this profile would aim for. Per-adapter settings get one
    /// target for each of `adapters` (adapter GUIDs).
    pub fn targets(&self, adapters: &[String]) -> Vec<Target> {
        let mut targets = Vec::new();
        for assignment in &self.settings {
            let Some(setting) = catalog::find(&assignment.id) else {
                continue;
            };
            let desired = assignment.set.to_state();
            match setting.scope {
                Scope::System => targets.push(Target { setting, adapter: None, desired }),
                Scope::Adapter => targets.extend(adapters.iter().map(|guid| Target {
                    setting,
                    adapter: Some(guid.clone()),
                    desired: desired.clone(),
                })),
            }
        }
        targets
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn with_settings(settings: &str) -> String {
        format!(r#"{{"id":"t","name":"T","description":"test","settings":[{settings}]}}"#)
    }

    #[test]
    fn builtin_profiles_are_valid() {
        let profiles = builtin().unwrap();
        let ids: Vec<&str> = profiles.iter().map(|profile| profile.id.as_str()).collect();
        assert_eq!(ids, ["privacy", "performance", "recommended"]);
        for profile in &profiles {
            assert!(!profile.settings.is_empty(), "{} changes nothing", profile.id);
        }
    }

    #[test]
    fn unknown_setting_is_rejected() {
        let error = parse(&with_settings(r#"{"id":"no.such.setting","set":{"dword":1}}"#));
        assert_eq!(error.unwrap_err(), "profile t: unknown setting no.such.setting");
    }

    #[test]
    fn duplicate_setting_is_rejected() {
        let entry = r#"{"id":"dns.llmnr","set":{"dword":0}}"#;
        let error = parse(&with_settings(&format!("{entry},{entry}")));
        assert_eq!(error.unwrap_err(), "profile t: dns.llmnr is listed twice");
    }

    #[test]
    fn value_of_the_wrong_kind_is_rejected() {
        for entry in [
            r#"{"id":"dns.llmnr","set":{"text":"0"}}"#,
            r#"{"id":"dns.static_servers","set":{"dword":1}}"#,
            r#"{"id":"service.diagtrack","set":{"dword":4}}"#,
            r#"{"id":"service.diagtrack","set":"unset"}"#,
            // 0 and 1 are driver start types; a service must never get them.
            r#"{"id":"service.diagtrack","set":{"start_type":0}}"#,
        ] {
            let error = parse(&with_settings(entry)).unwrap_err();
            assert!(error.ends_with("has a value of the wrong kind"), "{entry}: {error}");
        }
    }

    #[test]
    fn misspelled_field_is_rejected() {
        let error = parse(&with_settings(r#"{"id":"dns.llmnr","sett":{"dword":0}}"#));
        assert!(error.unwrap_err().starts_with("invalid profile:"));
    }

    #[test]
    fn unset_becomes_not_set() {
        let profile = parse(&with_settings(r#"{"id":"dns.llmnr","set":"unset"}"#)).unwrap();
        assert_eq!(profile.targets(&[])[0].desired, State::NotSet);
    }

    #[test]
    fn adapter_settings_get_one_target_per_adapter() {
        let profile = parse(&with_settings(
            r#"{"id":"tcp.nagle.ack_frequency","set":{"dword":1}},
               {"id":"mm.network_throttling","set":{"dword":4294967295}}"#,
        ))
        .unwrap();

        let adapters = ["{A}".to_string(), "{B}".to_string()];
        let targets = profile.targets(&adapters);
        let summary: Vec<(&str, Option<&str>)> = targets
            .iter()
            .map(|target| (target.setting.id, target.adapter.as_deref()))
            .collect();
        assert_eq!(
            summary,
            [
                ("tcp.nagle.ack_frequency", Some("{A}")),
                ("tcp.nagle.ack_frequency", Some("{B}")),
                ("mm.network_throttling", None),
            ]
        );
        assert_eq!(targets[2].desired, State::Value(Value::Dword(0xFFFF_FFFF)));

        // Without a usable adapter only the system-wide settings remain.
        assert_eq!(profile.targets(&[]).len(), 1);
    }

    fn with_dns(dns: &str) -> String {
        format!(r#"{{"id":"t","name":"T","description":"test","settings":[],"dns":{dns}}}"#)
    }

    #[test]
    fn dns_section_parses_and_unknown_provider_is_rejected() {
        let profile = parse(&with_dns(r#"{"providers":["quad9"],"choose":"fastest"}"#)).unwrap();
        let spec = profile.dns.unwrap();
        assert_eq!(spec.providers, ["quad9"]);
        assert_eq!(spec.choose, Choose::Fastest);

        let error = parse(&with_dns(r#"{"providers":["nope"],"choose":"first_working"}"#));
        assert_eq!(error.unwrap_err(), "profile t: unknown DNS provider nope");
    }

    #[test]
    fn dns_section_rejects_empty_and_duplicate_providers() {
        let empty = parse(&with_dns(r#"{"providers":[],"choose":"first_working"}"#));
        assert_eq!(empty.unwrap_err(), "profile t: DNS providers are empty");
        let twice = parse(&with_dns(r#"{"providers":["quad9","quad9"],"choose":"first_working"}"#));
        assert_eq!(twice.unwrap_err(), "profile t: DNS provider quad9 is listed twice");
    }

    #[test]
    fn builtin_profiles_have_the_designed_dns_sections() {
        let found: Vec<(String, Vec<String>, Choose)> = builtin()
            .unwrap()
            .into_iter()
            .map(|profile| {
                let spec = profile.dns.expect("every built-in profile sets DNS");
                (profile.id, spec.providers, spec.choose)
            })
            .collect();
        let providers = |names: &[&str]| names.iter().map(|name| name.to_string()).collect::<Vec<_>>();
        assert_eq!(
            found,
            [
                ("privacy".to_string(), providers(&["quad9", "cloudflare"]), Choose::FirstWorking),
                ("performance".to_string(), providers(&["cloudflare", "google"]), Choose::Fastest),
                ("recommended".to_string(), providers(&["cloudflare", "google"]), Choose::FirstWorking),
            ]
        );
    }

    #[test]
    fn dns_targets_cover_every_adapter_with_the_first_provider() {
        let profile = parse(&with_dns(r#"{"providers":["google","quad9"],"choose":"first_working"}"#)).unwrap();
        let adapters = ["{A}".to_string(), "{B}".to_string()];
        let targets = profile.dns_targets(&adapters);

        let summary: Vec<(&str, Option<&str>)> =
            targets.iter().map(|t| (t.setting.id, t.adapter.as_deref())).collect();
        assert_eq!(summary, [("dns.resolver", Some("{A}")), ("dns.resolver", Some("{B}"))]);
        let google = provider::find("google").unwrap().state();
        assert!(targets.iter().all(|target| target.desired == google));
        assert!(profile.dns_targets(&[]).is_empty());
    }

    #[test]
    fn profiles_without_a_dns_section_have_no_dns_targets() {
        let profile = parse(&with_settings(r#"{"id":"dns.llmnr","set":{"dword":0}}"#)).unwrap();
        assert!(profile.dns.is_none());
        assert!(profile.dns_targets(&["{A}".to_string()]).is_empty());
    }

    #[test]
    fn assignments_cannot_set_dns_resolver_directly() {
        let error = parse(&with_settings(r#"{"id":"dns.resolver","set":{"text":"1.1.1.1"}}"#));
        assert_eq!(error.unwrap_err(), "profile t: dns.resolver has a value of the wrong kind");
    }
}
