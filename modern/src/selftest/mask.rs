//! Hides what could identify a person or a home network. Only the addresses of
//! the DNS providers NetShutUp itself sets stay readable.

use std::net::{Ipv4Addr, Ipv6Addr};

use crate::settings::provider::PROVIDERS;

/// IPv6 addresses of the same providers (the provider table lists IPv4 only).
const KNOWN_IPV6: &[&str] = &[
    "2606:4700:4700::1111",
    "2606:4700:4700::1001",
    "2001:4860:4860::8888",
    "2001:4860:4860::8844",
    "2620:fe::fe",
    "2620:fe::9",
];

fn is_known(text: &str) -> bool {
    PROVIDERS.iter().any(|provider| provider.servers.contains(&text)) || KNOWN_IPV6.contains(&text)
}

/// An IP address as it may appear in a report. Anything that is not an
/// address is returned unchanged.
pub fn mask_ip(text: &str) -> String {
    let text = text.trim();
    if is_known(text) {
        return text.to_string();
    }
    if let Ok(address) = text.parse::<Ipv4Addr>() {
        let octets = address.octets();
        return if address.is_private() || address.is_link_local() {
            format!("{}.{}.x.x", octets[0], octets[1])
        } else {
            "x.x.x.x".to_string()
        };
    }
    if text.parse::<Ipv6Addr>().is_ok() {
        return "x:x::x".to_string();
    }
    text.to_string()
}

/// A MAC address with only the vendor part (first three bytes) left.
pub fn mask_mac(text: &str) -> String {
    let parts: Vec<&str> = text.split(['-', ':']).collect();
    let is_mac = parts.len() == 6
        && parts.iter().all(|part| part.len() == 2 && part.chars().all(|c| c.is_ascii_hexdigit()));
    if is_mac {
        format!(
            "{}-{}-{}-xx-xx-xx",
            parts[0].to_uppercase(),
            parts[1].to_uppercase(),
            parts[2].to_uppercase()
        )
    } else {
        text.to_string()
    }
}

/// An adapter GUID shortened to its first eight characters.
pub fn mask_guid(text: &str) -> String {
    text.trim_matches(['{', '}']).chars().take(8).collect()
}

fn is_guid(text: &str) -> bool {
    text.len() == 36
        && text.char_indices().all(|(index, c)| match index {
            8 | 13 | 18 | 23 => c == '-',
            _ => c.is_ascii_hexdigit(),
        })
}

/// Masks what could identify a person in free text, such as an error message:
/// addresses, MACs, GUIDs, URLs (a template can carry a personal ID) and the
/// user name in a profile path. Everything else is left as it is.
pub fn scrub(text: &str) -> String {
    let text = hide_user_names(&hide_urls(text));
    let mut out = String::with_capacity(text.len());
    let mut token = String::new();
    for ch in text.chars() {
        if ch.is_ascii_hexdigit() || matches!(ch, ':' | '.' | '-') {
            token.push(ch);
        } else {
            flush(&mut token, &mut out);
            out.push(ch);
        }
    }
    flush(&mut token, &mut out);
    out
}

/// `https://...` up to the next space, quote, bracket or comma becomes `<url>`.
fn hide_urls(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    let mut rest = text;
    loop {
        let start = match (rest.find("https://"), rest.find("http://")) {
            (Some(a), Some(b)) => a.min(b),
            (a, b) => match a.or(b) {
                Some(found) => found,
                None => break,
            },
        };
        out.push_str(&rest[..start]);
        let after = &rest[start..];
        let end = after
            .find(|c: char| c.is_whitespace() || matches!(c, '"' | '\'' | ')' | ','))
            .unwrap_or(after.len());
        out.push_str("<url>");
        rest = &after[end..];
    }
    out.push_str(rest);
    out
}

/// `C:\Users\arda\...` becomes `C:\Users\x\...`.
fn hide_user_names(text: &str) -> String {
    const MARKER_LEN: usize = 7; // "\users\" and "/users/"
    let lower = text.to_ascii_lowercase();
    let mut out = String::with_capacity(text.len());
    let mut position = 0;
    loop {
        let rest = &lower[position..];
        let found = match (rest.find("\\users\\"), rest.find("/users/")) {
            (Some(a), Some(b)) => a.min(b),
            (a, b) => match a.or(b) {
                Some(found) => found,
                None => break,
            },
        };
        let name_start = position + found + MARKER_LEN;
        out.push_str(&text[position..name_start]);
        let name = &text[name_start..];
        let length = name
            .find(|c: char| matches!(c, '\\' | '/' | '"' | '\'') || c.is_whitespace())
            .unwrap_or(name.len());
        if length > 0 {
            out.push('x');
        }
        position = name_start + length;
    }
    out.push_str(&text[position..]);
    out
}

fn flush(token: &mut String, out: &mut String) {
    if token.is_empty() {
        return;
    }
    let text = std::mem::take(token);
    // A sentence may end in a dot or a dash right after an address.
    let core = text.trim_matches(['.', '-']);
    let start = text.len() - text.trim_start_matches(['.', '-']).len();
    out.push_str(&text[..start]);
    out.push_str(&mask_core(core));
    out.push_str(&text[start + core.len()..]);
}

/// The masked form of a GUID, MAC or IP address, or `None` if `text` is none of them
/// (or is a known provider address).
fn mask_whole(text: &str) -> Option<String> {
    let masked = if is_guid(text) {
        mask_guid(text)
    } else if mask_mac(text) != text {
        mask_mac(text)
    } else {
        mask_ip(text)
    };
    (masked != text).then_some(masked)
}

/// Masks a token. When the whole token is not an address it may still be one
/// glued to a colon: `192.168.1.5:` , `192.168.1.5:53` or `failed:192.168.1.5`.
fn mask_core(core: &str) -> String {
    if let Some(masked) = mask_whole(core) {
        return masked;
    }
    for (index, _) in core.match_indices(':') {
        let (left, right) = (&core[..index], &core[index + 1..]);
        let (left_masked, right_masked) = (mask_whole(left), mask_whole(right));
        if left_masked.is_some() || right_masked.is_some() {
            return format!(
                "{}:{}",
                left_masked.unwrap_or_else(|| left.to_string()),
                right_masked.unwrap_or_else(|| right.to_string())
            );
        }
    }
    core.to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn known_provider_addresses_stay_readable() {
        for address in ["1.1.1.1", "1.0.0.1", "9.9.9.9", "149.112.112.112", "8.8.8.8", "8.8.4.4"] {
            assert_eq!(mask_ip(address), address);
        }
        assert_eq!(mask_ip("2606:4700:4700::1111"), "2606:4700:4700::1111");
    }

    #[test]
    fn private_and_public_addresses_are_masked() {
        assert_eq!(mask_ip("192.168.1.5"), "192.168.x.x");
        assert_eq!(mask_ip("10.0.8.78"), "10.0.x.x");
        assert_eq!(mask_ip("169.254.110.114"), "169.254.x.x");
        assert_eq!(mask_ip("203.0.113.7"), "x.x.x.x");
        assert_eq!(mask_ip("127.0.0.1"), "x.x.x.x");
        assert_eq!(mask_ip("fe80::ab45:3927:d4aa:78a"), "x:x::x");
        assert_eq!(mask_ip("2a01:db8::1"), "x:x::x");
        assert_eq!(mask_ip("not an address"), "not an address");
    }

    #[test]
    fn mac_keeps_only_the_vendor_part() {
        assert_eq!(mask_mac("5C-FB-3A-CC-8C-0E"), "5C-FB-3A-xx-xx-xx");
        assert_eq!(mask_mac("5c:fb:3a:cc:8c:0e"), "5C-FB-3A-xx-xx-xx");
        assert_eq!(mask_mac("-"), "-");
    }

    #[test]
    fn guid_keeps_the_first_eight_characters() {
        assert_eq!(mask_guid("{D7747AC7-885F-4A75-8F0C-84BA153614EC}"), "D7747AC7");
        assert_eq!(mask_guid("d7747ac7-885f-4a75-8f0c-84ba153614ec"), "d7747ac7");
    }

    #[test]
    fn scrub_masks_addresses_glued_to_colons_and_ports() {
        for (text, leaked) in [
            ("192.168.1.5: timed out", "192.168.1.5"),
            ("connect 192.168.1.5:53 refused", "192.168.1.5"),
            ("failed:192.168.1.5", "192.168.1.5"),
            ("mac 5C-FB-3A-CC-8C-0E: down", "CC-8C-0E"),
            ("[fe80::ab45:3927:d4aa:78a]:53", "ab45"),
        ] {
            let clean = scrub(text);
            assert!(!clean.contains(leaked), "{text} -> {clean}");
        }
        assert_eq!(scrub("1.1.1.1: ok"), "1.1.1.1: ok", "known providers stay readable");
        assert_eq!(scrub("Windows 11 build 10.0.26200.1"), "Windows 11 build 10.0.26200.1");
    }

    #[test]
    fn scrub_hides_urls_and_user_names_in_paths() {
        let clean = scrub(r#"template https://dns.example/dns-query/abc123secret failed in C:\Users\arda\AppData\x"#);
        assert!(!clean.contains("abc123secret"), "{clean}");
        assert!(!clean.contains("arda"), "{clean}");
        assert!(clean.contains("template"), "{clean}");
        assert!(clean.contains(r"C:\Users\x\AppData"), "{clean}");
    }

    #[test]
    fn scrub_masks_addresses_inside_free_text() {
        let text = r#"DnsServer { address: "192.168.1.5" } via 5C-FB-3A-CC-8C-0E, fe80::1. Adapter {D7747AC7-885F-4A75-8F0C-84BA153614EC} used 1.1.1.1"#;
        let clean = scrub(text);
        for leaked in ["192.168.1.5", "CC-8C-0E", "fe80::1", "885F-4A75"] {
            assert!(!clean.contains(leaked), "{leaked} leaked: {clean}");
        }
        assert!(clean.contains("192.168.x.x"), "{clean}");
        assert!(clean.contains("5C-FB-3A-xx-xx-xx"), "{clean}");
        assert!(clean.contains("1.1.1.1"), "known provider stays: {clean}");
        assert!(clean.contains("DnsServer { address:"), "ordinary text untouched: {clean}");
    }
}
