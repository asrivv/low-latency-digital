#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "lle/common.hpp"
#include "lle/engine.hpp"
#include "lle/itch.hpp"
#include "lle/ouch.hpp"
#include "lle/spsc_ring.hpp"

namespace lle {

inline constexpr std::size_t kRingSlots = 1 << 16;

struct alignas(128) OutMsg {
    std::int64_t t_origin;
    std::uint64_t cmd_seq;
    SessionId session;
    std::uint8_t len;
    std::byte data[ouch::kMaxOutboundSize];
};
static_assert(sizeof(OutMsg) == 128);

struct alignas(128) MdMsg {
    std::uint8_t len;
    std::byte data[itch::kMaxMessageSize];
};
static_assert(sizeof(MdMsg) == 128);

using InboundRing = SpscRing<Command, kRingSlots>;
using OutboundRing = SpscRing<OutMsg, kRingSlots>;
using MdRing = SpscRing<MdMsg, kRingSlots>;

// Decodes one OUCH message from a SoupBin Unsequenced Data payload. False for an unknown
// type or a length that does not match it.
bool decode_command(const std::byte* p, std::size_t n, SessionId session, std::uint64_t ts, Command& out) noexcept;

// Engine sink that encodes into the outbound and market data rings, spinning while full.
class RingSink {
public:
    RingSink(OutboundRing& out, MdRing& md) noexcept : out_(out), md_(md) {}

    void begin(const Command& c) noexcept {
        t_origin_ = c.t_decode;
        seq_ = c.seq;
    }

    void ouch_accepted(SessionId s, const ouch::Accepted& m) noexcept { put_ouch(s, m); }
    void ouch_executed(SessionId s, const ouch::Executed& m) noexcept { put_ouch(s, m); }
    void ouch_canceled(SessionId s, const ouch::Canceled& m) noexcept { put_ouch(s, m); }
    void ouch_replaced(SessionId s, const ouch::Replaced& m) noexcept { put_ouch(s, m); }
    void ouch_rejected(SessionId s, const ouch::Rejected& m) noexcept { put_ouch(s, m); }
    void itch_add(const itch::AddOrder& m) noexcept { put_itch(m); }
    void itch_executed(const itch::OrderExecuted& m) noexcept { put_itch(m); }
    void itch_cancel(const itch::OrderCancel& m) noexcept { put_itch(m); }
    void itch_delete(const itch::OrderDelete& m) noexcept { put_itch(m); }
    void itch_replace(const itch::OrderReplace& m) noexcept { put_itch(m); }

    template <class M>
    void put_itch(const M& m) noexcept {
        MdMsg* slot;
        while ((slot = md_.claim()) == nullptr) cpu_relax();
        slot->len = static_cast<std::uint8_t>(itch::encode(m, slot->data));
        md_.commit();
        ++itch_out_;
    }

    std::uint64_t ouch_out() const noexcept { return ouch_out_; }
    std::uint64_t itch_out() const noexcept { return itch_out_; }

private:
    template <class M>
    void put_ouch(SessionId s, const M& m) noexcept {
        OutMsg* slot;
        while ((slot = out_.claim()) == nullptr) cpu_relax();
        slot->t_origin = t_origin_;
        slot->cmd_seq = seq_;
        slot->session = s;
        slot->len = static_cast<std::uint8_t>(ouch::encode(m, slot->data));
        out_.commit();
        ++ouch_out_;
    }

    OutboundRing& out_;
    MdRing& md_;
    std::int64_t t_origin_ = 0;
    std::uint64_t seq_ = 0;
    std::uint64_t ouch_out_ = 0;
    std::uint64_t itch_out_ = 0;
};

struct GatewayStats {
    std::uint64_t sessions_accepted = 0;
    std::uint64_t commands = 0;
    std::uint64_t frames_out = 0;
    std::uint64_t bytes_out = 0;
    std::uint64_t dropped_out = 0;
};

// OUCH over SoupBinTCP. Single-threaded busy-poll: accept, read and decode into the inbound
// ring, drain the outbound ring into per-session send buffers, one send per session per pass.
class Gateway {
public:
    static constexpr std::size_t kMaxSessions = 64;
    static constexpr std::size_t kRecvBuffer = 64 * 1024;
    static constexpr std::size_t kSendBuffer = 2 * 1024 * 1024;
    static constexpr int kSocketBuffer = 4 * 1024 * 1024;
    static constexpr std::int64_t kHeartbeatNs = 1'000'000'000;
    static constexpr std::int64_t kIdleTimeoutNs = 15'000'000'000;

    Gateway(InboundRing& in, OutboundRing& out);
    ~Gateway();
    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;

    bool listen(std::uint16_t port) noexcept;
    std::uint16_t port() const noexcept { return port_; }

    void poll() noexcept;
    void stop_input() noexcept { input_ = false; }
    // Drains the outbound ring, sends End of Session to every session and closes everything.
    void finish() noexcept;

    const GatewayStats& stats() const noexcept { return stats_; }

private:
    struct Session {
        int fd = -1;
        SessionId id = 0;
        bool logged_in = false;
        bool closing = false;
        std::size_t rlen = 0;
        std::size_t soff = 0;
        std::size_t slen = 0;
        std::int64_t last_recv = 0;
        std::int64_t last_send = 0;
        std::unique_ptr<std::byte[]> rbuf;
        std::unique_ptr<std::byte[]> sbuf;
    };

    void accept_new(std::int64_t now) noexcept;
    void read_session(Session& s, std::int64_t now) noexcept;
    bool on_frame(Session& s, char type, const std::byte* p, std::size_t n, std::int64_t now) noexcept;
    bool drain_outbound(std::int64_t now) noexcept;
    std::byte* reserve(Session& s, std::size_t n) noexcept;
    void flush(Session& s, std::int64_t now) noexcept;
    void close_session(Session& s) noexcept;

    InboundRing& in_;
    OutboundRing& out_;
    std::array<Session, kMaxSessions> sessions_;
    std::array<std::uint16_t, kMaxSessions> generation_{};
    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    bool input_ = true;
    std::uint64_t iteration_ = 0;
    std::uint64_t next_seq_ = 1;
    std::int64_t midnight_offset_;
    GatewayStats stats_;
};

}  // namespace lle
