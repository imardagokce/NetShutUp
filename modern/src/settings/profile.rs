//! Profiles: named sets of desired values for catalog settings.
//!
//! A profile is data, not code, so new ones can be added without touching the
//! engine. The built-in profiles are embedded in the executable.

use std::collections::HashSet;

use serde::Deserialize;

use super::apply::Target;
use super::catalog::{self, Scope, Source};
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
        Ok(())
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
}
