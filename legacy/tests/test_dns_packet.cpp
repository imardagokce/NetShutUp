// test_dns_packet.cpp — Unit tests for the DNS wire-format helpers (NetShutUp Legacy)

#include "dns_packet.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <vector>

namespace pk = netshutup::dns::packet;
using Bytes = pk::Bytes;

static int g_failed = 0;

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            wprintf(L"FAIL  %hs:%d  %hs\n", __FILE__, __LINE__, #cond);       \
            ++g_failed;                                                       \
        }                                                                     \
    } while (0)

#define EXPECT_EQ(a, b) EXPECT((a) == (b))

// Query for "example.com" type A class IN, ID as given, RD set. 29 bytes.
static Bytes sample_query(std::uint16_t id = 0x1234) {
    return Bytes{
        static_cast<std::uint8_t>(id >> 8), static_cast<std::uint8_t>(id & 0xFF),
        0x01, 0x00,                                   // flags: RD
        0x00, 0x01,                                   // QDCOUNT 1
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,           // ANCOUNT, NSCOUNT, ARCOUNT
        7, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
        3, 'c', 'o', 'm',
        0,
        0x00, 0x01,                                   // QTYPE A
        0x00, 0x01,                                   // QCLASS IN
    };
}

static void test_is_valid_query() {
    Bytes q = sample_query();
    EXPECT(pk::is_valid_query(q.data(), q.size()));

    // Too short
    EXPECT(!pk::is_valid_query(q.data(), 11));

    // Response (QR=1)
    Bytes resp = q;
    resp[2] |= 0x80;
    EXPECT(!pk::is_valid_query(resp.data(), resp.size()));

    // Opcode != 0 (flags 0x0900 -> opcode 1)
    Bytes op = q;
    op[2] = 0x09;
    op[3] = 0x00;
    EXPECT(!pk::is_valid_query(op.data(), op.size()));

    // QDCOUNT 0
    Bytes zero = q;
    zero[4] = 0x00;
    zero[5] = 0x00;
    EXPECT(!pk::is_valid_query(zero.data(), zero.size()));

    // Compression pointer in QNAME
    Bytes ptr = {
        0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0,
        0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01,
    };
    EXPECT(!pk::is_valid_query(ptr.data(), ptr.size()));

    // Label length 64
    Bytes big_label = q;
    big_label[12] = 64;
    EXPECT(!pk::is_valid_query(big_label.data(), big_label.size()));

    // Unterminated QNAME (no root label, no QTYPE/QCLASS)
    Bytes unterminated = {
        0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0,
        7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm',
    };
    EXPECT(!pk::is_valid_query(unterminated.data(), unterminated.size()));

    // Question cut short (QCLASS missing)
    EXPECT(!pk::is_valid_query(q.data(), 27));
    // Question cut short (name terminated, QTYPE/QCLASS missing)
    EXPECT(!pk::is_valid_query(q.data(), 25));
}

static void test_question_end() {
    Bytes q = sample_query();
    EXPECT_EQ(pk::question_end(q.data(), q.size()), 29u);

    // Malformed -> 0
    EXPECT_EQ(pk::question_end(q.data(), 27), 0u);

    Bytes big_label = q;
    big_label[12] = 64;
    EXPECT_EQ(pk::question_end(big_label.data(), big_label.size()), 0u);

    Bytes ptr = {
        0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0,
        0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01,
    };
    EXPECT_EQ(pk::question_end(ptr.data(), ptr.size()), 0u);

    Bytes unterminated = {
        0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0,
        7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm',
    };
    EXPECT_EQ(pk::question_end(unterminated.data(), unterminated.size()), 0u);

    // Nothing to read
    EXPECT_EQ(pk::question_end(nullptr, 0), 0u);
}

static void test_id() {
    Bytes q = sample_query(0x1234);
    auto id = pk::get_id(q.data(), q.size());
    EXPECT(id.has_value());
    EXPECT(id.has_value() && *id == 0x1234);

    EXPECT(pk::set_id(q, 0xBEEF));
    EXPECT(q[0] == 0xBE && q[1] == 0xEF);
    EXPECT(q[2] == 0x01 && q[3] == 0x00);   // flags untouched
    id = pk::get_id(q.data(), q.size());
    EXPECT(id.has_value() && *id == 0xBEEF);

    // Tiny inputs
    EXPECT(!pk::get_id(q.data(), 1).has_value());
    EXPECT(!pk::get_id(nullptr, 0).has_value());
    Bytes one = {0x01};
    EXPECT(!pk::set_id(one, 0x1234));
    Bytes empty;
    EXPECT(!pk::set_id(empty, 0x1234));
}

static void test_is_truncated() {
    Bytes q = sample_query();
    EXPECT(!pk::is_truncated(q.data(), q.size()));

    Bytes tc = q;
    tc[2] |= 0x02;
    EXPECT(pk::is_truncated(tc.data(), tc.size()));

    // Header incomplete: TC bit present in the buffer but len < 12
    EXPECT(!pk::is_truncated(tc.data(), 11));
    EXPECT(!pk::is_truncated(nullptr, 0));
}

static void test_make_servfail() {
    Bytes q = sample_query(0x1234);
    Bytes r = pk::make_servfail(q.data(), q.size());

    EXPECT_EQ(r.size(), 29u);
    auto id = pk::get_id(r.data(), r.size());
    EXPECT(id.has_value() && *id == 0x1234);
    EXPECT_EQ(r[2], static_cast<std::uint8_t>(0x81));   // QR | RD
    EXPECT_EQ(r[3], static_cast<std::uint8_t>(0x82));   // RA | RCODE 2
    EXPECT_EQ(r[4], 0x00);
    EXPECT_EQ(r[5], 0x01);                               // QDCOUNT 1
    for (std::size_t i = 6; i < 12; ++i) {
        EXPECT_EQ(r[i], 0x00);                           // AN/NS/AR zero
    }
    EXPECT(std::equal(r.begin() + 12, r.end(), q.begin() + 12));

    // Invalid query -> empty
    Bytes resp = q;
    resp[2] |= 0x80;
    EXPECT(pk::make_servfail(resp.data(), resp.size()).empty());
    EXPECT(pk::make_servfail(q.data(), 11).empty());
    EXPECT(pk::make_servfail(nullptr, 0).empty());
}

static void test_truncate_for_udp() {
    // Small response unchanged
    Bytes small = sample_query();
    EXPECT(pk::truncate_for_udp(small) == small);

    // 600-byte response: header + question with ANCOUNT=1 + 571 junk bytes
    Bytes big = sample_query(0xABCD);
    big[7] = 0x01;
    big.insert(big.end(), 571, 0xAA);
    EXPECT_EQ(big.size(), 600u);

    Bytes out = pk::truncate_for_udp(big, 512);
    EXPECT_EQ(out.size(), 29u);
    EXPECT((out[2] & 0x02) != 0);
    EXPECT(out[4] == 0x00 && out[5] == 0x01);
    for (std::size_t i = 6; i < 12; ++i) {
        EXPECT_EQ(out[i], 0x00);
    }
    auto id = pk::get_id(out.data(), out.size());
    EXPECT(id.has_value() && *id == 0xABCD);
    EXPECT(std::equal(out.begin() + 12, out.end(), big.begin() + 12));

    // Custom max_udp
    Bytes medium = sample_query();
    medium[7] = 0x01;
    medium.insert(medium.end(), 20, 0xBB);
    EXPECT_EQ(medium.size(), 49u);
    EXPECT(pk::truncate_for_udp(medium, 49) == medium);
    EXPECT(pk::truncate_for_udp(medium, 64) == medium);
    Bytes cut = pk::truncate_for_udp(medium, 40);
    EXPECT_EQ(cut.size(), 29u);
    EXPECT((cut[2] & 0x02) != 0);

    // Malformed, too large: first 12 bytes, TC set, counts zero
    Bytes bad(600, 0xFF);
    bad[0] = 0x12;
    bad[1] = 0x34;
    bad[2] = 0x00;
    bad[3] = 0x00;
    bad[4] = 0x00;
    bad[5] = 0x01;
    bad[12] = 0xC0;                                      // compression in question
    Bytes bad_out = pk::truncate_for_udp(bad, 512);
    EXPECT_EQ(bad_out.size(), 12u);
    EXPECT(bad_out[2] == 0x02);
    for (std::size_t i = 4; i < 12; ++i) {
        EXPECT_EQ(bad_out[i], 0x00);
    }
    EXPECT(bad_out[0] == 0x12 && bad_out[1] == 0x34);

    // Empty -> empty
    EXPECT(pk::truncate_for_udp(Bytes{}).empty());
}

static void test_tcp_framing() {
    Bytes q = sample_query();
    Bytes framed = pk::frame_tcp(q);
    EXPECT_EQ(framed.size(), 31u);
    EXPECT(framed[0] == 0x00 && framed[1] == 0x1D);
    EXPECT(std::equal(framed.begin() + 2, framed.end(), q.begin()));

    // 65535 is the largest representable length
    Bytes max_msg(65535, 0x00);
    Bytes max_framed = pk::frame_tcp(max_msg);
    EXPECT_EQ(max_framed.size(), 65537u);
    EXPECT(max_framed[0] == 0xFF && max_framed[1] == 0xFF);

    // 65536 bytes cannot be framed
    Bytes too_big(65536, 0x00);
    EXPECT(pk::frame_tcp(too_big).empty());

    // Length prefix read-back
    auto len = pk::tcp_frame_length(framed.data(), framed.size());
    EXPECT(len.has_value() && *len == 29u);
    auto max_len = pk::tcp_frame_length(max_framed.data(), max_framed.size());
    EXPECT(max_len.has_value() && *max_len == 65535u);
    EXPECT(!pk::tcp_frame_length(framed.data(), 1).has_value());
    EXPECT(!pk::tcp_frame_length(nullptr, 0).has_value());
}

int main() {
    wprintf(L"Running DNS packet tests...\n");

    test_is_valid_query();
    test_question_end();
    test_id();
    test_is_truncated();
    test_make_servfail();
    test_truncate_for_udp();
    test_tcp_framing();

    if (g_failed == 0) {
        wprintf(L"All DNS packet tests passed.\n");
        return EXIT_SUCCESS;
    } else {
        wprintf(L"%d DNS packet test(s) FAILED.\n", g_failed);
        return EXIT_FAILURE;
    }
}
