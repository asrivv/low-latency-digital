#include "md_publisher.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace lle {

MdPublisher::~MdPublisher() {
    if (fd_ >= 0) ::close(fd_);
}

bool MdPublisher::open(const char* addr, std::uint16_t port) noexcept {
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    if (inet_pton(AF_INET, addr, &dst.sin_addr) != 1) return false;
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return false;
    int sz = kSocketBuffer;
    setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
    if (IN_MULTICAST(ntohl(dst.sin_addr.s_addr))) {
        unsigned char ttl = 1, loop = 1;
        setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
        setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
    }
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&dst), sizeof dst) != 0) return false;
    last_send_ = now_ns();
    return true;
}

void MdPublisher::send_packet(const std::byte* p, std::size_t n) noexcept {
    // A UDP send with a full socket buffer fails with ENOBUFS on macOS instead of blocking.
    // Retry rather than drop: a gap here is a gap for every subscriber.
    for (;;) {
        ssize_t r = ::send(fd_, p, n, 0);
        if (r >= 0) break;
        if (errno == ECONNREFUSED) break;
        if (errno != ENOBUFS && errno != EAGAIN && errno != EINTR) break;
        ++stats_.send_retries;
        cpu_relax();
    }
    ++stats_.packets;
    last_send_ = now_ns();
    pacer_.sent(last_send_);
}

void MdPublisher::flush() noexcept {
    if (pk_.empty()) return;
    std::size_t n = pk_.finish();
    send_packet(pk_.data(), n);
}

void MdPublisher::poll() noexcept {
    while (MdMsg* m = ring_.peek()) {
        if (!pk_.fits(m->len)) flush();
        pk_.add(m->data, m->len);
        ++stats_.messages;
        ring_.release();
    }
    std::int64_t now = now_ns();
    if (pk_.empty()) {
        if (now - last_send_ >= kHeartbeatNs) {
            std::byte hb[mold::kHeaderSize];
            send_packet(hb, pk_.heartbeat(hb));
        }
    } else if (pacer_.ready(now)) {
        flush();
    }
}

void MdPublisher::finish() noexcept {
    while (!ring_.empty()) poll();
    flush();
    std::byte eos[mold::kHeaderSize];
    send_packet(eos, pk_.end_of_session(eos));
}

}  // namespace lle
