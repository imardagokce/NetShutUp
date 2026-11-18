// dns_frame.cpp — reassembles DNS-over-TCP/TLS messages (2-byte big-endian length prefix).
// Pure: no I/O. See dns_frame.h for the contract.

#include "dns_frame.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace netshutup::dns {

namespace {

constexpr std::size_t kMinMax = 1;
constexpr std::size_t kHardMax = 65535;

std::size_t read_prefix(const std::uint8_t* p) {
    return (static_cast<std::size_t>(p[0]) << 8) | static_cast<std::size_t>(p[1]);
}

} // namespace

FrameAssembler::FrameAssembler(std::size_t max_message)
    : max_(max_message < kMinMax ? kMinMax
           : max_message > kHardMax ? kHardMax
           : max_message) {}

bool FrameAssembler::push(const std::uint8_t* data, std::size_t len) {
    if (broken_) return false;
    if (data == nullptr || len == 0) return true;

    buf_.insert(buf_.end(), data, data + len);

    // Validate every length prefix present in the buffer, not just the first one:
    // a bad frame may sit behind complete good frames that next() has not returned yet.
    std::size_t pos = 0;
    while (buf_.size() - pos >= 2) {
        const std::size_t frame_len = read_prefix(buf_.data() + pos);
        if (frame_len == 0 || frame_len > max_) {
            broken_ = true;
            buf_.clear();
            return false;
        }
        if (buf_.size() - pos - 2 < frame_len) break;   // incomplete frame: stop here
        pos += 2 + frame_len;
    }
    return true;
}

std::optional<packet::Bytes> FrameAssembler::next() {
    if (broken_ || buf_.size() < 2) return std::nullopt;

    const std::size_t frame_len = read_prefix(buf_.data());
    if (buf_.size() - 2 < frame_len) return std::nullopt;

    packet::Bytes msg(buf_.begin() + 2, buf_.begin() + static_cast<std::ptrdiff_t>(2 + frame_len));
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(2 + frame_len));
    return msg;
}

void FrameAssembler::reset() {
    buf_.clear();
    broken_ = false;
}

} // namespace netshutup::dns
