//! Value types for catalog settings, plus how they are compared to the
//! Windows default and rendered.

use serde::{Deserialize, Serialize};

use super::catalog::{DefaultValue, Setting};
use super::service;

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Value {
    Dword(u32),
    Text(String),
    Service { state: u32, start_type: u32 },
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum State {
    /// Nothing is configured; the Windows default applies.
    NotSet,
    Value(Value),
    Unavailable(String),
}

#[derive(Debug, PartialEq, Eq)]
pub enum Origin {
    Default,
    Changed,
    Unknown,
}

/// Compares a state against the setting's Windows default.
pub fn classify(setting: &Setting, state: &State) -> Origin {
    let value = match state {
        State::NotSet => return Origin::Default,
        State::Unavailable(_) => return Origin::Unknown,
        State::Value(value) => value,
    };

    let is_default = match (&setting.default, value) {
        // An empty string configures nothing, exactly like a missing value.
        (Some(DefaultValue::Unset), Value::Text(text)) => text.trim().is_empty(),
        (Some(DefaultValue::Unset), _) => false,
        (Some(DefaultValue::Dword(default)), Value::Dword(current)) => default == current,
        (Some(DefaultValue::ServiceStartType(default)), Value::Service { start_type, .. }) => {
            default == start_type
        }
        _ => return Origin::Unknown,
    };

    if is_default { Origin::Default } else { Origin::Changed }
}

/// Whether `actual` already satisfies `desired`. Address lists match
/// regardless of separator; services match on start type.
pub fn matches(desired: &State, actual: &State) -> bool {
    match (desired, actual) {
        (State::NotSet, State::NotSet) => true,
        (State::Value(Value::Dword(want)), State::Value(Value::Dword(have))) => want == have,
        (State::Value(Value::Text(want)), State::Value(Value::Text(have))) => {
            normalize_list(want) == normalize_list(have)
        }
        (
            State::Value(Value::Service { start_type: want, .. }),
            State::Value(Value::Service { start_type: have, .. }),
        ) => want == have,
        _ => false,
    }
}

/// Renders a state for display.
pub fn format(setting: &Setting, state: &State) -> String {
    match state {
        State::NotSet => "not set".to_string(),
        State::Unavailable(reason) => format!("unavailable ({reason})"),
        State::Value(Value::Dword(number)) => setting
            .labels
            .iter()
            .find(|(labelled, _)| labelled == number)
            .map(|(_, label)| label.to_string())
            .unwrap_or_else(|| number.to_string()),
        State::Value(Value::Text(text)) => match normalize_list(text) {
            list if list.is_empty() => "not set".to_string(),
            list => list,
        },
        State::Value(Value::Service { state, start_type }) => format!(
            "{}, {}",
            service::state_name(*state),
            service::start_type_name(*start_type)
        ),
    }
}

/// Renders a state that a profile asks for. A service is shown by start type
/// alone, because that is all a profile decides.
pub fn format_desired(setting: &Setting, state: &State) -> String {
    match state {
        State::Value(Value::Service { start_type, .. }) => {
            service::start_type_name(*start_type).to_string()
        }
        other => format(setting, other),
    }
}

/// Windows stores address lists separated by commas or spaces.
pub(super) fn normalize_list(raw: &str) -> String {
    raw.split([',', ' '])
        .filter(|part| !part.is_empty())
        .collect::<Vec<_>>()
        .join(", ")
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::settings::catalog::CATALOG;

    fn setting(id: &str) -> &'static Setting {
        CATALOG.iter().find(|setting| setting.id == id).unwrap()
    }

    #[test]
    fn not_set_is_default() {
        assert_eq!(classify(setting("tcp.nagle.no_delay"), &State::NotSet), Origin::Default);
    }

    #[test]
    fn dword_is_compared_to_default() {
        let throttling = setting("mm.network_throttling");
        let at = |n| State::Value(Value::Dword(n));
        assert_eq!(classify(throttling, &at(10)), Origin::Default);
        assert_eq!(classify(throttling, &at(0xFFFF_FFFF)), Origin::Changed);
    }

    #[test]
    fn any_static_dns_is_a_change() {
        let empty = State::Value(Value::Text(String::new()));
        assert_eq!(classify(setting("dns.static_servers"), &empty), Origin::Default);
        assert_eq!(format(setting("dns.static_servers"), &empty), "not set");
        let state = State::Value(Value::Text("9.9.9.9".into()));
        assert_eq!(classify(setting("dns.static_servers"), &state), Origin::Changed);
    }

    #[test]
    fn unverified_default_gets_no_label() {
        let state = State::Value(Value::Dword(0));
        assert_eq!(classify(setting("qos.reservable_bandwidth"), &state), Origin::Unknown);
    }

    #[test]
    fn service_is_compared_by_start_type() {
        let diagtrack = setting("service.diagtrack");
        let with = |state, start_type| State::Value(Value::Service { state, start_type });
        assert_eq!(classify(diagtrack, &with(4, 2)), Origin::Default);
        assert_eq!(classify(diagtrack, &with(1, 4)), Origin::Changed);
        assert_eq!(format(diagtrack, &with(1, 4)), "Stopped, Disabled");
    }

    #[test]
    fn labels_replace_raw_numbers() {
        let throttling = setting("mm.network_throttling");
        assert_eq!(format(throttling, &State::Value(Value::Dword(0xFFFF_FFFF))), "disabled");
        assert_eq!(format(throttling, &State::Value(Value::Dword(10))), "10");
    }

    #[test]
    fn address_lists_are_normalized() {
        assert_eq!(normalize_list("9.9.9.9,149.112.112.112"), "9.9.9.9, 149.112.112.112");
        assert_eq!(normalize_list("1.1.1.1 1.0.0.1"), "1.1.1.1, 1.0.0.1");
        assert_eq!(normalize_list(""), "");
    }
}
