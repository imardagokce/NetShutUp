#pragma once

// dns_packet.h — minimal DNS wire-format helpers for a forwarding stub
// (pure, no I/O). Messages are treated as opaque except for the 12-byte header
// and the first question. Never reads past `len`; every function is safe on
// arbitrary bytes.
//
// Header layout (RFC 1035): bytes 0-1 ID, 2-3 flags (byte 2: QR=0x80, TC=0x02,
// RD=0x01; byte 3: RA=0x80, RCODE=low 4 bits), 4-5 QDCOUNT, 6-7 ANCOUNT,
// 8-9 NSCOUNT, 10-11 ARCOUNT (all big-endian).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace netshutup::dns::packet {

using Bytes = std::vector<std::uint8_t>;

// len >= 12, QR == 0, opcode == 0 (QUERY), QDCOUNT >= 1 and the first question
// is well-formed (see question_end).
bool is_valid_query(const std::uint8_t* data, std::size_t len);

// Offset just past the first question (QNAME + QTYPE + QCLASS), or 0 if
// malformed: label length byte > 63, compression pointer (>= 0xC0) in the
// question, QNAME not terminated, or running past `len`.
std::size_t question_end(const std::uint8_t* data, std::size_t len);

std::optional<std::uint16_t> get_id(const std::uint8_t* data, std::size_t len);   // nullopt if len < 2
bool set_id(Bytes& msg, std::uint16_t id);                                         // false if msg.size() < 2

bool is_truncated(const std::uint8_t* data, std::size_t len);   // TC bit; false if len < 12

// SERVFAIL (RCODE 2) reply for a valid query: same ID and RD bit as the query, QR=1,
// RA=1, QDCOUNT=1 with the first question echoed, AN/NS/AR counts 0.
// Returns an empty vector if the query is not valid (see is_valid_query).
Bytes make_servfail(const std::uint8_t* query, std::size_t len);

// UDP size limit: if resp.size() <= max_udp return resp unchanged. Otherwise return
// header + first question only, with TC=1 and AN/NS/AR counts zeroed (QDCOUNT=1).
// If the response header/question is malformed and too large, return the first 12 bytes
// with TC set and all counts zero. Empty input -> empty output. max_udp default 512.
Bytes truncate_for_udp(const Bytes& resp, std::size_t max_udp = 512);

// DNS-over-TCP framing: 2-byte big-endian length prefix + message. Messages longer than
// 65535 bytes yield an empty vector.
Bytes frame_tcp(const Bytes& msg);

// Length from the 2-byte prefix (nullopt if len < 2).
std::optional<std::size_t> tcp_frame_length(const std::uint8_t* data, std::size_t len);

} // namespace netshutup::dns::packet
