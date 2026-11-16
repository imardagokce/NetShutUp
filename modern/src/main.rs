use windows::Win32::System::SystemInformation::OSVERSIONINFOW;

#[link(name = "ntdll")]
unsafe extern "system" {
    fn RtlGetVersion(lp_version_information: *mut OSVERSIONINFOW) -> i32;
}

fn main() {
    let version = get_windows_version();
    println!("Windows Version: {}.{}.{}", version.0, version.1, version.2);
}

fn get_windows_version() -> (u32, u32, u32) {
    let mut info = OSVERSIONINFOW {
        dwOSVersionInfoSize: std::mem::size_of::<OSVERSIONINFOW>() as u32,
        ..Default::default()
    };

    unsafe {
        RtlGetVersion(&mut info);
    }

    (
        info.dwMajorVersion,
        info.dwMinorVersion,
        info.dwBuildNumber,
    )
}