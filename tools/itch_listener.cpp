#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

#include "lle/common.hpp"
#include "lle/flat_map.hpp"
#include "lle/itch.hpp"
#include "lle/moldudp64.hpp"
#include "lle/order_book.hpp"
#include "lle/spsc_ring.hpp"

using namespace lle;

namespace {

struct Options {
    std::uint16_t port = 16000;
    std::string group;
    double timeout = 10.0;
    std::string out;
};

// Books rebuilt purely from ITCH, on the same level-array structure the engine uses. Level FIFOs
// follow message order, which is the exchange's priority order, so the checksum is comparable.
class Books {
public:
    static constexpr std::uint32_t kCapacity = 1u << 22;

    Books()
        : books_(new OrderBook[kNumSymbols]),
          pool_(new Order[kCapacity]()),
          free_(new std::uint32_t[kCapacity]),
          free_top_(kCapacity),
          refs_(std::size_t{kCapacity} * 2) {
        for (std::uint32_t i = 0; i < kCapacity; ++i) free_[i] = kCapacity - 1 - i;
    }

    void add(std::uint64_t ref, std::uint8_t loc, char side, std::uint32_t price, std::uint32_t shares) {
        if (loc == 0 || loc > kNumSymbols || (side != 'B' && side != 'S') || free_top_ == 0) return;
        std::uint32_t tick = price / kTickSize;
        if (tick == 0 || tick > kMaxTick || refs_.find(ref) != nullptr) return;
        std::uint32_t idx = free_[--free_top_];
        Order& o = pool_[idx];
        o.ref = ref;
        o.open = shares;
        o.tick = tick;
        o.side = side;
        o.locate = loc;
        books_[loc - 1].append(pool_.get(), idx);
        refs_.insert(ref, idx);
    }

    void reduce(std::uint64_t ref, std::uint32_t shares) {
        const std::uint32_t* idx = refs_.find(ref);
        if (idx == nullptr) return;
        Order& o = pool_[*idx];
        if (o.open <= shares) remove(ref); else books_[o.locate - 1].reduce(o, shares);
    }

    void remove(std::uint64_t ref) {
        const std::uint32_t* p = refs_.find(ref);
        if (p == nullptr) return;
        std::uint32_t idx = *p;
        books_[pool_[idx].locate - 1].unlink(pool_.get(), idx);
        refs_.erase(ref);
        free_[free_top_++] = idx;
    }

    void replace(std::uint64_t old_ref, std::uint64_t new_ref, std::uint32_t price, std::uint32_t shares) {
        const std::uint32_t* idx = refs_.find(old_ref);
        if (idx == nullptr) return;
        std::uint8_t loc = pool_[*idx].locate;
        char side = pool_[*idx].side;
        remove(old_ref);
        add(new_ref, loc, side, price, shares);
    }

    std::uint64_t checksum() const {
        std::uint64_t h = kFnvOffset;
        for (std::size_t s = 0; s < kNumSymbols; ++s)
            books_[s].for_each_resting(pool_.get(), [&](const Order& o) {
                h = checksum_order(h, o.locate, o.side, o.tick, o.open, o.ref);
            });
        return h;
    }

    std::string top_of_book() const {
        std::string s = "{";
        for (std::size_t i = 0; i < kNumSymbols; ++i) {
            const OrderBook& b = books_[i];
            std::uint32_t bid = b.best_bid(), ask = b.best_ask();
            std::uint64_t bid_sz = bid != OrderBook::kNoBid ? b.level('B', bid).total_shares : 0;
            std::uint64_t ask_sz = ask != OrderBook::kNoAsk ? b.level('S', ask).total_shares : 0;
            char buf[192];
            std::snprintf(buf, sizeof buf,
                          "%s\"%.*s\": {\"bid\": %u, \"bid_size\": %llu, \"ask\": %u, \"ask_size\": %llu}",
                          i ? ", " : "", static_cast<int>(kSymbolNames[i].size()), kSymbolNames[i].data(),
                          bid * kTickSize, static_cast<unsigned long long>(bid_sz),
                          ask != OrderBook::kNoAsk ? ask * kTickSize : 0, static_cast<unsigned long long>(ask_sz));
            s += buf;
        }
        return s + "}";
    }

    std::size_t size() const { return refs_.size(); }

private:
    std::unique_ptr<OrderBook[]> books_;
    std::unique_ptr<Order[]> pool_;
    std::unique_ptr<std::uint32_t[]> free_;
    std::uint32_t free_top_;
    FlatMap<std::uint64_t, std::uint32_t> refs_;
};

struct Packet {
    std::uint32_t len;
    std::byte data[2048];
};
using PacketRing = SpscRing<Packet, 1 << 14>;

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
    auto books_ptr = std::make_unique<Books>();
    Books& books = *books_ptr;
    std::uint64_t expected = 1, gaps = 0, messages = 0, packets = 0, heartbeats = 0;
    std::uint64_t by_type[128] = {};
    bool ended = false;

    // The receive thread only drains the socket into a ring; book building happens here. At
    // full exchange throughput one thread doing both falls behind and the kernel drops packets.
    auto ring = std::make_unique<PacketRing>();
    std::atomic<bool> receiver_done{false}, timed_out{false}, stop{false};
    std::thread receiver([&] {
        make_thread_hot(-1);
        std::int64_t last_packet = now_ns();
        while (!stop.load(std::memory_order_relaxed)) {
            Packet* p;
            while ((p = ring->claim()) == nullptr) cpu_relax();
            ssize_t n = ::recv(fd, p->data, sizeof p->data, 0);
            if (n < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) break;
                if (static_cast<double>(now_ns() - last_packet) * 1e-9 >= opt.timeout) {
                    timed_out = true;
                    break;
                }
                continue;
            }
            last_packet = now_ns();
            p->len = static_cast<std::uint32_t>(n);
            ring->commit();
        }
        receiver_done.store(true, std::memory_order_release);
    });

    for (;;) {
        Packet* p = ring->peek();
        if (p == nullptr) {
            if (receiver_done.load(std::memory_order_acquire) && ring->peek() == nullptr) break;
            cpu_relax();
            continue;
        }
        ++packets;
        mold::PacketHeader h;
        bool ok = mold::parse(p->data, p->len, h, [&](std::uint64_t seq, const std::byte* m, std::size_t len) {
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
        ring->release();
        if (!ok) continue;
        if (h.sequence > expected) gaps += h.sequence - expected;
        if (h.count == mold::kEndOfSession) {
            ended = true;
            break;
        }
        if (h.count == 0) {
            ++heartbeats;
            if (h.sequence > expected) expected = h.sequence;
        } else if (h.sequence + h.count > expected) {
            expected = h.sequence + h.count;
        }
    }
    stop = true;
    receiver.join();
    ::close(fd);

    char head[512];
    std::snprintf(head, sizeof head,
                  "{\"port\": %u, \"messages\": %llu, \"packets\": %llu, \"heartbeats\": %llu, \"gaps\": %llu, "
                  "\"end_of_session\": %s, \"by_type\": {\"S\": %llu, \"R\": %llu, \"A\": %llu, \"E\": %llu, "
                  "\"X\": %llu, \"D\": %llu, \"U\": %llu}, \"resting_orders\": %zu, \"book_checksum\": \"%016llx\", ",
                  opt.port, static_cast<unsigned long long>(messages), static_cast<unsigned long long>(packets),
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
    return timed_out.load() ? 1 : 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "itch_listener: %s\n", e.what());
    return 1;
}
