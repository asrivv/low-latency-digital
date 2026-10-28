#include "gateway.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "lle/soupbin.hpp"

namespace lle {

namespace {

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

void set_nonblocking(int fd) noexcept { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }

void tune_stream(int fd) noexcept {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#if defined(SO_NOSIGPIPE)
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    int sz = Gateway::kSocketBuffer;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz);
}

}  // namespace

bool decode_command(const std::byte* p, std::size_t n, SessionId session, std::uint64_t ts, Command& out) noexcept {
    if (n == 0) return false;
    char type = static_cast<char>(p[0]);
    if (n != ouch::inbound_size(type)) return false;
    switch (type) {
        case 'O':
            out.kind = Command::Kind::Enter;
            out.enter.ts = ts;
            out.enter.session = session;
            out.enter.msg = ouch::decode_enter(p);
            return true;
        case 'X':
            out.kind = Command::Kind::Cancel;
            out.cancel.ts = ts;
            out.cancel.session = session;
            out.cancel.msg = ouch::decode_cancel(p);
            return true;
        case 'U':
            out.kind = Command::Kind::Replace;
            out.replace.ts = ts;
            out.replace.session = session;
            out.replace.msg = ouch::decode_replace(p);
            return true;
        default: return false;
    }
}

Gateway::Gateway(InboundRing& in, OutboundRing& out) : in_(in), out_(out), midnight_offset_(midnight_offset_ns()) {
    for (Session& s : sessions_) {
        s.rbuf.reset(new std::byte[kRecvBuffer]);
        s.sbuf.reset(new std::byte[kSendBuffer]);
    }
}

Gateway::~Gateway() {
    for (Session& s : sessions_)
        if (s.fd >= 0) ::close(s.fd);
    if (listen_fd_ >= 0) ::close(listen_fd_);
}

bool Gateway::listen(std::uint16_t port) noexcept {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return false;
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) return false;
    if (::listen(listen_fd_, 64) != 0) return false;
    socklen_t len = sizeof addr;
    getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    set_nonblocking(listen_fd_);
    return true;
}

void Gateway::poll() noexcept {
    std::int64_t now = now_ns();
    // accept() is a syscall that almost always returns EAGAIN; don't pay for it every pass.
    if ((iteration_++ & 255) == 0) accept_new(now);
    if (input_)
        for (Session& s : sessions_)
            if (s.fd >= 0 && !s.closing) read_session(s, now);
    drain_outbound(now);
    for (Session& s : sessions_) {
        if (s.fd < 0) continue;
        if (s.logged_in && s.slen == 0 && now - s.last_send >= kHeartbeatNs) {
            soupbin::encode_empty(reserve(s, soupbin::kHeaderSize), 'H');
            s.last_send = now;
        }
        flush(s, now);
        if (s.fd < 0) continue;
        if ((s.closing && s.slen == 0) || now - s.last_recv >= kIdleTimeoutNs) close_session(s);
    }
}

void Gateway::accept_new(std::int64_t now) noexcept {
    for (;;) {
        int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) return;
        std::size_t slot = 0;
        while (slot < kMaxSessions && sessions_[slot].fd >= 0) ++slot;
        if (slot == kMaxSessions) {
            ::close(fd);
            continue;
        }
        set_nonblocking(fd);
        tune_stream(fd);
        Session& s = sessions_[slot];
        // Session ids carry a per-slot generation so late engine output for a closed
        // connection is never delivered to whoever reuses the slot.
        std::uint16_t gen = generation_[slot] = static_cast<std::uint16_t>((generation_[slot] + 1) & 0x3ff);
        s.fd = fd;
        s.id = static_cast<SessionId>((gen << 6) | slot);
        s.logged_in = false;
        s.closing = false;
        s.rlen = s.soff = s.slen = 0;
        s.last_recv = s.last_send = now;
        ++stats_.sessions_accepted;
    }
}

void Gateway::read_session(Session& s, std::int64_t now) noexcept {
    if (s.rlen < kRecvBuffer) {
        ssize_t n = ::recv(s.fd, s.rbuf.get() + s.rlen, kRecvBuffer - s.rlen, 0);
        if (n > 0) {
            s.rlen += static_cast<std::size_t>(n);
            s.last_recv = now;
        } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            close_session(s);
            return;
        }
    }
    if (s.rlen == 0) return;
    auto r = soupbin::parse(
        s.rbuf.get(), s.rlen,
        [&](char type, const std::byte* p, std::size_t n) { return on_frame(s, type, p, n, now); },
        kRecvBuffer - soupbin::kHeaderSize);
    if (r.error || s.fd < 0) {
        close_session(s);
        return;
    }
    if (r.consumed != 0) {
        s.rlen -= r.consumed;
        if (s.rlen != 0) std::memmove(s.rbuf.get(), s.rbuf.get() + r.consumed, s.rlen);
    }
}

bool Gateway::on_frame(Session& s, char type, const std::byte* p, std::size_t n, std::int64_t now) noexcept {
    if (s.closing) return true;
    if (!s.logged_in) {
        if (type != 'L' || n != soupbin::kLoginRequestPayload) {
            s.closing = true;
            return true;
        }
        // Any credentials are accepted. The requested sequence is ignored: there is no replay,
        // a session always starts at sequence 1.
        s.logged_in = true;
        soupbin::encode_login_accepted(reserve(s, soupbin::kHeaderSize + soupbin::kLoginAcceptedPayload),
                                       soupbin::kSessionName, 1);
        s.last_send = now;
        return true;
    }
    switch (type) {
        case 'U': {
            // A full inbound ring leaves the frame in the receive buffer; blocking here could
            // deadlock with an engine that is itself waiting on the outbound ring we drain.
            Command* c = in_.claim();
            if (c == nullptr) return false;
            std::int64_t t = now_ns();
            if (!decode_command(p, n, s.id, since_midnight(t, midnight_offset_), *c)) return true;
            c->t_decode = t;
            c->seq = next_seq_++;
            in_.commit();
            ++stats_.commands;
            return true;
        }
        case 'O': s.closing = true; return true;
        default: return true;
    }
}

bool Gateway::drain_outbound(std::int64_t now) noexcept {
    while (OutMsg* m = out_.peek()) {
        Session& s = sessions_[m->session & (kMaxSessions - 1)];
        if (s.fd >= 0 && s.id == m->session && s.logged_in) {
            std::byte* p = reserve(s, soupbin::kHeaderSize + m->len);
            if (p == nullptr) return false;
            soupbin::encode_frame(p, 'S', m->data, m->len);
            s.last_send = now;
            ++stats_.frames_out;
        } else {
            ++stats_.dropped_out;
        }
        out_.release();
    }
    return true;
}

// Space for n bytes at the end of the session's send buffer, compacting and then flushing if
// needed. nullptr when the peer is not keeping up; the caller retries on a later pass.
std::byte* Gateway::reserve(Session& s, std::size_t n) noexcept {
    if (s.slen + n > kSendBuffer && s.soff != 0) {
        std::memmove(s.sbuf.get(), s.sbuf.get() + s.soff, s.slen - s.soff);
        s.slen -= s.soff;
        s.soff = 0;
    }
    if (s.slen + n > kSendBuffer) {
        flush(s, now_ns());
        if (s.fd < 0 || s.slen + n > kSendBuffer) return nullptr;
    }
    std::byte* p = s.sbuf.get() + s.slen;
    s.slen += n;
    return p;
}

void Gateway::flush(Session& s, std::int64_t) noexcept {
    if (s.soff == s.slen) return;
    ssize_t n = ::send(s.fd, s.sbuf.get() + s.soff, s.slen - s.soff, kSendFlags);
    if (n > 0) {
        s.soff += static_cast<std::size_t>(n);
        stats_.bytes_out += static_cast<std::uint64_t>(n);
        if (s.soff == s.slen) s.soff = s.slen = 0;
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        close_session(s);
    }
}

void Gateway::close_session(Session& s) noexcept {
    if (s.fd >= 0) ::close(s.fd);
    s.fd = -1;
    s.logged_in = false;
    s.closing = false;
    s.rlen = s.soff = s.slen = 0;
}

void Gateway::finish() noexcept {
    input_ = false;
    std::int64_t deadline = now_ns() + 2'000'000'000;
    for (;;) {
        std::int64_t now = now_ns();
        bool drained = drain_outbound(now);
        bool pending = false;
        for (Session& s : sessions_) {
            if (s.fd < 0) continue;
            flush(s, now);
            pending |= s.fd >= 0 && s.slen != 0;
        }
        if ((drained && out_.empty() && !pending) || now > deadline) break;
        cpu_relax();
    }
    for (Session& s : sessions_) {
        if (s.fd < 0 || !s.logged_in) continue;
        if (std::byte* p = reserve(s, soupbin::kHeaderSize)) soupbin::encode_empty(p, 'Z');
        std::int64_t until = now_ns() + 500'000'000;
        while (s.fd >= 0 && s.slen != 0 && now_ns() < until) flush(s, now_ns());
    }
    for (Session& s : sessions_) close_session(s);
    if (listen_fd_ >= 0) ::close(listen_fd_);
    listen_fd_ = -1;
}

}  // namespace lle
