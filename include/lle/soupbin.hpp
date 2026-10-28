#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "lle/common.hpp"

// SoupBinTCP 3.0 framing: [length:2, excludes itself][type:1][payload].
namespace lle::soupbin {

inline constexpr std::size_t kHeaderSize = 3;
inline constexpr std::size_t kLoginRequestPayload = 46;
inline constexpr std::size_t kLoginAcceptedPayload = 30;
inline constexpr std::size_t kLoginRejectedPayload = 1;
inline constexpr std::string_view kSessionName = "LLE0000001";

static_assert(6 + 10 + 10 + 20 == kLoginRequestPayload);
static_assert(10 + 20 == kLoginAcceptedPayload);

// Numeric ASCII, right-justified, space-padded on the left.
inline void store_numeric(std::byte* p, std::uint64_t v, std::size_t width) noexcept {
    std::memset(p, ' ', width);
    std::size_t i = width;
    do {
        p[--i] = static_cast<std::byte>('0' + v % 10);
        v /= 10;
    } while (v != 0 && i > 0);
}

inline std::uint64_t load_numeric(const std::byte* p, std::size_t width) noexcept {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < width; ++i) {
        char c = static_cast<char>(p[i]);
        if (c >= '0' && c <= '9') v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return v;
}

inline std::size_t encode_header(std::byte* out, char type, std::size_t payload_len) noexcept {
    store_be16(out, static_cast<std::uint16_t>(payload_len + 1));
    store_char(out + 2, type);
    return kHeaderSize;
}

inline std::size_t encode_frame(std::byte* out, char type, const std::byte* payload, std::size_t n) noexcept {
    encode_header(out, type, n);
    if (n != 0) std::memcpy(out + kHeaderSize, payload, n);
    return kHeaderSize + n;
}

inline std::size_t encode_empty(std::byte* out, char type) noexcept { return encode_header(out, type, 0); }

struct LoginRequest {
    std::string_view username;
    std::string_view password;
    std::string_view session;
    std::uint64_t sequence;
};

inline std::size_t encode_login_request(std::byte* out, const LoginRequest& r) noexcept {
    std::byte* p = out + encode_header(out, 'L', kLoginRequestPayload);
    store_alpha(p, r.username, 6);
    store_alpha(p + 6, r.password, 10);
    store_alpha(p + 16, r.session, 10);
    store_numeric(p + 26, r.sequence, 20);
    return kHeaderSize + kLoginRequestPayload;
}

inline std::uint64_t login_requested_sequence(const std::byte* payload) noexcept {
    return load_numeric(payload + 26, 20);
}

inline std::size_t encode_login_accepted(std::byte* out, std::string_view session, std::uint64_t seq) noexcept {
    std::byte* p = out + encode_header(out, 'A', kLoginAcceptedPayload);
    store_alpha(p, session, 10);
    store_numeric(p + 10, seq, 20);
    return kHeaderSize + kLoginAcceptedPayload;
}

inline std::size_t encode_login_rejected(std::byte* out, char reason) noexcept {
    encode_header(out, 'J', kLoginRejectedPayload);
    store_char(out + kHeaderSize, reason);
    return kHeaderSize + kLoginRejectedPayload;
}

struct ParseResult {
    std::size_t consumed;
    bool error;
};

// Calls on_frame(type, payload, payload_len) for each complete frame and stops early if it
// returns false. Leaves a trailing partial frame unconsumed. A zero length (no type byte)
// or a frame longer than max_payload is a protocol error.
template <class F>
inline ParseResult parse(const std::byte* data, std::size_t len, F&& on_frame,
                         std::size_t max_payload = 0xffff) noexcept {
    std::size_t pos = 0;
    while (len - pos >= 2) {
        std::size_t flen = load_be16(data + pos);
        if (flen == 0 || flen - 1 > max_payload) return {pos, true};
        if (len - pos < 2 + flen) break;
        bool more = on_frame(load_char(data + pos + 2), data + pos + kHeaderSize, flen - 1);
        pos += 2 + flen;
        if (!more) break;
    }
    return {pos, false};
}

}  // namespace lle::soupbin
