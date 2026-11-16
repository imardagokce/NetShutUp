//! Reads and changes how a Windows service starts.

use std::thread::sleep;
use std::time::Duration;

use windows::Win32::System::Services::{
    ChangeServiceConfigW, CloseServiceHandle, ControlService, ENUM_SERVICE_TYPE, OpenSCManagerW,
    OpenServiceW, QUERY_SERVICE_CONFIGW, QueryServiceConfigW, QueryServiceStatus, SC_HANDLE,
    SC_MANAGER_CONNECT, SERVICE_CHANGE_CONFIG, SERVICE_CONTROL_STOP, SERVICE_ERROR,
    SERVICE_NO_CHANGE, SERVICE_QUERY_CONFIG, SERVICE_QUERY_STATUS, SERVICE_START,
    SERVICE_START_TYPE, SERVICE_STATUS, SERVICE_STOP, StartServiceW,
};
use windows::core::{HSTRING, PCWSTR};

use super::reader::{State, Value};

pub const STATE_STOPPED: u32 = 1;
pub const STATE_RUNNING: u32 = 4;

pub const START_AUTOMATIC: u32 = 2;
pub const START_MANUAL: u32 = 3;
pub const START_DISABLED: u32 = 4;

const ERROR_ACCESS_DENIED: u32 = 5;

/// How long a service gets to stop before we move on.
const STOP_TIMEOUT: Duration = Duration::from_secs(15);
const POLL_INTERVAL: Duration = Duration::from_millis(250);

fn describe(error: &windows::core::Error) -> String {
    match error.code().0 as u32 & 0xFFFF {
        ERROR_ACCESS_DENIED => "access denied (administrator rights required)".to_string(),
        code => format!("service error {code}"),
    }
}

/// Opens a service, runs `action` on it and closes the handles again.
fn with_service<T>(
    name: &str,
    access: u32,
    action: impl FnOnce(SC_HANDLE) -> Result<T, String>,
) -> Result<T, String> {
    unsafe {
        let manager = OpenSCManagerW(PCWSTR::null(), PCWSTR::null(), SC_MANAGER_CONNECT)
            .map_err(|error| describe(&error))?;

        let result = match OpenServiceW(manager, &HSTRING::from(name), access) {
            Ok(service) => {
                let result = action(service);
                let _ = CloseServiceHandle(service);
                result
            }
            Err(error) => Err(describe(&error)),
        };

        let _ = CloseServiceHandle(manager);
        result
    }
}

fn current_state(service: SC_HANDLE) -> Result<u32, String> {
    let mut status = SERVICE_STATUS::default();
    unsafe { QueryServiceStatus(service, &mut status) }.map_err(|error| describe(&error))?;
    Ok(status.dwCurrentState.0)
}

pub(super) fn read(name: &str) -> State {
    let read = with_service(name, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG, |service| {
        let state = current_state(service)?;

        // u64 storage keeps the buffer aligned; 8 KB is the API maximum.
        let mut config = vec![0u64; 1024];
        let mut needed = 0u32;
        unsafe {
            QueryServiceConfigW(
                service,
                Some(config.as_mut_ptr().cast::<QUERY_SERVICE_CONFIGW>()),
                (config.len() * 8) as u32,
                &mut needed,
            )
        }
        .map_err(|error| describe(&error))?;

        let config = unsafe { &*config.as_ptr().cast::<QUERY_SERVICE_CONFIGW>() };
        Ok(Value::Service { state, start_type: config.dwStartType.0 })
    });

    match read {
        Ok(value) => State::Value(value),
        Err(reason) => State::Unavailable(reason),
    }
}

/// Sets a service's start type, then brings its running state in line:
/// a disabled service is stopped, and `state` = running starts it again
/// (used when a backup is restored).
///
/// Only the start type decides success. A service that will not stop or
/// start right now still follows the new start type from the next boot.
pub(super) fn write(name: &str, start_type: u32, state: u32) -> Result<(), String> {
    // 0 and 1 are driver start types and would break a normal service.
    if !matches!(start_type, START_AUTOMATIC | START_MANUAL | START_DISABLED) {
        return Err(format!("start type {start_type} is not valid for a service"));
    }

    let access = SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS | SERVICE_STOP | SERVICE_START;
    with_service(name, access, |service| {
        unsafe {
            ChangeServiceConfigW(
                service,
                ENUM_SERVICE_TYPE(SERVICE_NO_CHANGE),
                SERVICE_START_TYPE(start_type),
                SERVICE_ERROR(SERVICE_NO_CHANGE),
                PCWSTR::null(),
                PCWSTR::null(),
                None,
                PCWSTR::null(),
                PCWSTR::null(),
                PCWSTR::null(),
                PCWSTR::null(),
            )
        }
        .map_err(|error| describe(&error))?;

        if start_type == START_DISABLED {
            stop(service);
        } else if state == STATE_RUNNING {
            // Fails harmlessly when the service is already running.
            let _ = unsafe { StartServiceW(service, None) };
        }
        Ok(())
    })
}

fn stop(service: SC_HANDLE) {
    if current_state(service) == Ok(STATE_STOPPED) {
        return;
    }

    let mut status = SERVICE_STATUS::default();
    if unsafe { ControlService(service, SERVICE_CONTROL_STOP, &mut status) }.is_err() {
        return;
    }

    let mut waited = Duration::ZERO;
    while waited < STOP_TIMEOUT && current_state(service) != Ok(STATE_STOPPED) {
        sleep(POLL_INTERVAL);
        waited += POLL_INTERVAL;
    }
}

pub(super) fn state_name(state: u32) -> &'static str {
    match state {
        1 => "Stopped",
        2 => "Starting",
        3 => "Stopping",
        4 => "Running",
        7 => "Paused",
        _ => "Other",
    }
}

pub(super) fn start_type_name(start_type: u32) -> &'static str {
    match start_type {
        0 | 1 => "Driver",
        START_AUTOMATIC => "Automatic",
        START_MANUAL => "Manual",
        START_DISABLED => "Disabled",
        _ => "Other",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn driver_start_types_are_refused_before_touching_anything() {
        for start_type in [0, 1, 5, SERVICE_NO_CHANGE] {
            let error = write("NetShutUp-No-Such-Service", start_type, 0).unwrap_err();
            assert!(error.contains("is not valid for a service"), "{error}");
        }
    }

    #[test]
    fn missing_service_is_reported_not_created() {
        // 1060 = ERROR_SERVICE_DOES_NOT_EXIST
        let name = "NetShutUp-No-Such-Service";
        assert_eq!(read(name), State::Unavailable("service error 1060".to_string()));
        assert_eq!(write(name, START_MANUAL, 0), Err("service error 1060".to_string()));
    }
}
