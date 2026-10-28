#pragma once

#include <cstdint>

#include "gateway.hpp"
#include "lle/moldudp64.hpp"

namespace lle {

struct MdStats {
    std::uint64_t messages = 0;
    std::uint64_t packets = 0;
    std::uint64_t send_retries = 0;
};

// ITCH over MoldUDP64. Packs messages from the market data ring into packets of at most
// mold::kMaxPacket bytes. A full packet goes out at once; a partial one goes out once the ring
// is empty and the pacer allows it. Heartbeats after one second of silence.
class MdPublisher {
public:
    static constexpr std::int64_t kHeartbeatNs = 1'000'000'000;
    static constexpr int kSocketBuffer = 4 * 1024 * 1024;
    static constexpr std::int64_t kDefaultPaceNs = 25'000;
    static constexpr int kPaceBurst = 4;

    explicit MdPublisher(MdRing& ring) noexcept : ring_(ring) {}
    ~MdPublisher();
    MdPublisher(const MdPublisher&) = delete;
    MdPublisher& operator=(const MdPublisher&) = delete;

    bool open(const char* addr, std::uint16_t port) noexcept;
    void set_pace_ns(std::int64_t ns) noexcept { pacer_.set_interval(ns, kPaceBurst); }
    void poll() noexcept;
    // Drains the ring, flushes and sends the end-of-session packet.
    void finish() noexcept;

    const MdStats& stats() const noexcept { return stats_; }

private:
    void send_packet(const std::byte* p, std::size_t n) noexcept;
    void flush() noexcept;

    MdRing& ring_;
    mold::Packetizer pk_;
    int fd_ = -1;
    std::int64_t last_send_ = 0;
    SendPacer pacer_{kDefaultPaceNs, kPaceBurst};
    MdStats stats_;
};

}  // namespace lle
