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

/// Runs this program again as administrator and waits for it to finish.
/// Windows asks the user for permission first. `args` must not contain
/// spaces or quotes; they are passed on as written.
pub fn relaunch_elevated(args: &[&str]) -> Relaunch {
    let program = match std::env::current_exe() {
        Ok(path) => HSTRING::from(path.as_os_str()),
        Err(error) => return Relaunch::Failed(format!("cannot locate the program: {error}")),
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
            return match error.code().0 as u32 & 0xFFFF {
                ERROR_CANCELLED => Relaunch::Declined,
                code => Relaunch::Failed(format!("could not start as administrator (error {code})")),
            };
        }
        if info.hProcess.is_invalid() {
            return Relaunch::Failed("the administrator copy did not start".to_string());
        }

        WaitForSingleObject(info.hProcess, INFINITE);
        let mut exit_code = 0u32;
        let read = GetExitCodeProcess(info.hProcess, &mut exit_code);
        let _ = CloseHandle(info.hProcess);

        match read {
            Ok(()) => Relaunch::Finished(exit_code),
            Err(_) => Relaunch::Failed("the administrator copy's result is unknown".to_string()),
        }
    }
}
