// test_dns_forward.cpp — Unit tests for dns::forward() (NetShutUp Legacy)
// Pure logic: a FakeTransport replaces the network; no real I/O, no threads.

#include "dns_config.h"
#include "dns_failover.h"
#include "dns_forward.h"
#include "dns_packet.h"
#include "dns_transport.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace netshutup::dns;
namespace pkt = netshutup::dns::packet;

static int g_failed = 0;

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            wprintf(L"FAIL  %hs:%d  %hs\n", __FILE__, __LINE__, #cond);       \
            ++g_failed;                                                       \
        }                                                                     \
    } while (0)

#define EXPECT_EQ(a, b) EXPECT((a) == (b))

// ── Fake transport ──────────────────────────────────────────────────────────

static const std::string kLegacyKey = "legacy";

static std::string key_of(const Upstream& u) {
    std::string s;
    for (const wchar_t* p = u.provider_id.c_str(); *p; ++p) s += static_cast<char>(*p);
    s += (u.method == Method::DoH) ? "/doh" : "/dot";
    return s;
}

static QueryResult fail_result() {
    QueryResult r;
    r.ok = false;
    r.error = L"timeout";
    return r;
}

static QueryResult ok_result(const pkt::Bytes& bytes) {
    QueryResult r;
    r.ok = true;
    r.response = bytes;
    return r;
}

class FakeTransport : public Transport {
public:
    std::vector<std::string>   calls;              // "<provider>/<doh|dot>" or "legacy", in order
    std::vector<unsigned>      timeouts;           // timeout_ms of every call, in order
    std::vector<std::wstring>  legacy_servers;     // servers of the last query_legacy call
    std::map<std::string, QueryResult> upstream;   // scripted per upstream key; missing -> fail
    QueryResult                legacy_result = fail_result();   // returned by every legacy call

    QueryResult query(const Upstream& u, const pkt::Bytes& /*wire*/, unsigned timeout_ms) override {
        const std::string k = key_of(u);
        calls.push_back(k);
        timeouts.push_back(timeout_ms);
        auto it = upstream.find(k);
        return it != upstream.end() ? it->second : fail_result();
    }

    QueryResult query_legacy(const std::vector<std::wstring>& servers,
                             const pkt::Bytes& /*wire*/,
                             unsigned timeout_ms) override {
        calls.push_back(kLegacyKey);
        timeouts.push_back(timeout_ms);
        legacy_servers = servers;
        return legacy_result;
    }

    std::size_t count(const std::string& k) const {
        std::size_t n = 0;
        for (const auto& c : calls) if (c == k) ++n;
        return n;
    }
    std::size_t encrypted_calls() const {
        std::size_t n = 0;
        for (const auto& c : calls) if (c != kLegacyKey) ++n;
        return n;
    }
    void clear_calls() { calls.clear(); timeouts.clear(); }
};

// ── Packet helpers ──────────────────────────────────────────────────────────

// example.com A query, 29 bytes: ID 0x1234, RD set, QDCOUNT 1.
static pkt::Bytes sample_query() {
    return pkt::Bytes{
        0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        7, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
        3, 'c', 'o', 'm', 0,
        0x00, 0x01,   // QTYPE A
        0x00, 0x01    // QCLASS IN
    };
}

static pkt::Bytes good_response(const pkt::Bytes& q) {
    pkt::Bytes r = q;
    r[2] |= 0x80;     // QR = 1
    return r;
}

static pkt::Bytes wrong_id_response(const pkt::Bytes& q) {
    pkt::Bytes r = good_response(q);
    r[0] = 0xBE;
    r[1] = 0xEF;
    return r;
}

// Privacy chain (quad9 doh, quad9 dot, cloudflare doh, cloudflare dot) cut to `upstreams`.
static Config make_cfg(bool with_legacy, std::size_t upstreams = 4) {
    Config c;
    c.chain = chain_for_profile(L"privacy");
    c.chain.resize(upstreams);
    if (with_legacy) c.legacy_dns = {L"192.168.1.1"};
    return c;
}

// Scripts every upstream of the chain to fail (the default, made explicit).
static void fail_all_upstreams(FakeTransport& ft, const Config& cfg) {
    for (const auto& u : cfg.chain) ft.upstream[key_of(u)] = fail_result();
}

// Drives a Failover (threshold 2, legacy present) into LegacyFallback via two failed rounds.
static void enter_fallback(Failover& fo, FakeTransport& ft, const Config& cfg, const pkt::Bytes& q) {
    fail_all_upstreams(ft, cfg);
    forward(q, cfg, fo, ft, 1000);
    forward(q, cfg, fo, ft, 1000);
}

// ── Tests ───────────────────────────────────────────────────────────────────

static void test_first_upstream_answers() {
    FakeTransport ft;
    Config cfg = make_cfg(false);
    Failover fo(cfg.chain.size(), 3, false);
    const pkt::Bytes q = sample_query();
    const pkt::Bytes resp = good_response(q);
    ft.upstream[key_of(cfg.chain[0])] = ok_result(resp);

    ForwardOutcome out = forward(q, cfg, fo, ft, 2000);

    EXPECT(out.source == Source::Encrypted);
    EXPECT(out.used.has_value());
    EXPECT(out.used.has_value() && *out.used == cfg.chain[0]);
    EXPECT(out.response == resp);
    EXPECT_EQ(fo.current(), 0u);
    EXPECT_EQ(ft.calls.size(), 1u);
}

static void test_fallback_to_second_upstream() {
    FakeTransport ft;
    Config cfg = make_cfg(false);
    Failover fo(cfg.chain.size(), 3, false);
    const pkt::Bytes q = sample_query();
    const pkt::Bytes resp = good_response(q);
    ft.upstream[key_of(cfg.chain[0])] = fail_result();
    ft.upstream[key_of(cfg.chain[1])] = ok_result(resp);

    ForwardOutcome out = forward(q, cfg, fo, ft, 2000);

    EXPECT(out.source == Source::Encrypted);
    EXPECT(out.used.has_value() && *out.used == cfg.chain[1]);
    EXPECT(out.response == resp);
    EXPECT_EQ(fo.current(), 1u);
    EXPECT_EQ(ft.calls.size(), 2u);

    // The next query must start with the upstream that worked last time (index 1).
    ft.clear_calls();
    ForwardOutcome again = forward(q, cfg, fo, ft, 2000);
    EXPECT(again.source == Source::Encrypted);
    EXPECT_EQ(ft.calls.size(), 1u);
    EXPECT(!ft.calls.empty() && ft.calls[0] == key_of(cfg.chain[1]));
}

static void test_rejects_bad_responses() {
    const pkt::Bytes q = sample_query();
    const pkt::Bytes good = good_response(q);

    // Wrong ID on the first upstream -> rejected, second upstream accepted.
    {
        FakeTransport ft;
        Config cfg = make_cfg(false);
        Failover fo(cfg.chain.size(), 3, false);
        ft.upstream[key_of(cfg.chain[0])] = ok_result(wrong_id_response(q));
        ft.upstream[key_of(cfg.chain[1])] = ok_result(good);
        ForwardOutcome out = forward(q, cfg, fo, ft, 2000);
        EXPECT(out.source == Source::Encrypted);
        EXPECT(out.used.has_value() && *out.used == cfg.chain[1]);
        EXPECT(out.response == good);
        EXPECT_EQ(ft.count(key_of(cfg.chain[0])), 1u);
    }
    // QR bit clear on the first upstream -> rejected.
    {
        FakeTransport ft;
        Config cfg = make_cfg(false);
        Failover fo(cfg.chain.size(), 3, false);
        ft.upstream[key_of(cfg.chain[0])] = ok_result(q);   // a query, QR == 0
        ft.upstream[key_of(cfg.chain[1])] = ok_result(good);
        ForwardOutcome out = forward(q, cfg, fo, ft, 2000);
        EXPECT(out.source == Source::Encrypted);
        EXPECT(out.used.has_value() && *out.used == cfg.chain[1]);
        EXPECT_EQ(ft.count(key_of(cfg.chain[0])), 1u);
    }
    // Shorter than 12 bytes on the first upstream -> rejected.
    {
        FakeTransport ft;
        Config cfg = make_cfg(false);
        Failover fo(cfg.chain.size(), 3, false);
        ft.upstream[key_of(cfg.chain[0])] = ok_result(pkt::Bytes{0x12, 0x34, 0x80, 0x00, 0x00});
        ft.upstream[key_of(cfg.chain[1])] = ok_result(good);
        ForwardOutcome out = forward(q, cfg, fo, ft, 2000);
        EXPECT(out.source == Source::Encrypted);
        EXPECT(out.used.has_value() && *out.used == cfg.chain[1]);
        EXPECT_EQ(ft.count(key_of(cfg.chain[0])), 1u);
    }
    // ok == false with bytes attached is still a failure.
    {
        FakeTransport ft;
        Config cfg = make_cfg(false);
        Failover fo(cfg.chain.size(), 3, false);
        QueryResult bad = ok_result(good);
        bad.ok = false;
        ft.upstream[key_of(cfg.chain[0])] = bad;
        ft.upstream[key_of(cfg.chain[1])] = ok_result(good);
        ForwardOutcome out = forward(q, cfg, fo, ft, 2000);
        EXPECT(out.source == Source::Encrypted);
        EXPECT(out.used.has_value() && *out.used == cfg.chain[1]);
    }
}

static void test_all_fail_no_legacy_servfail() {
    FakeTransport ft;
    Config cfg = make_cfg(false);
    Failover fo(cfg.chain.size(), 3, false);
    const pkt::Bytes q = sample_query();
    fail_all_upstreams(ft, cfg);

    ForwardOutcome out = forward(q, cfg, fo, ft, 2000);

    EXPECT(out.source == Source::Servfail);
    EXPECT(!out.used.has_value());
    EXPECT(out.response == pkt::make_servfail(q.data(), q.size()));
    EXPECT(!out.response.empty());
    EXPECT_EQ(fo.failed_rounds(), 1u);
    EXPECT(fo.mode() == Mode::Encrypted);
    EXPECT_EQ(ft.count(kLegacyKey), 0u);
}

static void test_all_fail_legacy_present_but_not_yet_fallback() {
    FakeTransport ft;
    Config cfg = make_cfg(true);
    Failover fo(cfg.chain.size(), 3, true);
    const pkt::Bytes q = sample_query();
    fail_all_upstreams(ft, cfg);
    ft.legacy_result = ok_result(good_response(q));   // would answer if asked

    ForwardOutcome out = forward(q, cfg, fo, ft, 2000);

    // Encrypted mode: plan.use_legacy is false, so legacy is not asked this round.
    EXPECT(out.source == Source::Servfail);
    EXPECT(out.response == pkt::make_servfail(q.data(), q.size()));
    EXPECT_EQ(ft.count(kLegacyKey), 0u);
    EXPECT_EQ(fo.failed_rounds(), 1u);
    EXPECT(fo.mode() == Mode::Encrypted);
}

static void test_threshold_enters_fallback_then_legacy_answers() {
    FakeTransport ft;
    Config cfg = make_cfg(true);
    Failover fo(cfg.chain.size(), 2, true);
    const pkt::Bytes q = sample_query();
    const pkt::Bytes legacy_resp = good_response(q);
    ft.legacy_result = ok_result(legacy_resp);

    enter_fallback(fo, ft, cfg, q);
    EXPECT(fo.mode() == Mode::LegacyFallback);
    EXPECT_EQ(fo.failed_rounds(), 2u);

    ft.clear_calls();
    ForwardOutcome out = forward(q, cfg, fo, ft, 2500);

    EXPECT(out.source == Source::Legacy);
    EXPECT(out.response == legacy_resp);
    EXPECT(!out.used.has_value());
    EXPECT_EQ(ft.encrypted_calls(), 0u);          // no encrypted upstream was tried
    EXPECT_EQ(ft.count(kLegacyKey), 1u);
    EXPECT(ft.legacy_servers == cfg.legacy_dns);
    EXPECT_EQ(fo.failed_rounds(), 2u);            // round_result not called
    EXPECT(fo.mode() == Mode::LegacyFallback);
}

static void test_fallback_probe_succeeds_returns_to_encrypted() {
    FakeTransport ft;
    Config cfg = make_cfg(true);
    Failover fo(cfg.chain.size(), 2, true);
    const pkt::Bytes q = sample_query();
    const pkt::Bytes enc_resp = good_response(q);
    ft.legacy_result = ok_result(good_response(q));
    enter_fallback(fo, ft, cfg, q);

    // Calls 1..4 in fallback are plain legacy queries.
    for (int i = 0; i < 4; ++i) {
        ft.clear_calls();
        ForwardOutcome out = forward(q, cfg, fo, ft, 1000);
        EXPECT(out.source == Source::Legacy);
        EXPECT_EQ(ft.encrypted_calls(), 0u);
    }

    // Call 5 is the probe: the first upstream answers.
    ft.clear_calls();
    ft.upstream[key_of(cfg.chain[0])] = ok_result(enc_resp);
    ForwardOutcome out = forward(q, cfg, fo, ft, 1000);

    EXPECT(out.source == Source::Encrypted);
    EXPECT(out.used.has_value() && *out.used == cfg.chain[0]);
    EXPECT(out.response == enc_resp);
    EXPECT(fo.mode() == Mode::Encrypted);
    EXPECT_EQ(fo.failed_rounds(), 0u);
    EXPECT(!ft.calls.empty() && ft.calls[0] == key_of(cfg.chain[0]));
}

static void test_fallback_probe_fails_stays_legacy() {
    FakeTransport ft;
    Config cfg = make_cfg(true);
    Failover fo(cfg.chain.size(), 2, true);
    const pkt::Bytes q = sample_query();
    const pkt::Bytes legacy_resp = good_response(q);
    ft.legacy_result = ok_result(legacy_resp);
    enter_fallback(fo, ft, cfg, q);

    for (int i = 0; i < 4; ++i) forward(q, cfg, fo, ft, 1000);

    // Call 5 is the probe, and every encrypted upstream fails.
    ft.clear_calls();
    ForwardOutcome out = forward(q, cfg, fo, ft, 1000);

    EXPECT(out.source == Source::Legacy);
    EXPECT(out.response == legacy_resp);
    EXPECT_EQ(ft.encrypted_calls(), cfg.chain.size());   // probe tried every upstream
    EXPECT_EQ(ft.count(kLegacyKey), 1u);
    EXPECT(fo.mode() == Mode::LegacyFallback);
}

static void test_legacy_fails_servfail() {
    FakeTransport ft;
    Config cfg = make_cfg(true);
    Failover fo(cfg.chain.size(), 2, true);
    const pkt::Bytes q = sample_query();
    ft.legacy_result = fail_result();
    enter_fallback(fo, ft, cfg, q);

    ft.clear_calls();
    ForwardOutcome out = forward(q, cfg, fo, ft, 1000);

    EXPECT(out.source == Source::Servfail);
    EXPECT(out.response == pkt::make_servfail(q.data(), q.size()));
    EXPECT_EQ(ft.count(kLegacyKey), 1u);
    EXPECT(fo.mode() == Mode::LegacyFallback);
}

static void test_invalid_query_touches_nothing() {
    FakeTransport ft;
    Config cfg = make_cfg(true);
    Failover fo(cfg.chain.size(), 1, true);
    ft.legacy_result = ok_result(good_response(sample_query()));
    fail_all_upstreams(ft, cfg);

    // 5 bytes: too short for a header.
    ForwardOutcome short_out = forward(pkt::Bytes{1, 2, 3, 4, 5}, cfg, fo, ft, 1000);
    EXPECT(short_out.source == Source::Invalid);
    EXPECT(short_out.response.empty());

    // A response (QR set) is not a valid query.
    ForwardOutcome resp_out = forward(good_response(sample_query()), cfg, fo, ft, 1000);
    EXPECT(resp_out.source == Source::Invalid);
    EXPECT(resp_out.response.empty());

    EXPECT_EQ(ft.calls.size(), 0u);
    EXPECT_EQ(fo.failed_rounds(), 0u);
    EXPECT(fo.mode() == Mode::Encrypted);
    EXPECT_EQ(fo.current(), 0u);
}

static void test_timeout_passed_through() {
    FakeTransport ft;
    Config cfg = make_cfg(true);
    Failover fo(cfg.chain.size(), 1, true);
    const pkt::Bytes q = sample_query();
    fail_all_upstreams(ft, cfg);
    ft.legacy_result = ok_result(good_response(q));

    forward(q, cfg, fo, ft, 1234);            // all upstreams fail -> fallback (threshold 1)
    EXPECT(fo.mode() == Mode::LegacyFallback);
    forward(q, cfg, fo, ft, 1234);            // legacy round

    EXPECT(!ft.timeouts.empty());
    for (unsigned t : ft.timeouts) EXPECT_EQ(t, 1234u);
}

static void test_chain_smaller_than_failover() {
    FakeTransport ft;
    Config cfg = make_cfg(true, 2);              // chain has 2 entries...
    Failover fo(4, 2, true);                     // ...failover knows 4 upstreams
    const pkt::Bytes q = sample_query();
    fail_all_upstreams(ft, cfg);

    ForwardOutcome out = forward(q, cfg, fo, ft, 1000);
    EXPECT(out.source == Source::Servfail);      // indices 2 and 3 skipped, no crash
    EXPECT_EQ(ft.encrypted_calls(), 2u);
    EXPECT_EQ(fo.failed_rounds(), 1u);

    // Upstream at index 1 now answers and becomes current.
    ft.upstream[key_of(cfg.chain[1])] = ok_result(good_response(q));
    ft.clear_calls();
    ForwardOutcome ok_out = forward(q, cfg, fo, ft, 1000);
    EXPECT(ok_out.source == Source::Encrypted);
    EXPECT(ok_out.used.has_value() && *ok_out.used == cfg.chain[1]);
    EXPECT_EQ(fo.current(), 1u);
    EXPECT_EQ(ft.count(key_of(cfg.chain[1])), 1u);
    EXPECT_EQ(ft.calls.size(), 2u);              // index 0 still fails (current was 0), then index 1
    EXPECT_EQ(ft.count(key_of(cfg.chain[0])), 1u);
}

int main() {
    wprintf(L"Running DNS forward tests...\n");

    test_first_upstream_answers();
    test_fallback_to_second_upstream();
    test_rejects_bad_responses();
    test_all_fail_no_legacy_servfail();
    test_all_fail_legacy_present_but_not_yet_fallback();
    test_threshold_enters_fallback_then_legacy_answers();
    test_fallback_probe_succeeds_returns_to_encrypted();
    test_fallback_probe_fails_stays_legacy();
    test_legacy_fails_servfail();
    test_invalid_query_touches_nothing();
    test_timeout_passed_through();
    test_chain_smaller_than_failover();

    if (g_failed) {
        wprintf(L"\n%d check(s) FAILED\n", g_failed);
        return 1;
    }
    wprintf(L"\nAll DNS forward tests passed.\n");
    return 0;
}
