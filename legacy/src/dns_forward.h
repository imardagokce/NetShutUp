#pragma once

// dns_forward.h — answers one client query by walking the upstream chain
// (pure orchestration over Transport + Failover; no I/O of its own).
// Design: docs/design/2026-11-18-legacy-dns-stub.md ("Zincir ve çalışma sırasında hata").

#include "dns_config.h"
#include "dns_failover.h"
#include "dns_packet.h"
#include "dns_transport.h"
#include <optional>

namespace netshutup::dns {

enum class Source {
    Encrypted,   // answered by a chain upstream
    Legacy,      // answered by the original (plain) DNS
    Servfail,    // nothing worked; response is a generated SERVFAIL
    Invalid      // the client query was malformed; response is empty
};

struct ForwardOutcome {
    packet::Bytes         response;     // what to send back to the client
    Source                source = Source::Invalid;
    std::optional<Upstream> used;       // set when source == Encrypted
};

// Rules:
//  1. If !packet::is_valid_query(query): return {empty response, Invalid}.
//  2. plan = failover.plan().
//  3. Try plan.order in order with transport.query(chain[i], query, timeout_ms).
//     An answer is accepted only if result.ok AND the response is at least 12
//     bytes AND its ID equals the query ID AND its QR bit (0x80 of byte 2) is set.
//     A response failing those checks counts as a failure of that upstream
//     (do not accept it, continue with the next one). First accepted answer wins:
//     response = that answer, source = Encrypted, used = chain[i].
//  4. If plan.order was non-empty, call failover.round_result(winner) exactly once
//     per call (winner = index i, or nullopt if none was accepted). If plan.order
//     was empty, do not call round_result.
//  5. If no encrypted answer and plan.use_legacy and cfg.legacy_dns is non-empty:
//     transport.query_legacy(cfg.legacy_dns, query, timeout_ms); accepted under the
//     same ID/QR/size checks -> source = Legacy.
//  6. Otherwise: response = packet::make_servfail(query), source = Servfail.
// Indices in plan.order that are >= cfg.chain.size() are skipped (but still must
// not crash). The ID of an accepted response is left as returned (the transport
// already echoes the client's ID because the query bytes are forwarded unchanged).
ForwardOutcome forward(const packet::Bytes& query,
                       const Config& cfg,
                       Failover& failover,
                       Transport& transport,
                       unsigned timeout_ms);

} // namespace netshutup::dns
