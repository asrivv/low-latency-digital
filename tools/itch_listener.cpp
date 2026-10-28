#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <unordered_map>

#include "lle/common.hpp"
#include "lle/itch.hpp"
#include "lle/moldudp64.hpp"

using namespace lle;

namespace {

struct Options {
    std::uint16_t port = 16000;
    std::string group;
    double timeout = 10.0;
    std::string out;
};

// Book rebuilt purely from ITCH. Level FIFOs follow message order, which is the exchange's
// priority order, so the checksum can be compared with the engine's.
class Books {
public:
    void add(std::uint64_t ref, std::uint8_t loc, char side, std::uint32_t price, std::uint32_t shares) {
        if (loc == 0 || loc > kNumSymbols) return;
        std::uint32_t tick = price / kTickSize;
        auto& q = level(loc, side, tick);
        q.push_back(ref);
        orders_[ref] = Order{loc, side, tick, shares, std::prev(q.end())};
    }

    void reduce(std::uint64_t ref, std::uint32_t shares) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) return;
        if (it->second.shares <= shares) erase(it); else it->second.shares -= shares;
    }

    void remove(std::uint64_t ref) {
        auto it = orders_.find(ref);
        if (it != orders_.end()) erase(it);
    }

    void replace(std::uint64_t old_ref, std::uint64_t new_ref, std::uint32_t price, std::uint32_t shares) {
        auto it = orders_.find(old_ref);
        if (it == orders_.end()) return;
        std::uint8_t loc = it->second.loc;
        char side = it->second.side;
        erase(it);
        add(new_ref, loc, side, price, shares);
    }

    std::uint64_t checksum() const {
        std::uint64_t h = kFnvOffset;
        for (std::size_t s = 0; s < kNumSymbols; ++s) {
            for (auto lvl = bids_[s].rbegin(); lvl != bids_[s].rend(); ++lvl)
                for (std::uint64_t ref : lvl->second) h = mix(h, ref);
            for (const auto& [tick, q] : asks_[s])
                for (std::uint64_t ref : q) h = mix(h, ref);
        }
        return h;
    }

    std::string top_of_book() const {
        std::string s = "{";
        for (std::size_t i = 0; i < kNumSymbols; ++i) {
            char buf[192];
            std::uint64_t bid_sz = 0, ask_sz = 0;
            std::uint32_t bid = 0, ask = 0;
            if (!bids_[i].empty()) {
                auto lvl = bids_[i].rbegin();
                bid = lvl->first * kTickSize;
                for (std::uint64_t r : lvl->second) bid_sz += orders_.at(r).shares;
            }
            if (!asks_[i].empty()) {
                auto lvl = asks_[i].begin();
                ask = lvl->first * kTickSize;
                for (std::uint64_t r : lvl->second) ask_sz += orders_.at(r).shares;
            }
            std::snprintf(buf, sizeof buf, "%s\"%.*s\": {\"bid\": %u, \"bid_size\": %llu, \"ask\": %u, \"ask_size\": %llu}",
                          i ? ", " : "", static_cast<int>(kSymbolNames[i].size()), kSymbolNames[i].data(), bid,
                          static_cast<unsigned long long>(bid_sz), ask, static_cast<unsigned long long>(ask_sz));
            s += buf;
        }
        return s + "}";
    }

    std::size_t size() const { return orders_.size(); }

private:
    using Queue = std::list<std::uint64_t>;
    struct Order {
        std::uint8_t loc;
        char side;
        std::uint32_t tick;
        std::uint32_t shares;
        Queue::iterator it;
    };

    Queue& level(std::uint8_t loc, char side, std::uint32_t tick) {
        return side == 'B' ? bids_[loc - 1][tick] : asks_[loc - 1][tick];
    }

    void erase(std::unordered_map<std::uint64_t, Order>::iterator it) {
        const Order& o = it->second;
        auto& side = o.side == 'B' ? bids_[o.loc - 1] : asks_[o.loc - 1];
        auto lvl = side.find(o.tick);
        lvl->second.erase(o.it);
        if (lvl->second.empty()) side.erase(lvl);
        orders_.erase(it);
    }

    std::uint64_t mix(std::uint64_t h, std::uint64_t ref) const {
        const Order& o = orders_.at(ref);
        return checksum_order(h, o.loc, o.side, o.tick, o.shares, ref);
    }

    std::map<std::uint32_t, Queue> bids_[kNumSymbols];
    std::map<std::uint32_t, Queue> asks_[kNumSymbols];
    std::unordered_map<std::uint64_t, Order> orders_;
};

int open_socket(const Options& o) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) throw std::runtime_error("socket");
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#if defined(SO_REUSEPORT)
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    // macOS caps socket buffers at kern.ipc.maxsockbuf including overhead; step down until accepted.
    for (int sz = 8 * 1024 * 1024; sz >= 256 * 1024; sz -= 256 * 1024)
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz) == 0) break;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(o.port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0)
        throw std::runtime_error(std::string("bind: ") + std::strerror(errno));
    if (!o.group.empty()) {
        ip_mreq m{};
        if (inet_pton(AF_INET, o.group.c_str(), &m.imr_multiaddr) != 1) throw std::runtime_error("bad group");
        m.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) != 0)
            throw std::runtime_error("IP_ADD_MEMBERSHIP failed");
    }
    timeval tv{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        if (i + 1 >= argc) throw std::runtime_error("missing value for " + k);
        std::string v = argv[++i];
        if (k == "--port") o.port = static_cast<std::uint16_t>(std::stoul(v));
        else if (k == "--group") o.group = v;
        else if (k == "--timeout") o.timeout = std::stod(v);
        else if (k == "--out") o.out = v;
        else throw std::runtime_error("unknown flag " + k);
    }
    return o;
}

}  // namespace

int main(int argc, char** argv) try {
    Options opt = parse_args(argc, argv);
    int fd = open_socket(opt);
    Books books;
    std::uint64_t expected = 1, gaps = 0, messages = 0, packets = 0, heartbeats = 0;
    std::uint64_t by_type[128] = {};
    bool ended = false, timed_out = false;
    std::int64_t last_packet = now_ns();
    alignas(8) std::byte buf[65536];

    while (!ended) {
        ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                throw std::runtime_error(std::string("recv: ") + std::strerror(errno));
            if (static_cast<double>(now_ns() - last_packet) * 1e-9 >= opt.timeout) {
                timed_out = true;
                break;
            }
            continue;
        }
        last_packet = now_ns();
        ++packets;
        mold::PacketHeader h;
        bool ok = mold::parse(buf, static_cast<std::size_t>(n), h, [&](std::uint64_t seq, const std::byte* m,
                                                                      std::size_t len) {
            if (seq < expected || len == 0) return;
            char type = static_cast<char>(m[0]);
            if (len != itch::message_size(type)) return;
            ++messages;
            ++by_type[static_cast<unsigned char>(type) & 127];
            switch (type) {
                case 'A': {
                    auto a = itch::decode_add(m);
                    books.add(a.ref, static_cast<std::uint8_t>(a.locate), a.side, a.price, a.shares);
                    break;
                }
                case 'E': {
                    auto e = itch::decode_executed(m);
                    books.reduce(e.ref, e.shares);
                    break;
                }
                case 'X': {
                    auto x = itch::decode_cancel(m);
                    books.reduce(x.ref, x.shares);
                    break;
                }
                case 'D': books.remove(itch::decode_delete(m).ref); break;
                case 'U': {
                    auto u = itch::decode_replace(m);
                    books.replace(u.orig_ref, u.new_ref, u.price, u.shares);
                    break;
                }
                default: break;
            }
        });
        if (!ok) continue;
        if (h.sequence > expected) gaps += h.sequence - expected;
        if (h.count == mold::kEndOfSession) {
            ended = true;
        } else if (h.count == 0) {
            ++heartbeats;
            if (h.sequence > expected) expected = h.sequence;
        } else if (h.sequence + h.count > expected) {
            expected = h.sequence + h.count;
        }
    }
    ::close(fd);

    char head[512];
    std::snprintf(head, sizeof head,
                  "{\"messages\": %llu, \"packets\": %llu, \"heartbeats\": %llu, \"gaps\": %llu, "
                  "\"end_of_session\": %s, \"by_type\": {\"S\": %llu, \"R\": %llu, \"A\": %llu, \"E\": %llu, "
                  "\"X\": %llu, \"D\": %llu, \"U\": %llu}, \"resting_orders\": %zu, \"book_checksum\": \"%016llx\", ",
                  static_cast<unsigned long long>(messages), static_cast<unsigned long long>(packets),
                  static_cast<unsigned long long>(heartbeats), static_cast<unsigned long long>(gaps),
                  ended ? "true" : "false", static_cast<unsigned long long>(by_type[static_cast<unsigned char>('S')]),
                  static_cast<unsigned long long>(by_type[static_cast<unsigned char>('R')]), static_cast<unsigned long long>(by_type[static_cast<unsigned char>('A')]),
                  static_cast<unsigned long long>(by_type[static_cast<unsigned char>('E')]), static_cast<unsigned long long>(by_type[static_cast<unsigned char>('X')]),
                  static_cast<unsigned long long>(by_type[static_cast<unsigned char>('D')]), static_cast<unsigned long long>(by_type[static_cast<unsigned char>('U')]),
                  books.size(), static_cast<unsigned long long>(books.checksum()));
    std::string line = std::string(head) + "\"top_of_book\": " + books.top_of_book() + "}";
    std::puts(line.c_str());
    std::fflush(stdout);
    if (!opt.out.empty()) {
        std::FILE* f = std::fopen(opt.out.c_str(), "w");
        if (f == nullptr) throw std::runtime_error("cannot write " + opt.out);
        std::fprintf(f, "%s\n", line.c_str());
        std::fclose(f);
    }
    return timed_out ? 1 : 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "itch_listener: %s\n", e.what());
    return 1;
}
