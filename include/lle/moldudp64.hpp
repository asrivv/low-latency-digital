#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "lle/common.hpp"

// MoldUDP64: [session:10][sequence of first message:8][count:2] then [length:2][message] * count.
namespace lle::mold {

inline constexpr std::size_t kHeaderSize = 20;
inline constexpr std::size_t kMaxPacket = 1400;
inline constexpr std::uint16_t kEndOfSession = 0xffff;
inline constexpr std::string_view kSessionName = "LLE0000001";

struct PacketHeader {
    char session[10];
    std::uint64_t sequence;
    std::uint16_t count;
};

class Packetizer {
public:
    explicit Packetizer(std::string_view session = kSessionName, std::uint64_t first_seq = 1) noexcept
        : next_seq_(first_seq) {
        store_alpha(buf_, session, 10);
    }

    bool empty() const noexcept { return count_ == 0; }
    bool fits(std::size_t n) const noexcept { return len_ + 2 + n <= kMaxPacket; }
    std::uint64_t next_sequence() const noexcept { return next_seq_; }

    std::byte* append(std::size_t n) noexcept {
        if (count_ == 0) first_seq_ = next_seq_;
        store_be16(buf_ + len_, static_cast<std::uint16_t>(n));
        std::byte* p = buf_ + len_ + 2;
        len_ += 2 + n;
        ++count_;
        ++next_seq_;
        return p;
    }

    void add(const std::byte* msg, std::size_t n) noexcept { std::memcpy(append(n), msg, n); }

    // Finalizes the header; the packet stays valid until the next append.
    std::size_t finish() noexcept {
        store_be64(buf_ + 10, first_seq_);
        store_be16(buf_ + 18, count_);
        std::size_t n = len_;
        len_ = kHeaderSize;
        count_ = 0;
        return n;
    }

    const std::byte* data() const noexcept { return buf_; }

    std::size_t heartbeat(std::byte* out) const noexcept { return control(out, 0); }
    std::size_t end_of_session(std::byte* out) const noexcept { return control(out, kEndOfSession); }

private:
    std::size_t control(std::byte* out, std::uint16_t count) const noexcept {
        std::memcpy(out, buf_, 10);
        store_be64(out + 10, next_seq_);
        store_be16(out + 18, count);
        return kHeaderSize;
    }

    std::byte buf_[kMaxPacket];
    std::size_t len_ = kHeaderSize;
    std::uint64_t next_seq_;
    std::uint64_t first_seq_ = 0;
    std::uint16_t count_ = 0;
};

// Calls on_message(seq, msg, len) for each message. Returns false on a malformed packet.
template <class F>
inline bool parse(const std::byte* data, std::size_t len, PacketHeader& h, F&& on_message) noexcept {
    if (len < kHeaderSize) return false;
    std::memcpy(h.session, data, 10);
    h.sequence = load_be64(data + 10);
    h.count = load_be16(data + 18);
    if (h.count == kEndOfSession) return true;
    std::size_t pos = kHeaderSize;
    for (std::uint16_t i = 0; i < h.count; ++i) {
        if (len - pos < 2) return false;
        std::size_t n = load_be16(data + pos);
        if (len - pos - 2 < n) return false;
        on_message(h.sequence + i, data + pos + 2, n);
        pos += 2 + n;
    }
    return true;
}

}  // namespace lle::mold
