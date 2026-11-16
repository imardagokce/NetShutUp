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
    // Read by profiles and backups from Milestone 4 on.
    #[allow(dead_code)]
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

pub static CATALOG: &[Setting] = &[
    Setting {
        id: "tcp.nagle.ack_frequency",
        name: "Nagle: ACK frequency",
        scope: Scope::Adapter,
        source: Source::RegDword { key: TCPIP_INTERFACE, value: "TcpAckFrequency" },
        default: Some(DefaultValue::Dword(2)),
        labels: &[],
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
];

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
                Source::Service { .. } => continue,
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
