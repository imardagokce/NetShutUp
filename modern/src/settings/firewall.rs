//! Windows Firewall base settings (on/off and default inbound action per
//! network type) through the `INetFwPolicy2` COM interface. Nothing here
//! parses command output.

use windows::Win32::Foundation::VARIANT_BOOL;
use windows::Win32::NetworkManagement::WindowsFirewall::{
    INetFwPolicy2, INetFwRule, INetFwRules, NET_FW_ACTION, NET_FW_ACTION_ALLOW,
    NET_FW_ACTION_BLOCK, NET_FW_PROFILE_TYPE2, NET_FW_RULE_DIR_OUT, NetFwPolicy2, NetFwRule,
};
use windows::Win32::System::Com::{
    CLSCTX_INPROC_SERVER, COINIT_MULTITHREADED, CoCreateInstance, CoInitializeEx,
};
use windows::core::BSTR;

/// The program's own outbound block rule for the telemetry service.
const RULE_NAME: &str = "NetShutUp: block telemetry service";
const RULE_GROUP: &str = "NetShutUp";
const RULE_SERVICE: &str = "DiagTrack";
/// Every profile (domain, private and public).
const ALL_PROFILES: i32 = 0x7FFF_FFFF;
/// `HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)`: `Rules.Item` on a missing rule.
const RULE_NOT_FOUND: u32 = 0x8007_0002;

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
    /// 1 when the program's telemetry block rule exists. Not tied to a
    /// network type; the setting's `network` is ignored.
    Rule,
    /// 1 when the rule group (its resource name, such as
    /// `@FirewallAPI.dll,-32752`) is enabled for the network type.
    Group(&'static str),
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
        Field::Rule => "rule",
        Field::Group(group) => group,
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

/// Whether the telemetry block rule exists. A missing rule is `Ok(false)`;
/// any other failure is an error.
fn rule_exists(policy: &INetFwPolicy2) -> Result<bool, String> {
    let rules = unsafe { policy.Rules() }.map_err(describe)?;
    match unsafe { rules.Item(&BSTR::from(RULE_NAME)) } {
        Ok(_) => Ok(true),
        Err(error) if error.code().0 as u32 == RULE_NOT_FOUND => Ok(false),
        Err(error) => Err(describe(error)),
    }
}

/// Creates the telemetry block rule: outbound, blocked, for `DiagTrack`, on
/// all profiles, enabled.
fn add_rule(rules: &INetFwRules) -> Result<(), String> {
    unsafe {
        let rule: INetFwRule =
            CoCreateInstance(&NetFwRule, None, CLSCTX_INPROC_SERVER).map_err(describe)?;
        rule.SetName(&BSTR::from(RULE_NAME)).map_err(describe)?;
        rule.SetGrouping(&BSTR::from(RULE_GROUP)).map_err(describe)?;
        rule.SetServiceName(&BSTR::from(RULE_SERVICE)).map_err(describe)?;
        rule.SetDirection(NET_FW_RULE_DIR_OUT).map_err(describe)?;
        rule.SetAction(NET_FW_ACTION_BLOCK).map_err(describe)?;
        rule.SetProfiles(ALL_PROFILES).map_err(describe)?;
        rule.SetEnabled(VARIANT_BOOL::from(true)).map_err(describe)?;
        rules.Add(&rule).map_err(describe)
    }
}

/// Adds or removes the telemetry block rule. Already in the wanted state is a
/// success that changes nothing.
fn set_rule(policy: &INetFwPolicy2, on: bool) -> Result<(), String> {
    if rule_exists(policy)? == on {
        return Ok(());
    }
    let rules = unsafe { policy.Rules() }.map_err(describe)?;
    if on {
        add_rule(&rules)
    } else {
        unsafe { rules.Remove(&BSTR::from(RULE_NAME)) }.map_err(describe)
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
            Field::Rule => Ok(u32::from(rule_exists(&policy)?)),
            Field::Group(group) => {
                let on = policy
                    .IsRuleGroupEnabled(network_code(network), &BSTR::from(group))
                    .map_err(describe)?;
                Ok(u32::from(on.as_bool()))
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
            Field::Rule => set_rule(&policy, value == 1),
            Field::Group(group) => policy
                .EnableRuleGroup(network_code(network), &BSTR::from(group), VARIANT_BOOL::from(value == 1))
                .map_err(describe),
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
        assert_eq!(field_name(Field::Rule), "rule");
        assert_eq!(field_name(Field::Group("@FirewallAPI.dll,-32752")), "@FirewallAPI.dll,-32752");
    }

    #[test]
    fn telemetry_rule_has_the_designed_identity() {
        assert_eq!(RULE_NAME, "NetShutUp: block telemetry service");
        assert_eq!(RULE_GROUP, "NetShutUp");
        assert_eq!(RULE_SERVICE, "DiagTrack");
        assert_eq!(ALL_PROFILES as u32, 0x7FFF_FFFF);
    }

    #[test]
    fn writing_a_value_above_one_is_rejected_without_touching_windows() {
        let error = write(Network::Public, Field::Enabled, 2).unwrap_err();
        assert_eq!(error, "value type does not match the setting");
    }
}
