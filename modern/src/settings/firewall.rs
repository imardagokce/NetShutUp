//! Windows Firewall base settings (on/off and default inbound action per
//! network type) through the `INetFwPolicy2` COM interface. Nothing here
//! parses command output.

use windows::Win32::Foundation::VARIANT_BOOL;
use windows::Win32::NetworkManagement::WindowsFirewall::{
    INetFwPolicy2, NET_FW_ACTION, NET_FW_ACTION_ALLOW, NET_FW_ACTION_BLOCK, NET_FW_PROFILE_TYPE2,
    NetFwPolicy2,
};
use windows::Win32::System::Com::{
    CLSCTX_INPROC_SERVER, COINIT_MULTITHREADED, CoCreateInstance, CoInitializeEx,
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Network {
    Domain,
    Private,
    Public,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Field {
    /// 1 when the firewall is on for the network type, 0 when off.
    Enabled,
    /// 0 blocks inbound connections by default, 1 allows them.
    Inbound,
}

/// `NET_FW_PROFILE2_DOMAIN` = 1, `PRIVATE` = 2, `PUBLIC` = 4.
fn network_code(network: Network) -> i32 {
    match network {
        Network::Domain => 1,
        Network::Private => 2,
        Network::Public => 4,
    }
}

#[cfg(test)]
fn field_name(field: Field) -> &'static str {
    match field {
        Field::Enabled => "enabled",
        Field::Inbound => "inbound",
    }
}

fn describe(error: windows::core::Error) -> String {
    let code = error.code().0 as u32;
    // E_ACCESSDENIED
    if code == 0x8007_0005 {
        return "access denied (administrator rights required)".to_string();
    }
    format!("firewall error {code:#010X}")
}

fn policy() -> Result<INetFwPolicy2, String> {
    unsafe {
        // Already-initialized results are fine; real failures surface below.
        let _ = CoInitializeEx(None, COINIT_MULTITHREADED);
        CoCreateInstance(&NetFwPolicy2, None, CLSCTX_INPROC_SERVER).map_err(describe)
    }
}

pub fn read(network: Network, field: Field) -> Result<u32, String> {
    let policy = policy()?;
    let profile = NET_FW_PROFILE_TYPE2(network_code(network));
    unsafe {
        match field {
            Field::Enabled => {
                let on = policy.get_FirewallEnabled(profile).map_err(describe)?;
                Ok(u32::from(on.as_bool()))
            }
            Field::Inbound => {
                let action = policy.get_DefaultInboundAction(profile).map_err(describe)?;
                Ok(u32::from(action == NET_FW_ACTION_ALLOW))
            }
        }
    }
}

pub fn write(network: Network, field: Field, value: u32) -> Result<(), String> {
    if value > 1 {
        return Err("value type does not match the setting".to_string());
    }
    let policy = policy()?;
    let profile = NET_FW_PROFILE_TYPE2(network_code(network));
    unsafe {
        match field {
            Field::Enabled => policy
                .put_FirewallEnabled(profile, VARIANT_BOOL::from(value == 1))
                .map_err(describe),
            Field::Inbound => {
                let action: NET_FW_ACTION =
                    if value == 1 { NET_FW_ACTION_ALLOW } else { NET_FW_ACTION_BLOCK };
                policy.put_DefaultInboundAction(profile, action).map_err(describe)
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn helpers_name_networks_and_fields() {
        assert_eq!(network_code(Network::Domain), 1);
        assert_eq!(network_code(Network::Private), 2);
        assert_eq!(network_code(Network::Public), 4);
        assert_eq!(field_name(Field::Enabled), "enabled");
        assert_eq!(field_name(Field::Inbound), "inbound");
    }

    #[test]
    fn writing_a_value_above_one_is_rejected_without_touching_windows() {
        let error = write(Network::Public, Field::Enabled, 2).unwrap_err();
        assert_eq!(error, "value type does not match the setting");
    }
}
