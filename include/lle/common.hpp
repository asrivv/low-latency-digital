#pragma once

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <pthread.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

#if defined(__linux__)
#include <sched.h>
#endif

namespace lle {

#if defined(__APPLE__) && defined(__aarch64__)
inline constexpr std::size_t kCacheLine = 128;
#else
inline constexpr std::size_t kCacheLine = 64;
#endif

inline void cpu_relax() noexcept {
#if defined(__aarch64__) && defined(__clang__)
    __builtin_arm_yield();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#endif
}

// Hot threads: hard affinity on Linux; on macOS only a QoS hint is available.
inline void make_thread_hot([[maybe_unused]] int cpu) noexcept {
#if defined(__linux__)
    if (cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    }
#elif defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}


inline std::uint16_t load_be16(const std::byte* p) noexcept {
    std::uint16_t v;
    std::memcpy(&v, p, 2);
    return std::endian::native == std::endian::little ? __builtin_bswap16(v) : v;
}
inline std::uint32_t load_be32(const std::byte* p) noexcept {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return std::endian::native == std::endian::little ? __builtin_bswap32(v) : v;
}
inline std::uint64_t load_be64(const std::byte* p) noexcept {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return std::endian::native == std::endian::little ? __builtin_bswap64(v) : v;
}
inline std::uint64_t load_be48(const std::byte* p) noexcept {
    return (std::uint64_t{load_be16(p)} << 32) | load_be32(p + 2);
}
inline void store_be16(std::byte* p, std::uint16_t v) noexcept {
    if constexpr (std::endian::native == std::endian::little) v = __builtin_bswap16(v);
    std::memcpy(p, &v, 2);
}
inline void store_be32(std::byte* p, std::uint32_t v) noexcept {
    if constexpr (std::endian::native == std::endian::little) v = __builtin_bswap32(v);
    std::memcpy(p, &v, 4);
}
inline void store_be64(std::byte* p, std::uint64_t v) noexcept {
    if constexpr (std::endian::native == std::endian::little) v = __builtin_bswap64(v);
    std::memcpy(p, &v, 8);
}
inline void store_be48(std::byte* p, std::uint64_t v) noexcept {
    store_be16(p, static_cast<std::uint16_t>(v >> 32));
    store_be32(p + 2, static_cast<std::uint32_t>(v));
}
inline char load_char(const std::byte* p) noexcept { return static_cast<char>(*p); }
inline void store_char(std::byte* p, char c) noexcept { *p = static_cast<std::byte>(c); }

// Left-justified, space-padded alpha field.
inline void store_alpha(std::byte* p, std::string_view s, std::size_t width) noexcept {
    std::size_t n = s.size() < width ? s.size() : width;
    std::memcpy(p, s.data(), n);
    std::memset(p + n, ' ', width - n);
}


using Clock = std::chrono::steady_clock;

inline std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

inline constexpr std::int64_t kNsPerDay = 86'400'000'000'000;

// steady_ns + offset gives nanoseconds since UTC midnight; computed once at startup.
inline std::int64_t midnight_offset_ns() noexcept {
    auto wall = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
    return (wall % kNsPerDay) - now_ns();
}

inline std::uint64_t since_midnight(std::int64_t steady_ns, std::int64_t offset) noexcept {
    std::int64_t t = (steady_ns + offset) % kNsPerDay;
    return static_cast<std::uint64_t>(t < 0 ? t + kNsPerDay : t);
}


using SessionId = std::uint16_t;

inline constexpr std::uint32_t kTickSize = 100;  // price units per tick ($0.01)
inline constexpr std::uint32_t kMaxTick = 100'000;
inline constexpr std::uint32_t kMinShares = 1;
inline constexpr std::uint32_t kMaxShares = 1'000'000;
inline constexpr std::size_t kTokenLen = 14;

struct Token {
    std::array<char, kTokenLen> b{};

    friend bool operator==(const Token&, const Token&) = default;

    static Token from(std::string_view s) noexcept {
        Token t;
        t.b.fill(' ');
        std::memcpy(t.b.data(), s.data(), s.size() < kTokenLen ? s.size() : kTokenLen);
        return t;
    }
    void load(const std::byte* p) noexcept { std::memcpy(b.data(), p, kTokenLen); }
    void store(std::byte* p) const noexcept { std::memcpy(p, b.data(), kTokenLen); }
    std::string_view view() const noexcept { return {b.data(), kTokenLen}; }
};


inline constexpr std::size_t kNumSymbols = 8;

inline constexpr std::array<std::string_view, kNumSymbols> kSymbolNames = {
    "AAPL", "MSFT", "NVDA", "AMZN", "GOOGL", "META", "TSLA", "SPY"};

// The 8-byte space-padded stock field read big-endian, so the key is the same on any host.
inline constexpr std::uint64_t symbol_key(std::string_view s) noexcept {
    std::uint64_t k = 0;
    for (std::size_t i = 0; i < 8; ++i) k = (k << 8) | static_cast<unsigned char>(i < s.size() ? s[i] : ' ');
    return k;
}

inline constexpr std::array<std::uint64_t, kNumSymbols> kSymbolKeys = [] {
    std::array<std::uint64_t, kNumSymbols> a{};
    for (std::size_t i = 0; i < kNumSymbols; ++i) a[i] = symbol_key(kSymbolNames[i]);
    return a;
}();

// Locate codes are 1-based; 0 means unknown.
inline std::uint8_t locate_of(std::uint64_t key) noexcept {
    for (std::size_t i = 0; i < kNumSymbols; ++i)
        if (kSymbolKeys[i] == key) return static_cast<std::uint8_t>(i + 1);
    return 0;
}

inline std::uint64_t key_of_locate(std::uint8_t locate) noexcept { return kSymbolKeys[locate - 1]; }


inline constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ull;
inline constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;

inline std::uint64_t fnv_mix(std::uint64_t h, std::uint64_t v, int bytes) noexcept {
    for (int i = 0; i < bytes; ++i) {
        h ^= (v >> (8 * i)) & 0xff;
        h *= kFnvPrime;
    }
    return h;
}

inline std::uint64_t checksum_order(std::uint64_t h, std::uint8_t locate, char side, std::uint32_t tick,
                                    std::uint32_t shares, std::uint64_t ref) noexcept {
    h = fnv_mix(h, locate, 1);
    h = fnv_mix(h, static_cast<unsigned char>(side), 1);
    h = fnv_mix(h, tick, 4);
    h = fnv_mix(h, shares, 4);
    return fnv_mix(h, ref, 8);
}

}  // namespace lle
