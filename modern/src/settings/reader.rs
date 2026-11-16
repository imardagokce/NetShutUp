//! Value types for catalog settings, plus how they are compared to the
//! Windows default and rendered. Also reads Windows service state.

use windows::Win32::System::Services::{
    CloseServiceHandle, OpenSCManagerW, OpenServiceW, QUERY_SERVICE_CONFIGW, QueryServiceConfigW,
    QueryServiceStatus, SERVICE_STATUS,
};
use windows::core::{HSTRING, PCWSTR};

use serde::{Deserialize, Serialize};

use super::catalog::{DefaultValue, Setting};


const SC_MANAGER_CONNECT: u32 = 0x0001;
const SERVICE_QUERY_CONFIG: u32 = 0x0001;
const SERVICE_QUERY_STATUS: u32 = 0x0004;

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
            service_state_name(*state),
            service_start_type_name(*start_type)
        ),
    }
}

/// Windows stores address lists separated by commas or spaces.
pub(super) fn normalize_list(raw: &str) -> String {
    raw.split([',', ' '])
        .filter(|part| !part.is_empty())
        .collect::<Vec<_>>()
        .join(", ")
}

pub(super) fn read_service(name: &str) -> State {
    let unavailable = |error: windows::core::Error| {
        State::Unavailable(format!("service error {}", error.code().0 as u32 & 0xFFFF))
    };

    unsafe {
        let manager = match OpenSCManagerW(PCWSTR::null(), PCWSTR::null(), SC_MANAGER_CONNECT) {
            Ok(handle) => handle,
            Err(error) => return unavailable(error),
        };

        let state = match OpenServiceW(
            manager,
            &HSTRING::from(name),
            SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG,
        ) {
            Ok(service) => {
                let mut status = SERVICE_STATUS::default();
                // u64 storage keeps the buffer aligned; 8 KB is the API maximum.
                let mut config = vec![0u64; 1024];
                let mut needed = 0u32;

                let queried = QueryServiceStatus(service, &mut status).and_then(|()| {
                    QueryServiceConfigW(
                        service,
                        Some(config.as_mut_ptr().cast::<QUERY_SERVICE_CONFIGW>()),
                        (config.len() * 8) as u32,
                        &mut needed,
                    )
                });

                let _ = CloseServiceHandle(service);

                match queried {
                    Ok(()) => {
                        let config = &*config.as_ptr().cast::<QUERY_SERVICE_CONFIGW>();
                        State::Value(Value::Service {
                            state: status.dwCurrentState.0,
                            start_type: config.dwStartType.0,
                        })
                    }
                    Err(error) => unavailable(error),
                }
            }
            Err(error) => unavailable(error),
        };

        let _ = CloseServiceHandle(manager);
        state
    }
}

fn service_state_name(state: u32) -> &'static str {
    match state {
        1 => "Stopped",
        2 => "Starting",
        3 => "Stopping",
        4 => "Running",
        7 => "Paused",
        _ => "Other",
    }
}

fn service_start_type_name(start_type: u32) -> &'static str {
    match start_type {
        0 | 1 => "Driver",
        2 => "Automatic",
        3 => "Manual",
        4 => "Disabled",
        _ => "Other",
    }
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
