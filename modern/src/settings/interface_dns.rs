//! A network adapter's IPv4 DNS servers and their encrypted-DNS (DoH)
//! settings, through the Windows IP Helper API (`Get/SetInterfaceDnsSettings`).
//! Unlike a registry write, this tells the Windows DNS client about the change.

use windows::Win32::NetworkManagement::IpHelper::{
    DNS_DOH_SERVER_SETTINGS, DNS_INTERFACE_SETTINGS_VERSION3, DNS_INTERFACE_SETTINGS3,
    DNS_SERVER_PROPERTY, DNS_SERVER_PROPERTY_TYPES, DNS_SERVER_PROPERTY_VERSION1,
    DNS_SETTING_DOH, DNS_SETTING_NAMESERVER, DnsServerDohProperty, FreeInterfaceDnsSettings,
    GetInterfaceDnsSettings, SetInterfaceDnsSettings,
};
use windows::core::{GUID, PWSTR};

use super::reader::{DnsServer, Doh, State, Value};

const ERROR_SUCCESS: u32 = 0;
const ERROR_ACCESS_DENIED: u32 = 5;

/// Parses an adapter GUID with or without braces, in any case.
pub fn parse_guid(text: &str) -> Option<GUID> {
    GUID::try_from(text.trim_matches(['{', '}'])).ok()
}

/// Windows stores server lists separated by commas or spaces.
fn split_name_servers(raw: &str) -> Vec<String> {
    raw.split([',', ' ']).filter(|part| !part.is_empty()).map(str::to_string).collect()
}

fn error_text(code: u32) -> String {
    match code {
        ERROR_ACCESS_DENIED => "access denied (administrator rights required)".to_string(),
        code => format!("DNS settings error {code}"),
    }
}

/// The IPv4 servers of an adapter and their DoH settings; `NotSet` when it
/// has no servers of its own (DNS comes from DHCP).
pub fn read(adapter_guid: &str) -> Result<State, String> {
    let guid =
        parse_guid(adapter_guid).ok_or_else(|| format!("invalid adapter id {adapter_guid}"))?;
    let mut settings = DNS_INTERFACE_SETTINGS3 {
        Version: DNS_INTERFACE_SETTINGS_VERSION3,
        Flags: u64::from(DNS_SETTING_NAMESERVER | DNS_SETTING_DOH),
        ..Default::default()
    };

    let code = unsafe { GetInterfaceDnsSettings(guid, (&raw mut settings).cast()) };
    if code.0 != ERROR_SUCCESS {
        return Err(error_text(code.0));
    }
    let state = unsafe { to_state(&settings) };
    unsafe { FreeInterfaceDnsSettings((&raw mut settings).cast()) };
    Ok(state)
}

/// Sets the IPv4 servers of an adapter and their DoH settings. `NotSet`
/// removes them, so the adapter takes DNS from DHCP again.
pub fn write(adapter_guid: &str, state: &State) -> Result<(), String> {
    let guid =
        parse_guid(adapter_guid).ok_or_else(|| format!("invalid adapter id {adapter_guid}"))?;
    let servers: &[DnsServer] = match state {
        State::NotSet => &[],
        State::Value(Value::Dns(servers)) => servers,
        _ => return Err("value type does not match the setting".to_string()),
    };

    // These buffers must stay alive until the call returns.
    let addresses: Vec<&str> = servers.iter().map(|server| server.address.as_str()).collect();
    let mut name_server = wide(&addresses.join(","));
    let with_doh: Vec<(usize, &Doh)> = servers
        .iter()
        .enumerate()
        .filter_map(|(index, server)| server.doh.as_ref().map(|doh| (index, doh)))
        .collect();
    let mut templates: Vec<Vec<u16>> =
        with_doh.iter().map(|(_, doh)| template_buffer(&doh.template)).collect();
    let mut doh_settings: Vec<DNS_DOH_SERVER_SETTINGS> = templates
        .iter_mut()
        .zip(&with_doh)
        .map(|(template, (_, doh))| DNS_DOH_SERVER_SETTINGS {
            Template: pointer_to(template),
            Flags: doh.flags,
        })
        .collect();
    let mut properties: Vec<DNS_SERVER_PROPERTY> = doh_settings
        .iter_mut()
        .zip(&with_doh)
        .map(|(doh_setting, (index, _))| DNS_SERVER_PROPERTY {
            Version: DNS_SERVER_PROPERTY_VERSION1,
            ServerIndex: *index as u32,
            Type: DnsServerDohProperty,
            Property: DNS_SERVER_PROPERTY_TYPES { DohSettings: doh_setting },
        })
        .collect();

    let settings = DNS_INTERFACE_SETTINGS3 {
        Version: DNS_INTERFACE_SETTINGS_VERSION3,
        Flags: u64::from(DNS_SETTING_NAMESERVER | DNS_SETTING_DOH),
        NameServer: PWSTR(name_server.as_mut_ptr()),
        cServerProperties: properties.len() as u32,
        ServerProperties: if properties.is_empty() {
            std::ptr::null_mut()
        } else {
            properties.as_mut_ptr()
        },
        ..Default::default()
    };

    let code = unsafe { SetInterfaceDnsSettings(guid, (&raw const settings).cast()) };
    if code.0 == ERROR_SUCCESS { Ok(()) } else { Err(error_text(code.0)) }
}

/// A DoH template as UTF-16. An empty template stays empty: it is read from
/// and written back to Windows as NULL, not as an empty string.
fn template_buffer(template: &str) -> Vec<u16> {
    if template.is_empty() { Vec::new() } else { wide(template) }
}

fn pointer_to(buffer: &mut Vec<u16>) -> PWSTR {
    if buffer.is_empty() { PWSTR::null() } else { PWSTR(buffer.as_mut_ptr()) }
}

/// NUL-terminated UTF-16 copy of `text`.
fn wide(text: &str) -> Vec<u16> {
    text.encode_utf16().chain(Some(0)).collect()
}

unsafe fn wide_to_string(text: PWSTR) -> String {
    if text.is_null() {
        return String::new();
    }
    unsafe { text.to_string() }.unwrap_or_default()
}

unsafe fn to_state(settings: &DNS_INTERFACE_SETTINGS3) -> State {
    let addresses = split_name_servers(&unsafe { wide_to_string(settings.NameServer) });
    if addresses.is_empty() {
        return State::NotSet;
    }

    let properties: &[DNS_SERVER_PROPERTY] = if settings.ServerProperties.is_null() {
        &[]
    } else {
        unsafe {
            std::slice::from_raw_parts(settings.ServerProperties, settings.cServerProperties as usize)
        }
    };

    let servers = addresses
        .into_iter()
        .enumerate()
        .map(|(index, address)| {
            let doh = properties
                .iter()
                .find(|property| {
                    property.Type == DnsServerDohProperty && property.ServerIndex as usize == index
                })
                .and_then(|property| {
                    let doh = unsafe { property.Property.DohSettings };
                    (!doh.is_null()).then(|| unsafe {
                        Doh { template: wide_to_string((*doh).Template), flags: (*doh).Flags }
                    })
                });
            DnsServer { address, doh }
        })
        .collect();
    State::Value(Value::Dns(servers))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::adapters;

    #[test]
    fn parse_guid_accepts_braces_and_any_case() {
        assert!(parse_guid("{D7747AC7-885F-4A75-8F0C-84BA153614EC}").is_some());
        assert!(parse_guid("d7747ac7-885f-4a75-8f0c-84ba153614ec").is_some());
        assert_eq!(
            parse_guid("{D7747AC7-885F-4A75-8F0C-84BA153614EC}"),
            parse_guid("d7747ac7-885f-4a75-8f0c-84ba153614ec")
        );
        assert!(parse_guid("nonsense").is_none());
    }

    #[test]
    fn empty_template_is_written_as_null() {
        let mut empty = template_buffer("");
        assert!(pointer_to(&mut empty).is_null());
        let mut filled = template_buffer("https://dns.google/dns-query");
        assert!(!pointer_to(&mut filled).is_null());
        assert_eq!(filled.last(), Some(&0), "NUL-terminated");
    }

    #[test]
    fn split_name_servers_handles_commas_and_spaces() {
        assert_eq!(split_name_servers("1.1.1.1, 1.0.0.1 8.8.8.8"), ["1.1.1.1", "1.0.0.1", "8.8.8.8"]);
        assert!(split_name_servers("").is_empty());
    }

    /// Read-only: runs against the real adapters of the machine running the tests.
    #[test]
    fn every_adapter_dns_can_be_read() {
        for adapter in adapters::get_network_adapters().unwrap() {
            if !adapter.takes_profile_settings {
                continue;
            }
            let state = read(&adapter.guid);
            assert!(state.is_ok(), "{}: {state:?}", adapter.friendly_name);
            println!("{}: {:?}", adapter.friendly_name, state);
        }
    }
}
