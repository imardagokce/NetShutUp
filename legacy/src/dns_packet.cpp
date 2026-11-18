// dns_packet.cpp — DNS wire-format helpers for the forwarding stub (pure, no I/O)
#include "dns_packet.h"

#include <algorithm>

namespace netshutup::dns::packet {

namespace {

constexpr std::size_t kHeaderSize = 12;
constexpr std::uint8_t kFlagQr = 0x80;     // byte 2
constexpr std::uint8_t kFlagTc = 0x02;     // byte 2
constexpr std::uint8_t kFlagRd = 0x01;     // byte 2
constexpr std::uint8_t kOpcodeMask = 0x78; // byte 2, bits 3-6
constexpr std::uint8_t kFlagRa = 0x80;     // byte 3
constexpr std::uint8_t kRcodeServfail = 0x02;
constexpr std::size_t kMaxLabel = 63;
constexpr std::size_t kMaxTcpMessage = 65535;

std::uint16_t read_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((static_cast<unsigned>(p[0]) << 8) | p[1]);
}

std::uint16_t qdcount(const std::uint8_t* data) {
    return read_u16(data + 4);
}

void write_u16(Bytes& msg, std::size_t pos, std::uint16_t v) {
    msg[pos] = static_cast<std::uint8_t>(v >> 8);
    msg[pos + 1] = static_cast<std::uint8_t>(v & 0xFF);
}

// Zeroes ANCOUNT, NSCOUNT, ARCOUNT (bytes 6-11) and sets QDCOUNT to 1 (bytes 4-5).
void set_single_question_no_answers(Bytes& msg) {
    write_u16(msg, 4, 1);
    std::fill(msg.begin() + 6, msg.begin() + 12, static_cast<std::uint8_t>(0));
}

} // namespace

bool is_valid_query(const std::uint8_t* data, std::size_t len) {
    if (data == nullptr || len < kHeaderSize) return false;
    if ((data[2] & kFlagQr) != 0) return false;
    if ((data[2] & kOpcodeMask) != 0) return false;
    if (qdcount(data) < 1) return false;
    return question_end(data, len) != 0;
}

std::size_t question_end(const std::uint8_t* data, std::size_t len) {
    if (data == nullptr || len < kHeaderSize) return 0;

    std::size_t pos = kHeaderSize;
    for (;;) {
        if (pos >= len) return 0;
        const std::uint8_t label = data[pos];
        if (label == 0) {
            ++pos;
            break;
        }
        // Covers both label lengths > 63 and compression pointers (0xC0+).
        if (label > kMaxLabel) return 0;
        pos += 1 + static_cast<std::size_t>(label);
        if (pos > len) return 0;
    }

    // QTYPE + QCLASS
    if (len - pos < 4) return 0;
    return pos + 4;
}

std::optional<std::uint16_t> get_id(const std::uint8_t* data, std::size_t len) {
    if (data == nullptr || len < 2) return std::nullopt;
    return read_u16(data);
}

bool set_id(Bytes& msg, std::uint16_t id) {
    if (msg.size() < 2) return false;
    write_u16(msg, 0, id);
    return true;
}

bool is_truncated(const std::uint8_t* data, std::size_t len) {
    if (data == nullptr || len < kHeaderSize) return false;
    return (data[2] & kFlagTc) != 0;
}

Bytes make_servfail(const std::uint8_t* query, std::size_t len) {
    if (!is_valid_query(query, len)) return {};

    const std::size_t qend = question_end(query, len);
    Bytes reply(query, query + qend);

    // QR=1, opcode 0, AA/TC 0, RD copied from the query.
    reply[2] = static_cast<std::uint8_t>(kFlagQr | (query[2] & kFlagRd));
    // RA=1, RCODE=SERVFAIL.
    reply[3] = static_cast<std::uint8_t>(kFlagRa | kRcodeServfail);
    set_single_question_no_answers(reply);
    return reply;
}

Bytes truncate_for_udp(const Bytes& resp, std::size_t max_udp) {
    if (resp.empty()) return {};
    if (resp.size() <= max_udp) return resp;

    // Too large and not even a full header: nothing meaningful to send.
    if (resp.size() < kHeaderSize) return {};

    const std::size_t qend = question_end(resp.data(), resp.size());
    if (qend != 0) {
        Bytes out(resp.begin(), resp.begin() + static_cast<std::ptrdiff_t>(qend));
        out[2] = static_cast<std::uint8_t>(out[2] | kFlagTc);
        set_single_question_no_answers(out);
        return out;
    }

    // Malformed question: keep only the header, marked truncated, counts zeroed.
    Bytes out(resp.begin(), resp.begin() + static_cast<std::ptrdiff_t>(kHeaderSize));
    out[2] = static_cast<std::uint8_t>(out[2] | kFlagTc);
    std::fill(out.begin() + 4, out.begin() + 12, static_cast<std::uint8_t>(0));
    return out;
}

Bytes frame_tcp(const Bytes& msg) {
    if (msg.size() > kMaxTcpMessage) return {};

    const std::size_t n = msg.size();
    Bytes out;
    out.reserve(n + 2);
    out.push_back(static_cast<std::uint8_t>(n >> 8));
    out.push_back(static_cast<std::uint8_t>(n & 0xFF));
    out.insert(out.end(), msg.begin(), msg.end());
    return out;
}

std::optional<std::size_t> tcp_frame_length(const std::uint8_t* data, std::size_t len) {
    if (data == nullptr || len < 2) return std::nullopt;
    return static_cast<std::size_t>(read_u16(data));
}

} // namespace netshutup::dns::packet
