use std::ffi::{CStr, c_char};

use windows::Win32::NetworkManagement::IpHelper::{
    GetAdaptersInfo, IP_ADAPTER_INFO, IP_ADDR_STRING,
};
use windows::Win32::System::SystemInformation::OSVERSIONINFOW;

#[link(name = "ntdll")]
unsafe extern "system" {
    fn RtlGetVersion(lp_version_information: *mut OSVERSIONINFOW) -> i32;
}

const ERROR_SUCCESS: u32 = 0;
const ERROR_BUFFER_OVERFLOW: u32 = 111;
const ERROR_NO_DATA: u32 = 232;

struct NetworkAdapter {
    name: String,
    description: String,
    ip_addresses: Vec<String>,
    mac_address: String,
    adapter_type: &'static str,
}

fn main() {
    let version = get_windows_version();
    println!("Windows Version: {}.{}.{}", version.0, version.1, version.2);

    match get_network_adapters() {
        Ok(adapters) => {
            println!("\nNetwork Adapters ({}):", adapters.len());
            for (i, adapter) in adapters.iter().enumerate() {
                println!("\n[{}] {}", i + 1, adapter.description);
                println!("    Name:        {}", adapter.name);
                println!("    Description: {}", adapter.description);
                println!("    IP Address:  {}", adapter.ip_addresses.join(", "));
                println!("    MAC Address: {}", adapter.mac_address);
                println!("    Type:        {}", adapter.adapter_type);
            }
        }
        Err(code) => eprintln!("GetAdaptersInfo failed with error code {code}"),
    }
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

/// Lists the system's network adapters via `GetAdaptersInfo`.
/// Read-only: nothing is modified. Returns the Win32 error code on failure.
fn get_network_adapters() -> Result<Vec<NetworkAdapter>, u32> {
    let mut size = std::mem::size_of::<IP_ADAPTER_INFO>() as u32;
    // u64 backing storage keeps the buffer aligned for IP_ADAPTER_INFO.
    let mut buffer: Vec<u64> = Vec::new();

    // The required size can change between calls if adapters come and go.
    let mut result = ERROR_BUFFER_OVERFLOW;
    for _ in 0..4 {
        buffer = vec![0u64; (size as usize).div_ceil(8)];
        result = unsafe { GetAdaptersInfo(Some(buffer.as_mut_ptr().cast()), &mut size) };
        if result != ERROR_BUFFER_OVERFLOW {
            break;
        }
    }

    match result {
        ERROR_SUCCESS => {}
        ERROR_NO_DATA => return Ok(Vec::new()),
        code => return Err(code),
    }

    let mut adapters = Vec::new();
    let mut current = buffer.as_ptr().cast::<IP_ADAPTER_INFO>();

    while !current.is_null() {
        let info = unsafe { &*current };

        let mut ip_addresses = Vec::new();
        let mut ip_entry: *const IP_ADDR_STRING = &info.IpAddressList;
        while !ip_entry.is_null() {
            let entry = unsafe { &*ip_entry };
            let ip = c_buf_to_string(entry.IpAddress.String.as_ptr().cast());
            if !ip.is_empty() {
                ip_addresses.push(ip);
            }
            ip_entry = entry.Next;
        }

        let mac_len = (info.AddressLength as usize).min(info.Address.len());
        let mac_address = info.Address[..mac_len]
            .iter()
            .map(|byte| format!("{byte:02X}"))
            .collect::<Vec<_>>()
            .join("-");

        adapters.push(NetworkAdapter {
            name: c_buf_to_string(info.AdapterName.as_ptr().cast()),
            description: c_buf_to_string(info.Description.as_ptr().cast()),
            ip_addresses,
            mac_address,
            adapter_type: adapter_type_name(info.Type),
        });

        current = info.Next;
    }

    Ok(adapters)
}

/// Reads a NUL-terminated ANSI string out of a fixed-size API buffer.
fn c_buf_to_string(ptr: *const c_char) -> String {
    unsafe { CStr::from_ptr(ptr) }.to_string_lossy().into_owned()
}

/// Maps an IANA ifType value (`IP_ADAPTER_INFO::Type`) to a readable name.
fn adapter_type_name(if_type: u32) -> &'static str {
    match if_type {
        6 => "Ethernet",
        9 => "Token Ring",
        15 => "FDDI",
        23 => "PPP",
        24 => "Loopback",
        28 => "SLIP",
        71 => "WiFi (IEEE 802.11)",
        131 => "Tunnel",
        _ => "Other",
    }
}
