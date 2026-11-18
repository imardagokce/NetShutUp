//! What the report says about the computer, limited to an allow-list: each
//! item answers a question that helps tell a NetShutUp bug from a problem
//! with the computer or its network. Nothing is collected outside this struct.

use serde::Serialize;
use windows::Win32::Globalization::GetUserDefaultLocaleName;
use windows::Win32::System::Registry::{HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
use windows::Win32::System::SystemInformation::{GlobalMemoryStatusEx, MEMORYSTATUSEX};
use windows::Win32::System::Time::{GetTimeZoneInformation, TIME_ZONE_INFORMATION};

use crate::adapters::NetworkAdapter;
use crate::settings::{registry, wmi};
use crate::version;

#[derive(Debug, Clone, Default, Serialize)]
pub struct Diagnostics {
    pub program: String,
    pub windows: WindowsInfo,
    pub locale: Option<String>,
    pub utc_offset_minutes: Option<i32>,
    pub cpu: Option<String>,
    pub logical_cores: Option<u32>,
    pub ram_mb: Option<u64>,
    pub machine: Option<MachineInfo>,
    pub adapters: Vec<AdapterInfo>,
    pub virtual_adapters: Vec<VirtualAdapters>,
    pub domain_joined: Option<bool>,
    pub policies: Vec<PolicyValue>,
    pub security_products: Vec<String>,
    pub proxy_enabled: Option<bool>,
    pub known_doh_servers: Vec<String>,
}

#[derive(Debug, Clone, Default, Serialize)]
pub struct WindowsInfo {
    pub name: String,
    pub display_version: Option<String>,
    pub build: u32,
    pub revision: Option<u32>,
    pub edition: Option<String>,
    pub arch: String,
}

/// Make and model only; never a serial number.
#[derive(Debug, Clone, Serialize)]
pub struct MachineInfo {
    pub manufacturer: String,
    pub model: String,
}

#[derive(Debug, Clone, Serialize)]
pub struct AdapterInfo {
    pub name: String,
    pub description: String,
    pub kind: String,
    pub up: bool,
    pub link_mbps: Option<u64>,
    pub mtu: Option<u32>,
    pub dhcp: Option<bool>,
    pub ipv6: bool,
    pub driver: Option<DriverInfo>,
}

#[derive(Debug, Clone, Serialize)]
pub struct DriverInfo {
    pub provider: String,
    pub version: String,
    pub date: String,
}

/// How many virtual adapters (VPN, tunnels, virtual machines) of a kind exist.
/// Their descriptions are never copied: they can name an employer.
#[derive(Debug, Clone, Serialize)]
pub struct VirtualAdapters {
    pub kind: String,
    pub count: u32,
}

#[derive(Debug, Clone, Serialize)]
pub struct PolicyValue {
    pub name: String,
    pub value: String,
}

/// Every key the JSON of `Diagnostics` may contain. A test holds the struct to it.
#[cfg(test)]
pub const ALLOWED_KEYS: &[&str] = &[
    "program", "windows", "locale", "utc_offset_minutes", "cpu", "logical_cores", "ram_mb",
    "machine", "adapters", "virtual_adapters", "domain_joined", "policies", "security_products",
    "proxy_enabled", "known_doh_servers", "name", "display_version", "build", "revision",
    "edition", "arch", "manufacturer", "model", "description", "kind", "up", "link_mbps", "mtu",
    "dhcp", "ipv6", "driver", "provider", "version", "date", "count", "value",
];


const WINDOWS_VERSION_KEY: &str = r"SOFTWARE\Microsoft\Windows NT\CurrentVersion";
const BIOS_KEY: &str = r"HARDWARE\DESCRIPTION\System\BIOS";
const CPU_KEY: &str = r"HARDWARE\DESCRIPTION\System\CentralProcessor\0";
const TCPIP_KEY: &str = r"SYSTEM\CurrentControlSet\Services\Tcpip\Parameters";
const INTERNET_SETTINGS_KEY: &str = r"Software\Microsoft\Windows\CurrentVersion\Internet Settings";

/// Policy values worth knowing because they can override what NetShutUp sets:
/// (key under HKLM, value name).
const POLICIES: &[(&str, &str)] = &[
    (r"SOFTWARE\Policies\Microsoft\Windows NT\DNSClient", "DoHPolicy"),
    (r"SOFTWARE\Policies\Microsoft\Windows NT\DNSClient", "EnableMulticast"),
    (r"SOFTWARE\Policies\Microsoft\Windows\DataCollection", "AllowTelemetry"),
    (r"SOFTWARE\Policies\Microsoft\Windows\DeliveryOptimization", "DODownloadMode"),
];

const TIME_ZONE_ID_DAYLIGHT: u32 = 2;

/// Everything about the system itself (not its adapters). Read-only; a source
/// that cannot be read leaves its field empty, never an error.
pub fn collect_system() -> Diagnostics {
    let text = |key: &str, value: &str| {
        registry::read_string(HKEY_LOCAL_MACHINE, key, value)
            .ok()
            .flatten()
            .map(|found| found.trim().to_string())
            .filter(|found| !found.is_empty())
    };
    let (major, minor, build) = version::get_windows_version();

    let product = text(WINDOWS_VERSION_KEY, "ProductName").unwrap_or_default();
    let edition_id = text(WINDOWS_VERSION_KEY, "EditionID").unwrap_or_default();
    let manufacturer = text(BIOS_KEY, "SystemManufacturer");
    let model = text(BIOS_KEY, "SystemProductName");

    let read_policy = |key: &str, value: &str| -> Option<String> {
        match registry::read_dword(HKEY_LOCAL_MACHINE, key, value) {
            Ok(Some(number)) => Some(number.to_string()),
            _ => registry::read_string(HKEY_LOCAL_MACHINE, key, value).ok().flatten(),
        }
    };

    Diagnostics {
        program: env!("CARGO_PKG_VERSION").to_string(),
        windows: WindowsInfo {
            name: windows_name(major, minor, build),
            display_version: text(WINDOWS_VERSION_KEY, "DisplayVersion"),
            build,
            revision: registry::read_dword(HKEY_LOCAL_MACHINE, WINDOWS_VERSION_KEY, "UBR")
                .ok()
                .flatten(),
            edition: Some(edition_label(&product, &edition_id)).filter(|label| !label.is_empty()),
            arch: std::env::consts::ARCH.to_string(),
        },
        locale: locale_name(),
        utc_offset_minutes: utc_offset_minutes(),
        cpu: text(CPU_KEY, "ProcessorNameString"),
        logical_cores: std::thread::available_parallelism().ok().map(|n| n.get() as u32),
        ram_mb: total_ram_mb(),
        machine: match (manufacturer, model) {
            (None, None) => None,
            (manufacturer, model) => Some(MachineInfo {
                manufacturer: manufacturer.unwrap_or_default(),
                model: model.unwrap_or_default(),
            }),
        },
        domain_joined: Some(text(TCPIP_KEY, "Domain").is_some()),
        policies: policy_rows(&read_policy),
        proxy_enabled: match registry::read_dword(HKEY_CURRENT_USER, INTERNET_SETTINGS_KEY, "ProxyEnable") {
            Ok(Some(number)) => Some(number != 0),
            Ok(None) => Some(false),
            Err(_) => None,
        },
        ..Diagnostics::default()
    }
}

/// The Windows 11 registry still says "Windows 10" in `ProductName`, so the
/// name comes from the build number.
fn windows_name(major: u32, minor: u32, build: u32) -> String {
    match (major, build) {
        (10, 22000..) => "Windows 11".to_string(),
        (10, _) => "Windows 10".to_string(),
        _ => format!("Windows {major}.{minor}"),
    }
}

fn edition_label(product: &str, edition_id: &str) -> String {
    if edition_id.is_empty() { product.to_string() } else { edition_id.to_string() }
}

/// The policy values that exist, as name and value.
fn policy_rows(read: &dyn Fn(&str, &str) -> Option<String>) -> Vec<PolicyValue> {
    POLICIES
        .iter()
        .filter_map(|(key, name)| {
            read(key, name).map(|value| PolicyValue { name: (*name).to_string(), value })
        })
        .collect()
}

fn locale_name() -> Option<String> {
    let mut buffer = [0u16; 85];
    let length = unsafe { GetUserDefaultLocaleName(&mut buffer) };
    // The count includes the terminating NUL.
    (length > 1).then(|| String::from_utf16_lossy(&buffer[..length as usize - 1]))
}

/// Offset from UTC in minutes (positive east of Greenwich), daylight saving included.
fn utc_offset_minutes() -> Option<i32> {
    let mut zone = TIME_ZONE_INFORMATION::default();
    let id = unsafe { GetTimeZoneInformation(&mut zone) };
    // u32::MAX is TIME_ZONE_ID_INVALID.
    if id == u32::MAX {
        return None;
    }
    let daylight = if id == TIME_ZONE_ID_DAYLIGHT { zone.DaylightBias } else { 0 };
    Some(-(zone.Bias + daylight))
}

fn total_ram_mb() -> Option<u64> {
    let mut status = MEMORYSTATUSEX {
        dwLength: std::mem::size_of::<MEMORYSTATUSEX>() as u32,
        ..Default::default()
    };
    unsafe { GlobalMemoryStatusEx(&mut status) }.ok()?;
    Some(status.ullTotalPhys / (1024 * 1024))
}


const NET_CLASS_KEY: &str =
    r"SYSTEM\CurrentControlSet\Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}";
const DOH_SERVERS_KEY: &str = r"SYSTEM\CurrentControlSet\Services\Dnscache\Parameters\DohWellKnownServers";
const SECURITY_CENTER: &str = r"ROOT\SecurityCenter2";

/// The registered firewall that is not Windows' own, if any. An unreadable
/// list counts as none, so Windows Firewall is assumed.
pub fn registered_third_party_firewall() -> Option<String> {
    let products =
        wmi::query_strings(SECURITY_CENTER, "FirewallProduct", "displayName").unwrap_or_default();
    crate::settings::plan::third_party_firewall(&products)
}

/// The adapters, virtual adapters, registered security products and the DoH
/// servers Windows knows. Read-only; anything unavailable is left out.
///
/// An adapter's own name is not copied (a person may have renamed it): it is
/// reported as its kind and a number.
pub fn collect_network(
    adapters: &[NetworkAdapter],
) -> (Vec<AdapterInfo>, Vec<VirtualAdapters>, Vec<String>, Vec<String>) {
    let classes = driver_classes();
    let infos = adapters
        .iter()
        .filter(|adapter| adapter.is_hardware && !adapter.is_loopback)
        .enumerate()
        .map(|(index, adapter)| AdapterInfo {
            name: format!("{} {}", adapter.adapter_type, index + 1),
            description: adapter.description.clone(),
            kind: adapter.adapter_type.to_string(),
            up: adapter.is_up,
            link_mbps: adapter.link_mbps,
            mtu: Some(adapter.mtu),
            dhcp: Some(adapter.dhcp4),
            ipv6: adapter.ipv6_enabled,
            driver: pick_driver(&adapter.guid, &classes),
        })
        .collect();

    let mut products: Vec<String> = ["AntiVirusProduct", "FirewallProduct"]
        .iter()
        .flat_map(|class| wmi::query_strings(SECURITY_CENTER, class, "displayName").unwrap_or_default())
        .collect();
    products.sort();
    products.dedup();

    let doh = registry::subkeys(HKEY_LOCAL_MACHINE, DOH_SERVERS_KEY).unwrap_or_default();
    (infos, summarize_virtual(adapters), products, mask_servers(doh))
}

/// Server addresses as they may appear in a report: only the known providers stay readable.
fn mask_servers(servers: Vec<String>) -> Vec<String> {
    servers.into_iter().map(|server| super::mask::mask_ip(&server)).collect()
}

/// Virtual adapters grouped by kind. Descriptions and names are never copied:
/// they can name an employer or a person.
fn summarize_virtual(adapters: &[NetworkAdapter]) -> Vec<VirtualAdapters> {
    let mut counts: std::collections::BTreeMap<String, u32> = std::collections::BTreeMap::new();
    for adapter in adapters.iter().filter(|a| !a.is_hardware && !a.is_loopback) {
        *counts.entry(adapter.adapter_type.to_string()).or_default() += 1;
    }
    counts.into_iter().map(|(kind, count)| VirtualAdapters { kind, count }).collect()
}

fn same_guid(a: &str, b: &str) -> bool {
    a.trim_matches(['{', '}']).eq_ignore_ascii_case(b.trim_matches(['{', '}']))
}

/// The driver recorded for the adapter with this GUID.
fn pick_driver(guid: &str, classes: &[(String, DriverInfo)]) -> Option<DriverInfo> {
    classes.iter().find(|(id, _)| same_guid(id, guid)).map(|(_, driver)| driver.clone())
}

/// (adapter GUID, driver) for every network device class entry.
fn driver_classes() -> Vec<(String, DriverInfo)> {
    let text = |key: &str, value: &str| {
        registry::read_string(HKEY_LOCAL_MACHINE, key, value).ok().flatten()
    };
    registry::subkeys(HKEY_LOCAL_MACHINE, NET_CLASS_KEY)
        .unwrap_or_default()
        .into_iter()
        .filter_map(|sub| {
            let key = format!(r"{NET_CLASS_KEY}\{sub}");
            let id = text(&key, "NetCfgInstanceId")?;
            Some((
                id,
                DriverInfo {
                    provider: text(&key, "ProviderName").unwrap_or_default(),
                    version: text(&key, "DriverVersion").unwrap_or_default(),
                    date: text(&key, "DriverDate").unwrap_or_default(),
                },
            ))
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::adapters::NetworkAdapter;
    use std::collections::BTreeSet;

    fn populated() -> Diagnostics {
        Diagnostics {
            program: "0.1.0-alpha.1".into(),
            windows: WindowsInfo {
                name: "Windows 11".into(),
                display_version: Some("24H2".into()),
                build: 26200,
                revision: Some(1000),
                edition: Some("Core".into()),
                arch: "x86_64".into(),
            },
            locale: Some("tr-TR".into()),
            utc_offset_minutes: Some(180),
            cpu: Some("Test CPU".into()),
            logical_cores: Some(8),
            ram_mb: Some(16000),
            machine: Some(MachineInfo { manufacturer: "Acme".into(), model: "Laptop".into() }),
            adapters: vec![AdapterInfo {
                name: "Wi-Fi".into(),
                description: "Test Wi-Fi".into(),
                kind: "WiFi".into(),
                up: true,
                link_mbps: Some(866),
                mtu: Some(1500),
                dhcp: Some(true),
                ipv6: true,
                driver: Some(DriverInfo {
                    provider: "Acme".into(),
                    version: "1.2.3".into(),
                    date: "2024-01-01".into(),
                }),
            }],
            virtual_adapters: vec![VirtualAdapters { kind: "Tunnel".into(), count: 2 }],
            domain_joined: Some(false),
            policies: vec![PolicyValue { name: "AllowTelemetry".into(), value: "0".into() }],
            security_products: vec!["Windows Defender".into()],
            proxy_enabled: Some(false),
            known_doh_servers: vec!["1.1.1.1".into()],
        }
    }

    fn keys(value: &serde_json::Value, into: &mut BTreeSet<String>) {
        match value {
            serde_json::Value::Object(map) => {
                for (key, inner) in map {
                    into.insert(key.clone());
                    keys(inner, into);
                }
            }
            serde_json::Value::Array(items) => items.iter().for_each(|item| keys(item, into)),
            _ => {}
        }
    }

    fn adapter(kind: &'static str, hardware: bool, loopback: bool) -> NetworkAdapter {
        NetworkAdapter {
            friendly_name: "Secret VPN name".into(),
            guid: "{X}".into(),
            description: "Corp VPN Client".into(),
            ip_addresses: vec![],
            mac_address: "-".into(),
            adapter_type: kind,
            is_up: true,
            is_loopback: loopback,
            takes_profile_settings: hardware,
            link_mbps: None,
            mtu: 1500,
            dhcp4: true,
            ipv6_enabled: true,
            is_hardware: hardware,
        }
    }

    #[test]
    fn summarize_virtual_counts_kinds_and_never_copies_descriptions() {
        let adapters = [
            adapter("Tunnel", false, false),
            adapter("Tunnel", false, false),
            adapter("Virtual", false, false),
            adapter("Ethernet", true, false),
            adapter("Loopback", false, true),
        ];
        let summary = summarize_virtual(&adapters);
        let pairs: Vec<(&str, u32)> = summary.iter().map(|v| (v.kind.as_str(), v.count)).collect();
        assert_eq!(pairs, [("Tunnel", 2), ("Virtual", 1)]);
        let text = format!("{summary:?}");
        assert!(!text.contains("Corp VPN") && !text.contains("Secret"), "{text}");
    }

    #[test]
    fn pick_driver_matches_the_guid_ignoring_case_and_braces() {
        let driver = |provider: &str| DriverInfo {
            provider: provider.into(),
            version: "1.0".into(),
            date: "2024-01-01".into(),
        };
        let classes = vec![
            ("{aaaaaaaa-885f-4a75-8f0c-84ba153614ec}".to_string(), driver("Other")),
            ("{d7747ac7-885f-4a75-8f0c-84ba153614ec}".to_string(), driver("Realtek")),
        ];
        let found = pick_driver("D7747AC7-885F-4A75-8F0C-84BA153614EC", &classes).unwrap();
        assert_eq!(found.provider, "Realtek");
        assert!(pick_driver("{00000000-0000-0000-0000-000000000000}", &classes).is_none());
    }

    /// Read-only: runs against the machine running the tests.
    #[test]
    fn collect_network_reads_this_machine_without_errors() {
        let adapters = crate::adapters::get_network_adapters().unwrap();
        let (infos, virtuals, security, doh) = collect_network(&adapters);
        println!("adapters: {infos:#?}
virtual: {virtuals:?}
security: {security:?}
doh: {doh:?}");
        let hardware = adapters.iter().filter(|a| a.is_hardware && !a.is_loopback).count();
        assert_eq!(infos.len(), hardware);
        assert!(infos.iter().all(|info| !info.description.is_empty()));
    }

    #[test]
    fn known_doh_servers_are_masked_at_collection() {
        let servers = vec!["192.168.1.2".to_string(), "1.1.1.1".to_string()];
        assert_eq!(mask_servers(servers), ["192.168.x.x", "1.1.1.1"]);
    }

    #[test]
    fn edition_label_prefers_the_edition_id_and_falls_back_to_the_product_name() {
        assert_eq!(edition_label("Windows 11 Home Single Language", "CoreSingleLanguage"), "CoreSingleLanguage");
        assert_eq!(edition_label("Windows 10 Pro", ""), "Windows 10 Pro");
    }

    #[test]
    fn policy_rows_lists_only_values_that_exist() {
        let read = |key: &str, value: &str| {
            (key.ends_with("DataCollection") && value == "AllowTelemetry").then(|| "0".to_string())
        };
        let rows = policy_rows(&read);
        assert_eq!(rows.len(), 1);
        assert_eq!((rows[0].name.as_str(), rows[0].value.as_str()), ("AllowTelemetry", "0"));
    }

    #[test]
    fn windows_name_follows_the_build_not_the_registry_product_name() {
        assert_eq!(windows_name(10, 0, 26200), "Windows 11");
        assert_eq!(windows_name(10, 0, 19045), "Windows 10");
        assert_eq!(windows_name(6, 3, 9600), "Windows 6.3");
    }

    /// Read-only: runs against the machine running the tests.
    #[test]
    fn collect_system_fills_the_basics_on_this_machine() {
        let found = collect_system();
        assert_eq!(found.program, env!("CARGO_PKG_VERSION"));
        assert!(found.windows.build > 0);
        assert!(!found.windows.arch.is_empty());
        assert!(found.logical_cores.unwrap_or(0) > 0);
        assert!(found.ram_mb.unwrap_or(0) > 0);
    }

    #[test]
    fn diagnostics_json_has_only_allowlisted_keys() {
        let mut found = BTreeSet::new();
        keys(&serde_json::to_value(populated()).unwrap(), &mut found);
        let allowed: BTreeSet<String> = ALLOWED_KEYS.iter().map(|key| key.to_string()).collect();
        // Equal sets: nothing collected that is not listed, nothing listed that is not collected.
        assert_eq!(found, allowed);
    }
}
