#pragma once

// dns_status.h — the small status file the DNS service writes
// (%ProgramData%\NetShutUp\dns.status) and `dns-status` reads. Pure: no I/O.

#include "dns_config.h"
#include "dns_failover.h"
#include <optional>
#include <string>

namespace netshutup::dns {

struct ServiceStatus {
    Mode                    mode = Mode::Encrypted;
    std::optional<Upstream> upstream;       // last upstream that answered, if any
    unsigned long long      queries = 0;
    unsigned long long      encrypted = 0;
    unsigned long long      legacy = 0;
    unsigned long long      servfail = 0;
    std::wstring            last_error;     // short English text; no line breaks; may be empty
    std::wstring            updated_utc;    // e.g. 2026-11-18T10:00:00Z (opaque text, no spaces)
};

// Text format (UTF-8 narrowed per character, LF line endings), fixed key order:
//   NETSHUTUP_DNS_STATUS_V1
//   MODE encrypted            (or: legacy)
//   UPSTREAM quad9 dot        (line omitted when no upstream)
//   QUERIES 10
//   ENCRYPTED 8
//   LEGACY 1
//   SERVFAIL 1
//   LAST_ERROR <rest of line>  (line omitted when empty; CR/LF inside the text become spaces)
//   UPDATED 2026-11-18T10:00:00Z
//   END
std::string serialize_status(const ServiceStatus& s);

// Strict parser (CRLF accepted). nullopt on wrong header, missing END, unknown key,
// duplicate key, non-numeric counter, unknown mode/provider/method, or a missing
// MODE/QUERIES/ENCRYPTED/LEGACY/SERVFAIL/UPDATED line. UPSTREAM and LAST_ERROR optional.
std::optional<ServiceStatus> parse_status(const std::string& text);

// Human-readable English summary for `netshutup-legacy dns-status`, several lines, e.g.
//   DNS service status (updated 2026-11-18T10:00:00Z)
//   Mode:      encrypted (quad9 over dot)
//   Queries:   10 (encrypted 8, legacy 1, servfail 1)
//   Last error: ...            (line only when last_error is not empty)
// In legacy mode the Mode line says: "Mode:      FALLBACK - using the original unencrypted DNS".
// Encrypted mode without an upstream: "Mode:      encrypted (no upstream answered yet)".
std::wstring describe_status(const ServiceStatus& s);

} // namespace netshutup::dns
