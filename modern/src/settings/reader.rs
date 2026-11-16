//! Reads the current value of catalog settings. Strictly read-only: registry
//! values are fetched with `RegGetValueW` and services are opened with query
//! rights only.

use std::ffi::c_void;

use windows::Win32::System::Registry::{
    HKEY_LOCAL_MACHINE, RRF_RT_REG_DWORD, RRF_RT_REG_SZ, RegGetValueW,
};
use windows::Win32::System::Services::{
    CloseServiceHandle, OpenSCManagerW, OpenServiceW, QUERY_SERVICE_CONFIGW, QueryServiceConfigW,
    QueryServiceStatus, SERVICE_STATUS,
};
use windows::core::{HSTRING, PCWSTR};

use super::catalog::{ADAPTER_GUID, DefaultValue, Setting, Source};
use super::wmi;

const ERROR_SUCCESS: u32 = 0;
const ERROR_FILE_NOT_FOUND: u32 = 2;

const SC_MANAGER_CONNECT: u32 = 0x0001;
const SERVICE_QUERY_CONFIG: u32 = 0x0001;
const SERVICE_QUERY_STATUS: u32 = 0x0004;

#[derive(Debug, PartialEq, Eq)]
pub enum Value {
    Dword(u32),
    Text(String),
    Service { state: u32, start_type: u32 },
}

#[derive(Debug, PartialEq, Eq)]
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

/// Reads a setting. `adapter_guid` is required for adapter-scoped settings.
pub fn read(setting: &Setting, adapter_guid: Option<&str>) -> State {
    let resolve = |key: &str| key.replace(ADAPTER_GUID, adapter_guid.unwrap_or_default());

    match setting.source {
        Source::RegDword { key, value } => read_reg_dword(&resolve(key), value),
        Source::RegString { key, value } => read_reg_string(&resolve(key), value),
        Source::Service { name } => read_service(name),
        Source::Wmi { class, filter, property } => match wmi::query_u32(class, filter, property) {
            Ok(Some(number)) => State::Value(Value::Dword(number)),
            Ok(None) => State::Unavailable("no value reported".to_string()),
            Err(reason) => State::Unavailable(reason),
        },
    }
}

/// Compares a state against the setting's Windows default.
pub fn classify(setting: &Setting, state: &State) -> Origin {
    let value = match state {
        State::NotSet => return Origin::Default,
        State::Unavailable(_) => return Origin::Unknown,
        State::Value(value) => value,
    };

    let is_default = match (&setting.default, value) {
        (Some(DefaultValue::Unset), _) => false,
        (Some(DefaultValue::Dword(default)), Value::Dword(current)) => default == current,
        (Some(DefaultValue::ServiceStartType(default)), Value::Service { start_type, .. }) => {
            default == start_type
        }
        _ => return Origin::Unknown,
    };

    if is_default { Origin::Default } else { Origin::Changed }
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
        State::Value(Value::Text(text)) => text.clone(),
        State::Value(Value::Service { state, start_type }) => format!(
            "{}, {}",
            service_state_name(*state),
            service_start_type_name(*start_type)
        ),
    }
}

fn read_reg_dword(key: &str, value: &str) -> State {
    let mut data: u32 = 0;
    let mut size = std::mem::size_of::<u32>() as u32;

    let result = unsafe {
        RegGetValueW(
            HKEY_LOCAL_MACHINE,
            &HSTRING::from(key),
            &HSTRING::from(value),
            RRF_RT_REG_DWORD,
            None,
            Some((&raw mut data).cast::<c_void>()),
            Some(&mut size),
        )
    };

    match result.0 {
        ERROR_SUCCESS => State::Value(Value::Dword(data)),
        ERROR_FILE_NOT_FOUND => State::NotSet,
        code => State::Unavailable(format!("registry error {code}")),
    }
}

fn read_reg_string(key: &str, value: &str) -> State {
    let mut buffer = [0u16; 1024];
    let mut size = std::mem::size_of_val(&buffer) as u32;

    let result = unsafe {
        RegGetValueW(
            HKEY_LOCAL_MACHINE,
            &HSTRING::from(key),
            &HSTRING::from(value),
            RRF_RT_REG_SZ,
            None,
            Some(buffer.as_mut_ptr().cast::<c_void>()),
            Some(&mut size),
        )
    };

    match result.0 {
        ERROR_SUCCESS => {
            let len = buffer.iter().position(|&unit| unit == 0).unwrap_or(buffer.len());
            match normalize_list(&String::from_utf16_lossy(&buffer[..len])) {
                text if text.is_empty() => State::NotSet,
                text => State::Value(Value::Text(text)),
            }
        }
        ERROR_FILE_NOT_FOUND => State::NotSet,
        code => State::Unavailable(format!("registry error {code}")),
    }
}

/// Windows stores address lists separated by commas or spaces.
fn normalize_list(raw: &str) -> String {
    raw.split([',', ' '])
        .filter(|part| !part.is_empty())
        .collect::<Vec<_>>()
        .join(", ")
}

fn read_service(name: &str) -> State {
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
    use crate::settings::catalog::{CATALOG, Scope};

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

    #[test]
    fn missing_registry_value_reads_as_not_set() {
        assert_eq!(
            read_reg_dword(r"SOFTWARE\NetShutUp\DoesNotExist", "Missing"),
            State::NotSet
        );
    }

    #[test]
    fn every_system_setting_can_be_read() {
        for setting in CATALOG.iter().filter(|s| s.scope == Scope::System) {
            let state = read(setting, None);
            assert!(
                !matches!(state, State::Unavailable(_)),
                "{}: {state:?}",
                setting.id
            );
        }
    }
}
