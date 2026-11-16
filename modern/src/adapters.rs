use std::net::{Ipv4Addr, Ipv6Addr};

use windows::Win32::NetworkManagement::IpHelper::{
    GAA_FLAG_SKIP_ANYCAST, GAA_FLAG_SKIP_DNS_SERVER, GAA_FLAG_SKIP_MULTICAST,
    GetAdaptersAddresses, IP_ADAPTER_ADDRESSES_LH,
};
use windows::Win32::NetworkManagement::Ndis::IfOperStatusUp;
use windows::Win32::Networking::WinSock::{AF_INET, AF_INET6, SOCKADDR, SOCKADDR_IN, SOCKADDR_IN6};

const ERROR_SUCCESS: u32 = 0;
const ERROR_BUFFER_OVERFLOW: u32 = 111;
const ERROR_NO_DATA: u32 = 232;

/// Address family filter for `GetAdaptersAddresses`: both IPv4 and IPv6.
const AF_UNSPEC: u32 = 0;

const IF_TYPE_LOOPBACK: u32 = 24;

pub struct NetworkAdapter {
    /// User-facing connection name, e.g. "Ethernet" or "Wi-Fi".
    pub friendly_name: String,
    /// Adapter GUID; keys per-interface registry settings.
    pub guid: String,
    pub description: String,
    pub ip_addresses: Vec<String>,
    pub mac_address: String,
    pub adapter_type: &'static str,
    pub is_up: bool,
    pub is_loopback: bool,
}

/// Lists the system's network adapters via `GetAdaptersAddresses`.
/// Read-only: nothing is modified. Returns the Win32 error code on failure.
pub fn get_network_adapters() -> Result<Vec<NetworkAdapter>, u32> {
    let flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;

    // Microsoft recommends starting with a 15 KB buffer to avoid a second call.
    let mut size: u32 = 15_000;
    // u64 backing storage keeps the buffer aligned for IP_ADAPTER_ADDRESSES_LH.
    let mut buffer: Vec<u64> = Vec::new();

    // The required size can change between calls if adapters come and go.
    let mut result = ERROR_BUFFER_OVERFLOW;
    for _ in 0..4 {
        buffer = vec![0u64; (size as usize).div_ceil(8)];
        result = unsafe {
            GetAdaptersAddresses(
                AF_UNSPEC,
                flags,
                None,
                Some(buffer.as_mut_ptr().cast()),
                &mut size,
            )
        };
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
    let mut current = buffer.as_ptr().cast::<IP_ADAPTER_ADDRESSES_LH>();

    while !current.is_null() {
        let info = unsafe { &*current };

        let mut ip_addresses = Vec::new();
        let mut unicast = info.FirstUnicastAddress;
        while !unicast.is_null() {
            let entry = unsafe { &*unicast };
            if let Some(ip) = unsafe { sockaddr_to_string(entry.Address.lpSockaddr) } {
                ip_addresses.push(ip);
            }
            unicast = entry.Next;
        }

        let mac_len = (info.PhysicalAddressLength as usize).min(info.PhysicalAddress.len());
        let mac_address = info.PhysicalAddress[..mac_len]
            .iter()
            .map(|byte| format!("{byte:02X}"))
            .collect::<Vec<_>>()
            .join("-");

        adapters.push(NetworkAdapter {
            friendly_name: unsafe { wide_to_string(info.FriendlyName) },
            guid: unsafe { ansi_to_string(info.AdapterName) },
            description: unsafe { wide_to_string(info.Description) },
            ip_addresses,
            mac_address,
            adapter_type: adapter_type_name(info.IfType),
            is_up: info.OperStatus == IfOperStatusUp,
            is_loopback: info.IfType == IF_TYPE_LOOPBACK,
        });

        current = info.Next;
    }

    Ok(adapters)
}

/// Formats an IPv4 or IPv6 socket address; `None` for null or other families.
unsafe fn sockaddr_to_string(addr: *const SOCKADDR) -> Option<String> {
    if addr.is_null() {
        return None;
    }

    unsafe {
        match (*addr).sa_family {
            AF_INET => {
                let v4 = &*addr.cast::<SOCKADDR_IN>();
                // S_addr holds the address in network byte order.
                Some(Ipv4Addr::from(v4.sin_addr.S_un.S_addr.to_ne_bytes()).to_string())
            }
            AF_INET6 => {
                let v6 = &*addr.cast::<SOCKADDR_IN6>();
                Some(Ipv6Addr::from(v6.sin6_addr.u.Byte).to_string())
            }
            _ => None,
        }
    }
}

unsafe fn wide_to_string(text: windows::core::PWSTR) -> String {
    if text.is_null() {
        return String::new();
    }
    String::from_utf16_lossy(unsafe { text.as_wide() })
}

unsafe fn ansi_to_string(text: windows::core::PSTR) -> String {
    if text.is_null() {
        return String::new();
    }
    String::from_utf8_lossy(unsafe { text.as_bytes() }).into_owned()
}

/// Maps an IANA ifType value to a readable name.
fn adapter_type_name(if_type: u32) -> &'static str {
    match if_type {
        6 => "Ethernet",
        9 => "Token Ring",
        15 => "FDDI",
        23 => "PPP",
        24 => "Loopback",
        28 => "SLIP",
        53 => "Virtual",
        71 => "WiFi (IEEE 802.11)",
        131 => "Tunnel",
        243 | 244 => "Mobile Broadband",
        _ => "Other",
    }
}
