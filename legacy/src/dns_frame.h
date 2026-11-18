#pragma once

// dns_frame.h — reassembles DNS-over-TCP/TLS messages (2-byte big-endian length
// prefix) from an arbitrary byte stream. Pure: no I/O. Used by the DoT client and
// the local TCP listener.

#include "dns_packet.h"
#include <cstddef>
#include <cstdint>
#include <optional>

namespace netshutup::dns {

class FrameAssembler {
public:
    // max_message: largest accepted message length (1..65535, default 65535).
    explicit FrameAssembler(std::size_t max_message = 65535);

    // Appends stream bytes. Returns false (and the assembler becomes `broken()`)
    // if a frame announces length 0 or a length > max_message; once broken every
    // later push() returns false and next() returns nullopt until reset().
    bool push(const std::uint8_t* data, std::size_t len);

    // Next complete message (without the length prefix), or nullopt if more bytes
    // are needed. Messages are returned in order; several may be ready after one push().
    std::optional<packet::Bytes> next();

    bool        broken() const { return broken_; }
    std::size_t buffered() const { return buf_.size(); }   // bytes not yet returned
    void        reset();

private:
    std::size_t   max_;
    packet::Bytes buf_;
    bool          broken_ = false;
};

} // namespace netshutup::dns
