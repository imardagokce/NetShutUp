//! The DNS providers a profile can choose from. Windows already knows these
//! three for encrypted DNS (DoH); the templates are the ones in its own list.

use super::reader::{DOH_ENABLED, DnsServer, Doh, State, Value};

pub struct Provider {
    /// What profiles call it; lowercase.
    pub id: &'static str,
    /// What the user reads.
    pub name: &'static str,
    pub servers: [&'static str; 2],
    pub doh_template: &'static str,
    /// The TLS name used for DoT (port 853).
    pub dot_host: &'static str,
}

pub const PROVIDERS: &[Provider] = &[
    Provider {
        id: "cloudflare",
        name: "Cloudflare",
        servers: ["1.1.1.1", "1.0.0.1"],
        doh_template: "https://cloudflare-dns.com/dns-query",
        dot_host: "one.one.one.one",
    },
    Provider {
        id: "google",
        name: "Google",
        servers: ["8.8.8.8", "8.8.4.4"],
        doh_template: "https://dns.google/dns-query",
        dot_host: "dns.google",
    },
    Provider {
        id: "quad9",
        name: "Quad9",
        servers: ["9.9.9.9", "149.112.112.112"],
        doh_template: "https://dns.quad9.net/dns-query",
        dot_host: "dns.quad9.net",
    },
];

pub fn find(id: &str) -> Option<&'static Provider> {
    PROVIDERS.iter().find(|provider| provider.id == id)
}

impl Provider {
    /// Both servers with DoH required: only the "enabled" flag, so there is
    /// no fallback to unencrypted queries and no automatic upgrade.
    pub fn state(&self) -> State {
        State::Value(Value::Dns(
            self.servers
                .iter()
                .map(|address| DnsServer {
                    address: (*address).to_string(),
                    doh: Some(Doh {
                        template: self.doh_template.to_string(),
                        flags: DOH_ENABLED,
                    }),
                })
                .collect(),
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn provider_table_matches_the_design() {
        let table: Vec<(&str, &str, [&str; 2], &str, &str)> = PROVIDERS
            .iter()
            .map(|p| (p.id, p.name, p.servers, p.doh_template, p.dot_host))
            .collect();
        assert_eq!(
            table,
            [
                ("cloudflare", "Cloudflare", ["1.1.1.1", "1.0.0.1"], "https://cloudflare-dns.com/dns-query", "one.one.one.one"),
                ("google", "Google", ["8.8.8.8", "8.8.4.4"], "https://dns.google/dns-query", "dns.google"),
                ("quad9", "Quad9", ["9.9.9.9", "149.112.112.112"], "https://dns.quad9.net/dns-query", "dns.quad9.net"),
            ]
        );
    }

    #[test]
    fn provider_state_requires_doh_without_fallback() {
        let State::Value(Value::Dns(servers)) = find("quad9").unwrap().state() else {
            panic!("a provider state is a DNS value");
        };
        let doh = |address: &str| DnsServer {
            address: address.to_string(),
            doh: Some(Doh { template: "https://dns.quad9.net/dns-query".to_string(), flags: 2 }),
        };
        assert_eq!(servers, [doh("9.9.9.9"), doh("149.112.112.112")]);
    }

    #[test]
    fn find_is_exact() {
        assert!(find("cloudflare").is_some());
        assert!(find("Cloudflare").is_none());
        assert!(find("").is_none());
    }
}
