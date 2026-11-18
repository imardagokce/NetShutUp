// test_preview.cpp — Unit tests for the Preview Engine (NetShutUp Legacy)

#include "preview.h"
#include "catalog.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace netshutup;

static int g_failed = 0;

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            wprintf(L"FAIL  %hs:%d  %hs\n", __FILE__, __LINE__, #cond);       \
            ++g_failed;                                                       \
        }                                                                     \
    } while (0)

#define EXPECT_EQ(a, b) EXPECT((a) == (b))

static void test_parse_profile_flag() {
    EXPECT_EQ(preview::parse_profile_flag(L"privacy"), catalog::PROFILE_PRIVACY);
    EXPECT_EQ(preview::parse_profile_flag(L"Privacy"), catalog::PROFILE_PRIVACY);
    EXPECT_EQ(preview::parse_profile_flag(L"PRIVACY"), catalog::PROFILE_PRIVACY);

    EXPECT_EQ(preview::parse_profile_flag(L"performance"), catalog::PROFILE_PERFORMANCE);
    EXPECT_EQ(preview::parse_profile_flag(L"Performance"), catalog::PROFILE_PERFORMANCE);

    EXPECT_EQ(preview::parse_profile_flag(L"recommended"), catalog::PROFILE_RECOMMENDED);
    EXPECT_EQ(preview::parse_profile_flag(L"Recommended"), catalog::PROFILE_RECOMMENDED);

    EXPECT_EQ(preview::parse_profile_flag(L"unknown_xyz"), 0u);
    EXPECT_EQ(preview::parse_profile_flag(nullptr), 0u);
}

static void test_generate_invalid() {
    auto res = preview::generate(L"invalid_profile", catalog::OS_ALL);
    EXPECT(!res.has_value());
}

static void test_generate_privacy() {
    auto res = preview::generate(L"privacy", catalog::OS_ALL);
    EXPECT(res.has_value());
    EXPECT_EQ(res->profile_name, L"privacy");
    EXPECT_EQ(res->profile_flag, catalog::PROFILE_PRIVACY);

    // Expect at least privacy.llmnr, privacy.ceip, privacy.wer
    bool has_llmnr = false, has_ceip = false, has_wer = false;
    for (const auto& item : res->items) {
        if (wcscmp(item.entry->id, L"privacy.llmnr") == 0) has_llmnr = true;
        if (wcscmp(item.entry->id, L"privacy.ceip") == 0)  has_ceip = true;
        if (wcscmp(item.entry->id, L"privacy.wer") == 0)   has_wer = true;
    }
    EXPECT(has_llmnr);
    EXPECT(has_ceip);
    EXPECT(has_wer);

    // Counts consistency
    EXPECT_EQ(res->items.size(),
              res->to_change_count + res->already_set_count + res->pending_adapter_count);
}

static void test_generate_performance() {
    // No adapters: the per-adapter entries show one pending row each.
    const std::vector<std::wstring> none;
    auto res = preview::generate(L"performance", catalog::OS_ALL, &none);
    EXPECT(res.has_value());

    // Expect perf.net_throttle, perf.tcp_nodelay, perf.tcp_ack_freq
    bool has_throttle = false, has_nodelay = false, has_ackfreq = false;
    for (const auto& item : res->items) {
        if (wcscmp(item.entry->id, L"perf.net_throttle") == 0) has_throttle = true;
        if (wcscmp(item.entry->id, L"perf.tcp_nodelay") == 0) {
            has_nodelay = true;
            EXPECT(item.action == preview::Action::ADAPTER_PENDING);
        }
        if (wcscmp(item.entry->id, L"perf.tcp_ack_freq") == 0) {
            has_ackfreq = true;
            EXPECT(item.action == preview::Action::ADAPTER_PENDING);
        }
    }
    EXPECT(has_throttle);
    EXPECT(has_nodelay);
    EXPECT(has_ackfreq);
    EXPECT_EQ(res->pending_adapter_count, 2u);
}

static void test_generate_performance_per_adapter_rows() {
    const std::vector<std::wstring> guids = {L"{AAAAAAAA-0000-0000-0000-000000000001}",
                                             L"{BBBBBBBB-0000-0000-0000-000000000002}"};
    auto res = preview::generate(L"performance", catalog::OS_ALL, &guids);
    EXPECT(res.has_value());

    int rows_nodelay = 0, rows_ackfreq = 0;
    for (const auto& item : res->items) {
        if (wcscmp(item.entry->id, L"perf.tcp_nodelay") == 0) {
            ++rows_nodelay;
            EXPECT(item.action == preview::Action::ADD);   // fake GUIDs: value absent
            EXPECT(item.label.rfind(L"perf.tcp_nodelay@{", 0) == 0);
        }
        if (wcscmp(item.entry->id, L"perf.tcp_ack_freq") == 0) {
            ++rows_ackfreq;
            EXPECT(item.label.rfind(L"perf.tcp_ack_freq@{", 0) == 0);
        }
    }
    EXPECT_EQ(rows_nodelay, 2);
    EXPECT_EQ(rows_ackfreq, 2);
    EXPECT_EQ(res->pending_adapter_count, 0u);
    EXPECT_EQ(res->items.size(),
              res->to_change_count + res->already_set_count + res->pending_adapter_count);
}

static void test_generate_recommended() {
    auto res = preview::generate(L"recommended", catalog::OS_ALL);
    EXPECT(res.has_value());

    bool has_llmnr = false;
    for (const auto& item : res->items) {
        if (wcscmp(item.entry->id, L"privacy.llmnr") == 0) has_llmnr = true;
    }
    EXPECT(has_llmnr);
}

int main() {
    wprintf(L"Running preview engine tests...\n");

    test_parse_profile_flag();
    test_generate_invalid();
    test_generate_privacy();
    test_generate_performance();
    test_generate_performance_per_adapter_rows();
    test_generate_recommended();

    if (g_failed == 0) {
        wprintf(L"All preview tests passed.\n");
        return EXIT_SUCCESS;
    } else {
        wprintf(L"%d preview test(s) FAILED.\n", g_failed);
        return EXIT_FAILURE;
    }
}
