#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "gateway.hpp"
#include "lle/engine.hpp"
#include "lle/itch.hpp"
#include "lle/ouch.hpp"
#include "lle/soupbin.hpp"

using namespace lle;
using Bytes = std::vector<std::byte>;

namespace {

class LoopbackExchange {
public:
    LoopbackExchange()
        : in_(std::make_unique<InboundRing>()),
          out_(std::make_unique<OutboundRing>()),
          md_(std::make_unique<MdRing>()),
          sink_(*out_, *md_),
          engine_(std::make_unique<Engine<RingSink>>(sink_, 1 << 12)),
          gateway_(std::make_unique<Gateway>(*in_, *out_)) {
        EXPECT_TRUE(gateway_->listen(0));
        gw_ = std::thread([this] {
            while (!stop_.load()) gateway_->poll();
            gateway_->finish();
        });
        eng_ = std::thread([this] {
            while (!stop_.load()) {
                Command* c = in_->peek();
                if (c == nullptr) {
                    cpu_relax();
                    continue;
                }
                sink_.begin(*c);
                engine_->on_command(*c);
                in_->release();
            }
        });
    }
    ~LoopbackExchange() {
        stop_ = true;
        gw_.join();
        eng_.join();
    }
    std::uint16_t port() const { return gateway_->port(); }

    std::vector<std::string> itch_messages() {
        std::vector<std::string> v;
        while (MdMsg* m = md_->peek()) {
            v.emplace_back(reinterpret_cast<const char*>(m->data), m->len);
            md_->release();
        }
        return v;
    }

private:
    std::unique_ptr<InboundRing> in_;
    std::unique_ptr<OutboundRing> out_;
    std::unique_ptr<MdRing> md_;
    RingSink sink_;
    std::unique_ptr<Engine<RingSink>> engine_;
    std::unique_ptr<Gateway> gateway_;
    std::atomic<bool> stop_{false};
    std::thread gw_, eng_;
};

class Client {
public:
    explicit Client(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        EXPECT_EQ(::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
        int one = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    ~Client() { ::close(fd_); }

    void send(const Bytes& b) { ASSERT_EQ(::send(fd_, b.data(), b.size(), 0), static_cast<ssize_t>(b.size())); }

    // Exactly n bytes, or fewer if the peer closes or the timeout expires.
    Bytes read(std::size_t n, int timeout_ms = 2000) {
        Bytes b(n);
        std::size_t got = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (got < n) {
            int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            deadline - std::chrono::steady_clock::now()).count());
            pollfd p{fd_, POLLIN, 0};
            if (left <= 0 || ::poll(&p, 1, left) <= 0) break;
            ssize_t r = ::recv(fd_, b.data() + got, n - got, 0);
            if (r <= 0) break;
            got += static_cast<std::size_t>(r);
        }
        b.resize(got);
        return b;
    }

    void login() {
        Bytes b(64);
        b.resize(soupbin::encode_login_request(b.data(), {"test", "pw", "", 0}));
        send(b);
        Bytes got = read(33);
        Bytes want(33);
        soupbin::encode_login_accepted(want.data(), "LLE0000001", 1);
        ASSERT_EQ(got, want);
    }

private:
    int fd_;
};

Bytes frame_u(const ouch::EnterOrder& m) {
    std::byte msg[64];
    std::size_t n = ouch::encode(m, msg);
    Bytes b(n + 3);
    soupbin::encode_frame(b.data(), 'U', msg, n);
    return b;
}

ouch::EnterOrder order(const char* tok, char side, std::uint32_t shares, std::uint32_t price) {
    return {Token::from(tok), side, shares, symbol_key("AAPL"), price, 99'999, 0x46524d31, 'Y', 'A', 'N', 0, 'N', 'R'};
}

// Builds the expected S frame for `m`, taking the timestamp from the frame actually received.
template <class M>
Bytes expect_frame(M m, const Bytes& got) {
    EXPECT_GE(got.size(), 12u);
    m.timestamp = load_be64(got.data() + 4);
    EXPECT_LT(m.timestamp, static_cast<std::uint64_t>(kNsPerDay));
    std::byte msg[128];
    std::size_t n = ouch::encode(m, msg);
    Bytes b(n + 3);
    soupbin::encode_frame(b.data(), 'S', msg, n);
    return b;
}

}  // namespace

TEST(Gateway, LoginEnterCrossAcrossSessions) {
    LoopbackExchange ex;
    Client a(ex.port()), b(ex.port());
    a.login();
    b.login();

    auto buy = order("BUY1", 'B', 100, 1'000'000);
    a.send(frame_u(buy));
    Bytes got = a.read(3 + 66);
    EXPECT_EQ(got, expect_frame(ouch::Accepted{0, buy.token, 'B', 100, buy.stock, 1'000'000, 99'999, buy.firm, 'Y', 1,
                                               'A', 'N', 0, 'N', 'L', '0'},
                                got));

    auto sell = order("SELL1", 'S', 300, 1'000'000);
    b.send(frame_u(sell));
    got = b.read(3 + 66);
    EXPECT_EQ(got, expect_frame(ouch::Accepted{0, sell.token, 'S', 300, sell.stock, 1'000'000, 99'999, sell.firm, 'Y',
                                               2, 'A', 'N', 0, 'N', 'L', '0'},
                                got));
    got = b.read(3 + 40);
    EXPECT_EQ(got, expect_frame(ouch::Executed{0, sell.token, 100, 1'000'000, 'R', 1}, got));
    got = a.read(3 + 40);
    EXPECT_EQ(got, expect_frame(ouch::Executed{0, buy.token, 100, 1'000'000, 'A', 1}, got));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto md = ex.itch_messages();
    ASSERT_EQ(md.size(), 3u);
    auto add1 = itch::decode_add(reinterpret_cast<const std::byte*>(md[0].data()));
    EXPECT_EQ(add1.ref, 1u);
    EXPECT_EQ(add1.side, 'B');
    auto exe = itch::decode_executed(reinterpret_cast<const std::byte*>(md[1].data()));
    EXPECT_EQ(exe.ref, 1u);
    EXPECT_EQ(exe.shares, 100u);
    EXPECT_EQ(exe.match, 1u);
    auto add2 = itch::decode_add(reinterpret_cast<const std::byte*>(md[2].data()));
    EXPECT_EQ(add2.ref, 2u);
    EXPECT_EQ(add2.shares, 200u);
    EXPECT_EQ(add2.side, 'S');
}

TEST(Gateway, FrameSplitAcrossWritesAndBatchedFrames) {
    LoopbackExchange ex;
    Client a(ex.port());
    a.login();
    Bytes f = frame_u(order("SPLIT", 'B', 100, 900'000));
    for (std::byte x : f) {
        a.send(Bytes{x});
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Bytes got = a.read(3 + 66);
    ASSERT_EQ(got.size(), 69u);
    EXPECT_EQ(static_cast<char>(got[3]), 'A');

    Bytes batch;
    for (int i = 0; i < 50; ++i) {
        Bytes one = frame_u(order(("B" + std::to_string(i)).c_str(), 'B', 100, 800'000));
        batch.insert(batch.end(), one.begin(), one.end());
    }
    a.send(batch);
    got = a.read(50 * 69);
    ASSERT_EQ(got.size(), 50u * 69);
    for (int i = 0; i < 50; ++i) {
        auto acc = ouch::decode_accepted(got.data() + i * 69 + 3);
        EXPECT_EQ(acc.token, Token::from("B" + std::to_string(i)));
        EXPECT_EQ(acc.ref, static_cast<std::uint64_t>(i + 2));
    }
}

TEST(Gateway, RejectAndIgnoreUnknownMessage) {
    LoopbackExchange ex;
    Client a(ex.port());
    a.login();
    Bytes junk{std::byte{0}, std::byte{3}, std::byte{'U'}, std::byte{'Q'}, std::byte{0}};
    a.send(junk);
    auto bad = order("BAD", 'B', 100, 1'000'050);
    a.send(frame_u(bad));
    Bytes got = a.read(3 + 24);
    EXPECT_EQ(got, expect_frame(ouch::Rejected{0, bad.token, 'X'}, got));
}

TEST(Gateway, ServerHeartbeatAndLogout) {
    LoopbackExchange ex;
    Client a(ex.port());
    a.login();
    Bytes hb = a.read(3, 2500);
    EXPECT_EQ(hb, (Bytes{std::byte{0}, std::byte{1}, std::byte{'H'}}));
    Bytes logout(3);
    soupbin::encode_empty(logout.data(), 'O');
    a.send(logout);
    EXPECT_TRUE(a.read(1, 2000).empty());
}

TEST(Gateway, MustLoginFirst) {
    LoopbackExchange ex;
    Client a(ex.port());
    a.send(frame_u(order("EARLY", 'B', 100, 1'000'000)));
    EXPECT_TRUE(a.read(1, 2000).empty());
}

TEST(Gateway, EndOfSessionOnFinish) {
    auto ex = std::make_unique<LoopbackExchange>();
    Client a(ex->port());
    a.login();
    ex.reset();
    Bytes z = a.read(3, 3000);
    EXPECT_EQ(z, (Bytes{std::byte{0}, std::byte{1}, std::byte{'Z'}}));
}
