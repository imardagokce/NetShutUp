// test_dns_failover.cpp — Unit tests for the upstream/legacy fail-over state machine (NetShutUp Legacy)

#include "dns_failover.h"
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <vector>

using namespace netshutup::dns;

static int g_failed = 0;

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            wprintf(L"FAIL  %hs:%d  %hs\n", __FILE__, __LINE__, #cond);       \
            ++g_failed;                                                       \
        }                                                                     \
    } while (0)

#define EXPECT_EQ(a, b) EXPECT((a) == (b))

using Idx = std::size_t;
using Order = std::vector<Idx>;

static const Order k_0123 = {0, 1, 2, 3};
static const Order k_2301 = {2, 3, 0, 1};
static const Order k_1230 = {1, 2, 3, 0};
static const Order k_none = {};

static void fail(Failover& f) { f.round_result(std::nullopt); }

// (1) fresh object
static void test_fresh_object() {
    Failover f(4, 3, true);
    EXPECT(f.mode() == Mode::Encrypted);
    EXPECT_EQ(f.current(), Idx(0));
    EXPECT_EQ(f.failed_rounds(), 0u);

    Plan p = f.plan();
    EXPECT(p.order == k_0123);
    EXPECT(!p.use_legacy);
}

// (2) success moves current and rotates the order
static void test_winner_moves_current() {
    Failover f(4, 3, true);
    f.round_result(Idx(2));
    EXPECT_EQ(f.current(), Idx(2));
    EXPECT(f.mode() == Mode::Encrypted);

    Plan p = f.plan();
    EXPECT(p.order == k_2301);
    EXPECT(!p.use_legacy);
}

// (3) threshold with legacy: enter fallback exactly at the threshold
static void test_threshold_enters_fallback() {
    Failover f(4, 3, true);
    fail(f);
    fail(f);
    EXPECT(f.mode() == Mode::Encrypted);
    EXPECT_EQ(f.failed_rounds(), 2u);

    fail(f);
    EXPECT(f.mode() == Mode::LegacyFallback);
}

// (4) without legacy DNS the mode never leaves Encrypted
static void test_no_legacy_never_falls_back() {
    Failover f(4, 3, false);
    for (int i = 0; i < 10; ++i) fail(f);
    EXPECT(f.mode() == Mode::Encrypted);
    EXPECT_EQ(f.failed_rounds(), 10u);

    Plan p = f.plan();
    EXPECT(p.order == k_0123);
    EXPECT(!p.use_legacy);
}

// (5) fallback plan cycle: probe on calls 5, 10, ...
static void test_fallback_probe_cycle() {
    Failover f(4, 1, true);
    f.round_result(Idx(2));
    fail(f);
    EXPECT(f.mode() == Mode::LegacyFallback);

    for (int call = 1; call <= 10; ++call) {
        Plan p = f.plan();
        EXPECT(p.use_legacy);
        if (call == 5 || call == 10) {
            EXPECT(p.order == k_2301);
        } else {
            EXPECT(p.order.empty());
        }
    }
}

// (6) success in fallback returns to Encrypted and restarts the probe counter
static void test_success_in_fallback_restarts_probe() {
    Failover f(4, 1, true);
    fail(f);
    EXPECT(f.mode() == Mode::LegacyFallback);
    for (int i = 0; i < 3; ++i) (void)f.plan();

    f.round_result(Idx(1));
    EXPECT(f.mode() == Mode::Encrypted);
    EXPECT_EQ(f.failed_rounds(), 0u);
    EXPECT_EQ(f.current(), Idx(1));

    Plan p = f.plan();
    EXPECT(p.order == k_1230);
    EXPECT(!p.use_legacy);

    fail(f);
    EXPECT(f.mode() == Mode::LegacyFallback);
    for (int call = 1; call <= 5; ++call) {
        Plan q = f.plan();
        EXPECT(q.use_legacy);
        if (call == 5) {
            EXPECT(!q.order.empty());
        } else {
            EXPECT(q.order.empty());
        }
    }
}

// (7) a success in the middle resets failed_rounds
static void test_success_resets_failed_rounds() {
    Failover f(4, 3, true);
    fail(f);
    fail(f);
    f.round_result(Idx(0));
    fail(f);
    fail(f);
    EXPECT(f.mode() == Mode::Encrypted);
    EXPECT_EQ(f.failed_rounds(), 2u);
}

// (8) out-of-range winner: current unchanged, but treated as success
static void test_out_of_range_winner_is_success() {
    Failover f(4, 1, true);
    fail(f);
    EXPECT(f.mode() == Mode::LegacyFallback);

    f.round_result(Idx(99));
    EXPECT(f.mode() == Mode::Encrypted);
    EXPECT_EQ(f.failed_rounds(), 0u);
    EXPECT_EQ(f.current(), Idx(0));
}

// (9) constructor clamps: count 0 -> 1, threshold 0 -> 1
static void test_constructor_clamps() {
    Failover f(0, 0, true);
    Plan p = f.plan();
    EXPECT(p.order == Order{0});
    EXPECT(!p.use_legacy);

    fail(f);
    EXPECT(f.mode() == Mode::LegacyFallback);
}

int main() {
    wprintf(L"Running DNS failover tests...\n");

    test_fresh_object();
    test_winner_moves_current();
    test_threshold_enters_fallback();
    test_no_legacy_never_falls_back();
    test_fallback_probe_cycle();
    test_success_in_fallback_restarts_probe();
    test_success_resets_failed_rounds();
    test_out_of_range_winner_is_success();
    test_constructor_clamps();

    if (g_failed == 0) {
        wprintf(L"All DNS failover tests passed.\n");
        return EXIT_SUCCESS;
    } else {
        wprintf(L"%d DNS failover test(s) FAILED.\n", g_failed);
        return EXIT_FAILURE;
    }
}
