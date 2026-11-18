#pragma once

// dns_failover.h — deterministic upstream/legacy fail-over state machine
// (pure, no clock, no I/O). Thread-safe: every member is guarded by an internal
// mutex, so the server's worker threads can share one instance.
// Design: docs/design/2026-11-18-legacy-dns-stub.md ("Zincir ve çalışma sırasında hata").

#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

namespace netshutup::dns {

enum class Mode { Encrypted, LegacyFallback };

struct Plan {
    std::vector<std::size_t> order;      // upstream indices to try, in order (may be empty)
    bool                     use_legacy = false;   // after `order` is exhausted, forward to legacy DNS
};

class Failover {
public:
    // upstream_count >= 1 (clamped); fail_threshold clamped to >= 1;
    // has_legacy: whether an original DNS server is on record.
    Failover(std::size_t upstream_count, unsigned fail_threshold, bool has_legacy);

    Mode        mode() const { std::lock_guard<std::mutex> l(mu_); return mode_; }
    std::size_t current() const { std::lock_guard<std::mutex> l(mu_); return current_; }   // last upstream that worked
    unsigned    failed_rounds() const { std::lock_guard<std::mutex> l(mu_); return failed_rounds_; }

    // Plan for the next query.
    //  Encrypted mode: order = current, current+1, ... wrapping once around (all
    //    upstream_count indices, each once); use_legacy = false.
    //  LegacyFallback mode: use_legacy = true. Every 5th call to plan() in this mode
    //    (counting from 1, i.e. calls 5, 10, ...) is a probe: order = all indices
    //    starting at current (wrapping); other calls: order empty.
    Plan plan();

    // Result of one finished query round.
    //  winner set: current = *winner (ignored if out of range -> treated as no change
    //    to current), failed_rounds = 0, mode = Encrypted, probe counter reset.
    //  winner empty: failed_rounds++; if failed_rounds >= fail_threshold and has_legacy
    //    the mode becomes LegacyFallback. Without legacy DNS the mode never leaves Encrypted.
    void round_result(std::optional<std::size_t> winner);

private:
    mutable std::mutex mu_;
    std::size_t count_;
    unsigned    threshold_;
    bool        has_legacy_;
    Mode        mode_ = Mode::Encrypted;
    std::size_t current_ = 0;
    unsigned    failed_rounds_ = 0;
    unsigned    plan_calls_in_fallback_ = 0;
};

} // namespace netshutup::dns
