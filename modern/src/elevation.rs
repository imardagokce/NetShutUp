//! Administrator rights: detecting them, and restarting the program with them.

use std::mem::size_of;

use windows::Win32::Foundation::{CloseHandle, HANDLE};
use windows::Win32::Security::{GetTokenInformation, TOKEN_ELEVATION, TOKEN_QUERY, TokenElevation};
use windows::Win32::System::Threading::{
    GetCurrentProcess, GetExitCodeProcess, INFINITE, OpenProcessToken, WaitForSingleObject,
};
use windows::Win32::UI::Shell::{SEE_MASK_NOCLOSEPROCESS, SHELLEXECUTEINFOW, ShellExecuteExW};
use windows::core::{HSTRING, PCWSTR, w};

const SW_SHOWNORMAL: i32 = 1;
/// The user answered "No" to the Windows permission prompt.
const ERROR_CANCELLED: u32 = 1223;

pub fn is_elevated() -> bool {
    unsafe {
        let mut token = HANDLE::default();
        if OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token).is_err() {
            return false;
        }

        let mut elevation = TOKEN_ELEVATION::default();
        let mut returned = 0u32;
        let queried = GetTokenInformation(
            token,
            TokenElevation,
            Some((&raw mut elevation).cast()),
            size_of::<TOKEN_ELEVATION>() as u32,
            &mut returned,
        );
        let _ = CloseHandle(token);

        queried.is_ok() && elevation.TokenIsElevated != 0
    }
}

pub enum Relaunch {
    /// The elevated copy ran and exited with this code.
    Finished(u32),
    /// The user declined the Windows permission prompt.
    Declined,
    Failed(String),
}

/// The elevated copy has been started: not waiting for it.
pub enum Start {
    Started,
    /// The user declined the Windows permission prompt.
    Declined,
    Failed(String),
}

enum LaunchError {
    Declined,
    Failed(String),
}

/// Runs this program again as administrator and waits for it to finish.
/// Windows asks the user for permission first. `args` must not contain
/// spaces or quotes; they are passed on as written.
pub fn relaunch_elevated(args: &[&str]) -> Relaunch {
    let process = match launch(args) {
        Ok(process) => process,
        Err(LaunchError::Declined) => return Relaunch::Declined,
        Err(LaunchError::Failed(reason)) => return Relaunch::Failed(reason),
    };

    unsafe {
        WaitForSingleObject(process, INFINITE);
        let mut exit_code = 0u32;
        let read = GetExitCodeProcess(process, &mut exit_code);
        let _ = CloseHandle(process);

        match read {
            Ok(()) => Relaunch::Finished(exit_code),
            Err(_) => Relaunch::Failed("the administrator copy's result is unknown".to_string()),
        }
    }
}

/// Runs this program again as administrator and returns as soon as it has
/// started, so the caller can exit and leave the work to the new window.
/// Same rules for `args` as `relaunch_elevated`.
pub fn start_elevated(args: &[&str]) -> Start {
    match launch(args) {
        Ok(process) => {
            unsafe {
                let _ = CloseHandle(process);
            }
            Start::Started
        }
        Err(LaunchError::Declined) => Start::Declined,
        Err(LaunchError::Failed(reason)) => Start::Failed(reason),
    }
}

/// Asks Windows to start this program as administrator. The caller owns the
/// returned process handle.
fn launch(args: &[&str]) -> Result<HANDLE, LaunchError> {
    let program = match std::env::current_exe() {
        Ok(path) => HSTRING::from(path.as_os_str()),
        Err(error) => {
            return Err(LaunchError::Failed(format!("cannot locate the program: {error}")));
        }
    };
    let parameters = HSTRING::from(args.join(" "));

    let mut info = SHELLEXECUTEINFOW {
        cbSize: size_of::<SHELLEXECUTEINFOW>() as u32,
        fMask: SEE_MASK_NOCLOSEPROCESS,
        lpVerb: w!("runas"),
        lpFile: PCWSTR(program.as_ptr()),
        lpParameters: PCWSTR(parameters.as_ptr()),
        nShow: SW_SHOWNORMAL,
        ..Default::default()
    };

    unsafe {
        if let Err(error) = ShellExecuteExW(&mut info) {
            return Err(match error.code().0 as u32 & 0xFFFF {
                ERROR_CANCELLED => LaunchError::Declined,
                code => LaunchError::Failed(format!("could not start as administrator (error {code})")),
            });
        }
    }
    if info.hProcess.is_invalid() {
        return Err(LaunchError::Failed("the administrator copy did not start".to_string()));
    }
    Ok(info.hProcess)
}
