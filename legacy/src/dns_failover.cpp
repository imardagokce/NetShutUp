// dns_failover.cpp — deterministic upstream/legacy fail-over state machine (NetShutUp Legacy)
#include "dns_failover.h"

#include <algorithm>

namespace netshutup::dns {

namespace {
constexpr unsigned kProbeEvery = 5;
} // namespace

Failover::Failover(std::size_t upstream_count, unsigned fail_threshold, bool has_legacy)
    : count_(std::max<std::size_t>(upstream_count, 1)),
      threshold_(std::max<unsigned>(fail_threshold, 1)),
      has_legacy_(has_legacy) {}

Plan Failover::plan() {
    Plan p;

    if (mode_ == Mode::Encrypted) {
        p.order.reserve(count_);
        for (std::size_t i = 0; i < count_; ++i) {
            p.order.push_back((current_ + i) % count_);
        }
        p.use_legacy = false;
        return p;
    }

    p.use_legacy = true;
    ++plan_calls_in_fallback_;
    if (plan_calls_in_fallback_ % kProbeEvery == 0) {
        p.order.reserve(count_);
        for (std::size_t i = 0; i < count_; ++i) {
            p.order.push_back((current_ + i) % count_);
        }
    }
    return p;
}

void Failover::round_result(std::optional<std::size_t> winner) {
    if (winner.has_value()) {
        if (*winner < count_) {
            current_ = *winner;
        }
        failed_rounds_ = 0;
        mode_ = Mode::Encrypted;
        plan_calls_in_fallback_ = 0;
        return;
    }

    ++failed_rounds_;
    if (failed_rounds_ >= threshold_ && has_legacy_) {
        mode_ = Mode::LegacyFallback;
        plan_calls_in_fallback_ = 0;
    }
}

} // namespace netshutup::dns
