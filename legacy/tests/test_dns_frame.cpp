// test_dns_frame.cpp — Unit tests for the DNS-over-TCP frame assembler (NetShutUp Legacy)

#include "dns_frame.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <string>
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

namespace {

using Bytes = packet::Bytes;

Bytes B(std::initializer_list<std::uint8_t> il) { return Bytes(il); }

Bytes S(const std::string& s) {
    Bytes out;
    for (char c : s) out.push_back(static_cast<std::uint8_t>(c));
    return out;
}

// Concatenates a 2-byte big-endian prefix for `len` with `body`.
Bytes framed(std::size_t len, const Bytes& body) {
    Bytes out;
    out.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(len & 0xFF));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

bool is(const std::optional<Bytes>& got, const Bytes& want) {
    return got.has_value() && *got == want;
}

void test_complete_frame_single_push() {
    FrameAssembler f;
    Bytes in = B({0x00, 0x03, 'a', 'b', 'c'});
    EXPECT(f.push(in.data(), in.size()));
    EXPECT(is(f.next(), S("abc")));
    EXPECT(!f.next().has_value());
    EXPECT_EQ(f.buffered(), std::size_t{0});
    EXPECT(!f.broken());
}

void test_byte_by_byte() {
    FrameAssembler f;
    Bytes in = B({0x00, 0x03, 'a', 'b', 'c'});
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT(f.push(&in[i], 1));
        EXPECT(!f.next().has_value());
    }
    EXPECT(f.push(&in[4], 1));
    EXPECT(is(f.next(), S("abc")));
    EXPECT(!f.next().has_value());
    EXPECT_EQ(f.buffered(), std::size_t{0});
}

void test_two_frames_one_push() {
    FrameAssembler f;
    Bytes in = B({0x00, 0x02, 'x', 'y', 0x00, 0x01, 'z'});
    EXPECT(f.push(in.data(), in.size()));
    EXPECT(is(f.next(), S("xy")));
    EXPECT(is(f.next(), S("z")));
    EXPECT(!f.next().has_value());
    EXPECT_EQ(f.buffered(), std::size_t{0});
}

void test_frame_plus_partial_next() {
    FrameAssembler f;
    // First frame complete; second frame has prefix and 1 of 2 body bytes.
    Bytes in = B({0x00, 0x01, 'a', 0x00, 0x02, 'b'});
    EXPECT(f.push(in.data(), in.size()));
    EXPECT(is(f.next(), S("a")));
    EXPECT(!f.next().has_value());
    EXPECT_EQ(f.buffered(), std::size_t{3});

    Bytes rest = B({'c'});
    EXPECT(f.push(rest.data(), rest.size()));
    EXPECT(is(f.next(), S("bc")));
    EXPECT(!f.next().has_value());
    EXPECT_EQ(f.buffered(), std::size_t{0});
}

void test_split_inside_prefix() {
    FrameAssembler f;
    Bytes first = B({0x00});
    EXPECT(f.push(first.data(), first.size()));
    EXPECT(!f.next().has_value());
    EXPECT_EQ(f.buffered(), std::size_t{1});

    Bytes rest = B({0x02, 'h', 'i'});
    EXPECT(f.push(rest.data(), rest.size()));
    EXPECT(is(f.next(), S("hi")));
    EXPECT(!f.next().has_value());
    EXPECT(!f.broken());
}

void test_length_zero_breaks() {
    FrameAssembler f;
    Bytes zero = B({0x00, 0x00});
    EXPECT(!f.push(zero.data(), zero.size()));
    EXPECT(f.broken());
    EXPECT(!f.next().has_value());

    Bytes later = B({0x00, 0x01, 'a'});
    EXPECT(!f.push(later.data(), later.size()));
    EXPECT(f.broken());
    EXPECT(!f.next().has_value());
}

void test_length_over_max_breaks() {
    FrameAssembler f(100);
    Bytes over = B({0x00, 0x65}); // announces 101
    EXPECT(!f.push(over.data(), over.size()));
    EXPECT(f.broken());
    EXPECT(!f.next().has_value());
}

void test_length_exactly_max_accepted() {
    FrameAssembler f(100);
    Bytes body(100, 0x41);
    Bytes in = framed(100, body);
    EXPECT(f.push(in.data(), in.size()));
    EXPECT(!f.broken());
    EXPECT(is(f.next(), body));
    EXPECT(!f.next().has_value());
}

void test_max_clamped_zero_behaves_as_one() {
    FrameAssembler f(0);
    Bytes one = B({0x00, 0x01, 'a'});
    EXPECT(f.push(one.data(), one.size()));
    EXPECT(is(f.next(), S("a")));

    FrameAssembler g(0);
    Bytes two = B({0x00, 0x02, 'a', 'b'});
    EXPECT(!g.push(two.data(), two.size()));
    EXPECT(g.broken());
}

void test_max_clamped_large_accepts_65535_prefix() {
    // 70000 clamps to 65535; a 16-bit prefix can never exceed that, so 0xFFFF must not break.
    FrameAssembler f(70000);
    Bytes prefix = B({0xFF, 0xFF});
    EXPECT(f.push(prefix.data(), prefix.size()));
    EXPECT(!f.broken());
}

void test_reset_clears_buffer() {
    FrameAssembler f;
    Bytes partial = B({0x00, 0x05, 'a'});
    EXPECT(f.push(partial.data(), partial.size()));
    EXPECT(!f.next().has_value());
    EXPECT_EQ(f.buffered(), std::size_t{3});

    f.reset();
    EXPECT_EQ(f.buffered(), std::size_t{0});
    Bytes fresh = B({0x00, 0x01, 'z'});
    EXPECT(f.push(fresh.data(), fresh.size()));
    EXPECT(is(f.next(), S("z")));
}

void test_reset_clears_broken() {
    FrameAssembler f;
    Bytes zero = B({0x00, 0x00});
    EXPECT(!f.push(zero.data(), zero.size()));
    EXPECT(f.broken());

    f.reset();
    EXPECT(!f.broken());
    EXPECT_EQ(f.buffered(), std::size_t{0});
    Bytes ok = B({0x00, 0x01, 'k'});
    EXPECT(f.push(ok.data(), ok.size()));
    EXPECT(is(f.next(), S("k")));
}

void test_null_push_harmless() {
    FrameAssembler f;
    EXPECT(f.push(nullptr, 0));
    EXPECT(!f.broken());
    EXPECT_EQ(f.buffered(), std::size_t{0});
    EXPECT(!f.next().has_value());
}

void test_max_size_frame_round_trip() {
    FrameAssembler f;
    Bytes body(65535);
    for (std::size_t i = 0; i < body.size(); ++i) {
        body[i] = static_cast<std::uint8_t>(i * 31 + 7);
    }
    Bytes in = framed(65535, body);
    EXPECT(f.push(in.data(), in.size()));
    EXPECT(!f.broken());
    EXPECT(is(f.next(), body));
    EXPECT(!f.next().has_value());
    EXPECT_EQ(f.buffered(), std::size_t{0});
}

} // namespace

int main() {
    wprintf(L"Running DNS frame assembler tests...\n");

    test_complete_frame_single_push();
    test_byte_by_byte();
    test_two_frames_one_push();
    test_frame_plus_partial_next();
    test_split_inside_prefix();
    test_length_zero_breaks();
    test_length_over_max_breaks();
    test_length_exactly_max_accepted();
    test_max_clamped_zero_behaves_as_one();
    test_max_clamped_large_accepts_65535_prefix();
    test_reset_clears_buffer();
    test_reset_clears_broken();
    test_null_push_harmless();
    test_max_size_frame_round_trip();

    if (g_failed == 0) {
        wprintf(L"All DNS frame tests passed.\n");
        return 0;
    }
    wprintf(L"%d check(s) failed.\n", g_failed);
    return 1;
}
