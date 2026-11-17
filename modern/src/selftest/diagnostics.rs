//! What the report says about the computer, limited to an allow-list: each
//! item answers a question that helps tell a NetShutUp bug from a problem
//! with the computer or its network. Nothing is collected outside this struct.

use serde::Serialize;

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

/// Every key the JSON of `Diagnostics` may contain.
pub const ALLOWED_KEYS: &[&str] = &[
    "program", "windows", "locale", "utc_offset_minutes", "cpu", "logical_cores", "ram_mb",
    "machine", "adapters", "virtual_adapters", "domain_joined", "policies", "security_products",
    "proxy_enabled", "known_doh_servers", "name", "display_version", "build", "revision",
    "edition", "arch", "manufacturer", "model", "description", "kind", "up", "link_mbps", "mtu",
    "dhcp", "ipv6", "driver", "provider", "version", "date", "count", "value",
];

#[cfg(test)]
mod tests {
    use super::*;
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

    #[test]
    fn diagnostics_json_has_only_allowlisted_keys() {
        let mut found = BTreeSet::new();
        keys(&serde_json::to_value(populated()).unwrap(), &mut found);
        let allowed: BTreeSet<String> = ALLOWED_KEYS.iter().map(|key| key.to_string()).collect();
        // Equal sets: nothing collected that is not listed, nothing listed that is not collected.
        assert_eq!(found, allowed);
    }
}
