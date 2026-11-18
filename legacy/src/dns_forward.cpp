// dns_forward.cpp — answers one client query by walking the upstream chain (NetShutUp Legacy)
// Rules: see dns_forward.h (numbered comments). Pure orchestration; no I/O of its own.

#include "dns_forward.h"

#include <cstdint>
#include <utility>

namespace netshutup::dns {

namespace {

// An answer counts only if the transport succeeded, it is at least a header long,
// its ID matches the query and its QR bit is set.
bool accepted(const QueryResult& r, std::uint16_t id) {
    if (!r.ok || r.response.size() < 12) return false;
    const auto rid = packet::get_id(r.response.data(), r.response.size());
    return rid.has_value() && *rid == id && (r.response[2] & 0x80) != 0;
}

} // namespace

ForwardOutcome forward(const packet::Bytes& query,
                       const Config& cfg,
                       Failover& failover,
                       Transport& transport,
                       unsigned timeout_ms) {
    ForwardOutcome out;

    // Rule 1.
    if (!packet::is_valid_query(query.data(), query.size())) {
        out.source = Source::Invalid;
        return out;
    }
    const std::uint16_t id = packet::get_id(query.data(), query.size()).value_or(0);

    // Rules 2 and 3.
    const Plan plan = failover.plan();
    std::optional<std::size_t> winner;
    for (const std::size_t idx : plan.order) {
        if (idx >= cfg.chain.size()) continue;   // failover may know more upstreams than the chain
        QueryResult r = transport.query(cfg.chain[idx], query, timeout_ms);
        if (accepted(r, id)) {
            winner = idx;
            out.response = std::move(r.response);
            break;
        }
    }

    // Rule 4: exactly one round_result per call that had a non-empty order.
    if (!plan.order.empty()) {
        failover.round_result(winner);
    }

    if (winner.has_value()) {
        out.source = Source::Encrypted;
        out.used = cfg.chain[*winner];
        return out;
    }

    // Rule 5.
    if (plan.use_legacy && !cfg.legacy_dns.empty()) {
        QueryResult r = transport.query_legacy(cfg.legacy_dns, query, timeout_ms);
        if (accepted(r, id)) {
            out.response = std::move(r.response);
            out.source = Source::Legacy;
            return out;
        }
    }

    // Rule 6.
    out.response = packet::make_servfail(query.data(), query.size());
    out.source = Source::Servfail;
    return out;
}

} // namespace netshutup::dns
