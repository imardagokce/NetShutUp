//! The list of every setting the program knows about.
//!
//! Profiles and backups refer to settings only by `id`, so ids must never
//! change once released.

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Scope {
    System,
    /// One value per network adapter; registry keys contain `ADAPTER_GUID`.
    Adapter,
}

/// Where a setting's current value is read from. Registry keys are relative
/// to `HKEY_LOCAL_MACHINE`.
pub enum Source {
    RegDword { key: &'static str, value: &'static str },
    RegString { key: &'static str, value: &'static str },
    Service { name: &'static str },
    /// Integer property of a WMI object in `ROOTStandardCimv2`.
    Wmi { class: &'static str, filter: Option<&'static str>, property: &'static str },
}

/// What a stock Windows installation has for a setting.
pub enum DefaultValue {
    /// Windows leaves the value unset; any explicit value is a change.
    Unset,
    Dword(u32),
    /// Service start type (`SERVICE_AUTO_START` = 2, etc.).
    ServiceStartType(u32),
}

pub struct Setting {
    pub id: &'static str,
    pub name: &'static str,
    pub scope: Scope,
    pub source: Source,
    /// `None` when the default is not verified; no default/changed label is
    /// shown for an explicit value in that case.
    pub default: Option<DefaultValue>,
    /// Readable names for specific DWORD values.
    pub labels: &'static [(u32, &'static str)],
}

/// Placeholder replaced with the adapter GUID in per-adapter registry keys.
pub const ADAPTER_GUID: &str = "{guid}";

const TCPIP_INTERFACE: &str =
    r"SYSTEM\CurrentControlSet\Services\Tcpip\Parameters\Interfaces\{guid}";
const SYSTEM_PROFILE: &str =
    r"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Multimedia\SystemProfile";

impl Setting {
    /// Whether the program can change this setting, not just display it.
    pub fn is_writable(&self) -> bool {
        !matches!(self.source, Source::Wmi { .. })
    }
}

/// Looks a setting up by its id.
pub fn find(id: &str) -> Option<&'static Setting> {
    CATALOG.iter().find(|setting| setting.id == id)
}

pub static CATALOG: &[Setting] = &[
    Setting {
        id: "tcp.nagle.ack_frequency",
        name: "Delayed ACK frequency",
        scope: Scope::Adapter,
        source: Source::RegDword { key: TCPIP_INTERFACE, value: "TcpAckFrequency" },
        default: Some(DefaultValue::Dword(2)),
        labels: &[(1, "every packet (no delay)")],
    },
    Setting {
        id: "tcp.nagle.no_delay",
        name: "Nagle: no delay",
        scope: Scope::Adapter,
        source: Source::RegDword { key: TCPIP_INTERFACE, value: "TCPNoDelay" },
        default: Some(DefaultValue::Dword(0)),
        labels: &[(0, "off"), (1, "on")],
    },
    Setting {
        id: "dns.static_servers",
        name: "Static DNS servers",
        scope: Scope::Adapter,
        source: Source::RegString { key: TCPIP_INTERFACE, value: "NameServer" },
        default: Some(DefaultValue::Unset),
        labels: &[],
    },
    Setting {
        id: "mm.network_throttling",
        name: "Network throttling",
        scope: Scope::System,
        source: Source::RegDword { key: SYSTEM_PROFILE, value: "NetworkThrottlingIndex" },
        default: Some(DefaultValue::Dword(10)),
        labels: &[(0xFFFF_FFFF, "disabled")],
    },
    Setting {
        id: "mm.system_responsiveness",
        name: "System responsiveness",
        scope: Scope::System,
        source: Source::RegDword { key: SYSTEM_PROFILE, value: "SystemResponsiveness" },
        default: Some(DefaultValue::Dword(20)),
        labels: &[],
    },
    Setting {
        id: "qos.reservable_bandwidth",
        name: "QoS reservable bandwidth",
        scope: Scope::System,
        source: Source::RegDword {
            key: r"SOFTWARE\Policies\Microsoft\Windows\Psched",
            value: "NonBestEffortLimit",
        },
        default: None,
        labels: &[],
    },
    Setting {
        id: "service.diagtrack",
        name: "Telemetry service",
        scope: Scope::System,
        source: Source::Service { name: "DiagTrack" },
        default: Some(DefaultValue::ServiceStartType(2)),
        labels: &[],
    },
    Setting {
        id: "privacy.telemetry_level",
        name: "Diagnostic data policy",
        scope: Scope::System,
        source: Source::RegDword {
            key: r"SOFTWARE\Policies\Microsoft\Windows\DataCollection",
            value: "AllowTelemetry",
        },
        default: Some(DefaultValue::Unset),
        // 0 turns diagnostic data off only on Enterprise and Education;
        // Windows treats it as 1 on Home and Pro.
        labels: &[(0, "lowest allowed"), (1, "required only"), (2, "enhanced"), (3, "optional")],
    },
    Setting {
        id: "dns.llmnr",
        name: "LLMNR name broadcast",
        scope: Scope::System,
        source: Source::RegDword {
            key: r"SOFTWARE\Policies\Microsoft\Windows NT\DNSClient",
            value: "EnableMulticast",
        },
        default: Some(DefaultValue::Unset),
        labels: ON_OFF,
    },
    Setting {
        id: "update.delivery_optimization",
        name: "Update sharing (P2P)",
        scope: Scope::System,
        source: Source::RegDword {
            key: r"SOFTWARE\Policies\Microsoft\Windows\DeliveryOptimization",
            value: "DODownloadMode",
        },
        default: Some(DefaultValue::Unset),
        labels: &[
            (0, "off (HTTP only)"),
            (1, "local network"),
            (2, "group"),
            (3, "internet"),
            (99, "off (no cloud)"),
            (100, "bypass"),
        ],
    },
    Setting {
        id: "tcp.autotuning",
        name: "TCP receive auto-tuning",
        scope: Scope::System,
        source: Source::Wmi {
            class: TCP_SETTING,
            filter: INTERNET_TEMPLATE,
            property: "AutoTuningLevelLocal",
        },
        default: Some(DefaultValue::Dword(3)),
        labels: &[
            (0, "disabled"),
            (1, "highly restricted"),
            (2, "restricted"),
            (3, "normal"),
            (4, "experimental"),
        ],
    },
    Setting {
        id: "tcp.ecn",
        name: "TCP ECN capability",
        scope: Scope::System,
        source: Source::Wmi {
            class: TCP_SETTING,
            filter: INTERNET_TEMPLATE,
            property: "EcnCapability",
        },
        default: None,
        labels: &[(0, "disabled"), (1, "enabled")],
    },
    Setting {
        id: "tcp.congestion_provider",
        name: "TCP congestion provider",
        scope: Scope::System,
        source: Source::Wmi {
            class: TCP_SETTING,
            filter: INTERNET_TEMPLATE,
            property: "CongestionProvider",
        },
        default: None,
        labels: &[(5, "CUBIC")],
    },
    Setting {
        id: "offload.rss",
        name: "Receive-side scaling",
        scope: Scope::System,
        source: Source::Wmi { class: OFFLOAD_SETTING, filter: None, property: "ReceiveSideScaling" },
        default: Some(DefaultValue::Dword(1)),
        labels: ON_OFF,
    },
    Setting {
        id: "offload.rsc",
        name: "Receive segment coalescing",
        scope: Scope::System,
        source: Source::Wmi {
            class: OFFLOAD_SETTING,
            filter: None,
            property: "ReceiveSegmentCoalescing",
        },
        default: Some(DefaultValue::Dword(1)),
        labels: ON_OFF,
    },
];

const TCP_SETTING: &str = "MSFT_NetTCPSetting";
/// The TCP template Windows applies to ordinary internet connections.
const INTERNET_TEMPLATE: Option<&str> = Some("SettingName = 'Internet'");
const OFFLOAD_SETTING: &str = "MSFT_NetOffloadGlobalSetting";
const ON_OFF: &[(u32, &str)] = &[(0, "disabled"), (1, "enabled")];

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashSet;

    #[test]
    fn ids_are_unique() {
        let mut seen = HashSet::new();
        for setting in CATALOG {
            assert!(seen.insert(setting.id), "duplicate id: {}", setting.id);
        }
    }

    #[test]
    fn adapter_scope_matches_guid_placeholder() {
        for setting in CATALOG {
            let key = match setting.source {
                Source::RegDword { key, .. } | Source::RegString { key, .. } => key,
                Source::Service { .. } | Source::Wmi { .. } => continue,
            };
            assert_eq!(
                key.contains(ADAPTER_GUID),
                setting.scope == Scope::Adapter,
                "scope/key mismatch: {}",
                setting.id
            );
        }
    }
}
