//! The report as a markdown document a person can read.

use super::diagnostics::Diagnostics;
use super::{Check, Report, Status};

const CONTENTS: &str = "Windows version and edition, processor and memory, computer make and model, \
network adapter hardware and drivers, how many virtual adapters (VPN, tunnels) exist, whether the \
computer is joined to a domain or has policies for DNS and telemetry, the names of registered \
antivirus and firewall products, whether a proxy is set, and what every test did and how long it \
took. It contains no user name, computer name, Wi-Fi name, full IP or MAC addresses or serial \
numbers. Nothing was sent anywhere: this file exists only on this computer.";

pub fn render_markdown(report: &Report) -> String {
    let (pass, fail, skip, info) = report.counts();
    let mut out = String::new();

    out.push_str("# NetShutUp self-test report\n\n");
    if !report.all_restored {
        out.push_str("> ❌ **NOT everything was restored.** Some settings may still be changed. Run `netshutup-modern revert` (or choose \"Revert last change\" in the menu), then send this report to the developer.\n\n");
    }
    out.push_str(&format!(
        "**Summary:** ✅ {pass} ❌ {fail} ⏭ {skip} ℹ️ {info} · Everything restored: {} · Program {} · Created {} · Took {:.1} s\n\n",
        if report.all_restored { "yes" } else { "NO" },
        report.program_version,
        report.created,
        report.total_millis as f64 / 1000.0,
    ));

    out.push_str("## What this report contains\n\n");
    out.push_str(CONTENTS);
    out.push_str("\n\n");

    out.push_str("## Environment\n\n");
    out.push_str(&checks_table(&report.environment));
    out.push_str("\n## Safety net\n\n");
    out.push_str(&checks_table(&report.safety));
    out.push_str("\nTo go back: use System Restore, or double-click the .reg files in the reg-backups folder (the folder name is shown on screen).\n");
    out.push_str("\n## Diagnostics\n\n");
    out.push_str(&diagnostics_list(&report.diagnostics));
    out.push_str("\n## Reading checks\n\n");
    out.push_str(&checks_table(&report.reading));

    out.push_str("\n## Profile cycles\n\n");
    if report.cycles.is_empty() {
        out.push_str("No profile was applied.\n");
    }
    for cycle in &report.cycles {
        out.push_str(&format!("### {}\n\n", cycle.profile));
        out.push_str(&checks_table(&cycle.checks));
        out.push('\n');
    }

    out.push_str("## Closing checks\n\n");
    out.push_str(&checks_table(&report.closing));

    out.push_str("\n## How to send this report\n\n");
    out.push_str("1. Read it first; it should contain nothing personal.\n");
    out.push_str("2. Open a new issue on GitHub with the **Test report** form and attach both files (`.md` and `.json`).\n");
    out.push_str("3. A security problem goes through the private report link instead, not a public issue.\n");
    out
}

fn icon(status: Status) -> &'static str {
    match status {
        Status::Pass => "✅",
        Status::Fail => "❌",
        Status::Skip => "⏭",
        Status::Info => "ℹ️",
    }
}

fn cell(text: &str) -> String {
    text.replace('|', "\\|").replace(['\r', '\n'], " ")
}

fn checks_table(checks: &[Check]) -> String {
    if checks.is_empty() {
        return "None.\n".to_string();
    }
    let mut out = String::from("| Step | Result | ms | Note |\n|---|---|---|---|\n");
    for check in checks {
        out.push_str(&format!(
            "| {} | {} | {} | {} |\n",
            cell(&check.title),
            icon(check.status),
            check.millis,
            cell(&check.detail)
        ));
    }
    out
}

fn diagnostics_list(d: &Diagnostics) -> String {
    let mut lines = vec![
        format!("- Program: {}", d.program),
        format!(
            "- Windows: {} {} (build {}{}) {} {}",
            d.windows.name,
            d.windows.display_version.as_deref().unwrap_or(""),
            d.windows.build,
            d.windows.revision.map(|r| format!(".{r}")).unwrap_or_default(),
            d.windows.edition.as_deref().unwrap_or(""),
            d.windows.arch
        ),
    ];
    let mut optional = |label: &str, value: Option<String>| {
        if let Some(value) = value {
            lines.push(format!("- {label}: {value}"));
        }
    };
    optional("Locale", d.locale.clone());
    optional("UTC offset (minutes)", d.utc_offset_minutes.map(|v| v.to_string()));
    optional("Processor", d.cpu.clone());
    optional("Logical cores", d.logical_cores.map(|v| v.to_string()));
    optional("Memory (MB)", d.ram_mb.map(|v| v.to_string()));
    optional("Computer", d.machine.as_ref().map(|m| format!("{} {}", m.manufacturer, m.model)));
    optional("Domain joined", d.domain_joined.map(|v| v.to_string()));
    optional("Proxy enabled", d.proxy_enabled.map(|v| v.to_string()));
    for adapter in &d.adapters {
        let driver = adapter
            .driver
            .as_ref()
            .map(|driver| format!("{} {} ({})", driver.provider, driver.version, driver.date))
            .unwrap_or_else(|| "driver unknown".to_string());
        lines.push(format!(
            "- Adapter {} — {} [{}], {}, {} Mbit/s, MTU {}, DHCP {}, IPv6 {}, {}",
            adapter.name,
            adapter.description,
            adapter.kind,
            if adapter.up { "up" } else { "down" },
            adapter.link_mbps.map_or("?".to_string(), |v| v.to_string()),
            adapter.mtu.map_or("?".to_string(), |v| v.to_string()),
            adapter.dhcp.map_or("?".to_string(), |v| v.to_string()),
            adapter.ipv6,
            driver
        ));
    }
    for virtual_adapters in &d.virtual_adapters {
        lines.push(format!("- Virtual adapters: {} × {}", virtual_adapters.count, virtual_adapters.kind));
    }
    for policy in &d.policies {
        lines.push(format!("- Policy {} = {}", policy.name, policy.value));
    }
    if !d.security_products.is_empty() {
        lines.push(format!("- Security products: {}", d.security_products.join(", ")));
    }
    if !d.known_doh_servers.is_empty() {
        lines.push(format!("- Windows knows DoH servers: {}", d.known_doh_servers.join(", ")));
    }
    // Not scrubbed as a whole: a driver version can look like an address. Fields
    // that can hold addresses are masked when they are collected.
    lines.join("\n") + "\n"
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::selftest::diagnostics::Diagnostics;
    use crate::selftest::{Check, Cycle, Report, Status};

    fn check(id: &str, status: Status, detail: &str) -> Check {
        Check::new(id, &format!("title of {id}"), status, detail, 12)
    }

    fn sample(all_restored: bool) -> Report {
        Report {
            schema: 1,
            program_version: "0.1.0-alpha.1".into(),
            created: "20261117-150000".into(),
            total_millis: 4200,
            all_restored,
            diagnostics: Diagnostics::default(),
            environment: vec![check("admin", Status::Pass, "elevated")],
            safety: vec![check("safety.registry_export", Status::Pass, "7 file(s)")],
            reading: vec![check("read.dns", Status::Info, "a|b")],
            cycles: vec![Cycle {
                profile: "privacy".into(),
                checks: vec![
                    check("apply", Status::Pass, "3 changed"),
                    check("revert", Status::Fail, "boom"),
                ],
            }],
            closing: vec![check("diff", Status::Pass, "no difference"), check("x", Status::Info, "")],
        }
    }

    #[test]
    fn render_puts_the_not_restored_warning_first() {
        let text = render_markdown(&sample(false));
        let first = text
            .lines()
            .skip_while(|line| line.starts_with('#') || line.trim().is_empty())
            .next()
            .unwrap();
        assert!(first.contains("NOT everything was restored"), "{first}");
        assert!(first.contains("netshutup-modern revert"), "{first}");
        assert!(!render_markdown(&sample(true)).contains("NOT everything was restored"));
    }

    #[test]
    fn render_shows_counts_icons_and_escapes_pipes() {
        let text = render_markdown(&sample(true));
        assert!(text.contains("✅ 4 ❌ 1 ⏭ 0 ℹ️ 2"), "{text}");
        assert!(text.contains("a\\|b"), "{text}");
        assert!(text.contains("### privacy"), "{text}");
        assert!(text.contains("Everything restored: yes"), "{text}");
    }

    #[test]
    fn render_has_a_safety_net_section() {
        let text = render_markdown(&sample(true));
        let environment = text.find("## Environment").unwrap();
        let safety = text.find("## Safety net").expect("safety section");
        assert!(safety > environment, "{text}");
        assert!(text.contains("title of safety.registry_export"), "{text}");
        assert!(text.contains("To go back: use System Restore"), "{text}");
        assert!(text.contains("reg-backups"), "{text}");
    }

    #[test]
    fn report_json_has_a_safety_section() {
        let json = serde_json::to_value(sample(true)).unwrap();
        assert_eq!(json["safety"][0]["id"], "safety.registry_export");
    }

    #[test]
    fn render_includes_the_what_this_report_contains_section() {
        let text = render_markdown(&sample(true));
        assert!(text.contains("## What this report contains"), "{text}");
        assert!(text.contains("How to send this report"), "{text}");
    }

    #[test]
    fn render_keeps_driver_versions_that_look_like_addresses() {
        use crate::selftest::diagnostics::{AdapterInfo, DriverInfo};
        let mut report = sample(true);
        report.diagnostics.adapters.push(AdapterInfo {
            name: "Ethernet 1".into(),
            description: "Intel Ethernet".into(),
            kind: "Ethernet".into(),
            up: true,
            link_mbps: Some(1000),
            mtu: Some(1500),
            dhcp: Some(true),
            ipv6: true,
            driver: Some(DriverInfo {
                provider: "Intel".into(),
                version: "22.250.1.2".into(),
                date: "2024-01-01".into(),
            }),
        });
        let text = render_markdown(&report);
        assert!(text.contains("22.250.1.2"), "{text}");
    }

    #[test]
    fn report_never_contains_unmasked_addresses() {
        let mut report = sample(true);
        report.closing.push(check("leak", Status::Fail, "dns 192.168.1.5 on 5C-FB-3A-CC-8C-0E"));
        let markdown = render_markdown(&report);
        let json = serde_json::to_string(&report).unwrap();
        for text in [markdown, json] {
            assert!(!text.contains("192.168.1.5"), "{text}");
            assert!(!text.contains("CC-8C-0E"), "{text}");
        }
    }
}
