#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
#include <vector>

#include "lle/itch.hpp"
#include "lle/moldudp64.hpp"
#include "lle/ouch.hpp"

using namespace lle;

namespace {

struct Bytes {
    std::vector<std::byte> v;
    Bytes& c(char x) { v.push_back(static_cast<std::byte>(x)); return *this; }
    Bytes& a(std::string_view s, std::size_t w) {
        for (std::size_t i = 0; i < w; ++i) c(i < s.size() ? s[i] : ' ');
        return *this;
    }
    Bytes& be(std::uint64_t x, int n) {
        for (int i = n - 1; i >= 0; --i) v.push_back(static_cast<std::byte>(x >> (8 * i)));
        return *this;
    }
    Bytes& u16(std::uint64_t x) { return be(x, 2); }
    Bytes& u32(std::uint64_t x) { return be(x, 4); }
    Bytes& u48(std::uint64_t x) { return be(x, 6); }
    Bytes& u64(std::uint64_t x) { return be(x, 8); }
};

std::uint32_t firm(std::string_view s) {
    std::uint32_t f = 0;
    for (char ch : s) f = (f << 8) | static_cast<unsigned char>(ch);
    return f;
}

template <class M>
std::vector<std::byte> enc(const M& m, std::size_t expected) {
    std::vector<std::byte> out(128);
    std::size_t n = ouch::encode(m, out.data());
    EXPECT_EQ(n, expected);
    out.resize(n);
    return out;
}

const Token kTok = Token::from("ABC00000000001");
const Token kTok2 = Token::from("ABC00000000002");

}  // namespace

TEST(Ouch, EnterOrder) {
    ouch::EnterOrder m{kTok, 'B', 300, symbol_key("NVDA"), 1'201'500, 99'999, firm("FIRM"), 'Y', 'A', 'N', 100, 'N', 'R'};
    auto bytes = enc(m, 49);
    Bytes ref;
    ref.c('O').a("ABC00000000001", 14).c('B').u32(300).a("NVDA", 8).u32(1'201'500).u32(99'999).a("FIRM", 4)
        .c('Y').c('A').c('N').u32(100).c('N').c('R');
    EXPECT_EQ(bytes, ref.v);
    auto d = ouch::decode_enter(bytes.data());
    EXPECT_EQ(d.token, m.token);
    EXPECT_EQ(d.side, 'B');
    EXPECT_EQ(d.shares, 300u);
    EXPECT_EQ(d.stock, m.stock);
    EXPECT_EQ(d.price, m.price);
    EXPECT_EQ(d.tif, m.tif);
    EXPECT_EQ(d.firm, m.firm);
    EXPECT_EQ(d.display, 'Y');
    EXPECT_EQ(d.capacity, 'A');
    EXPECT_EQ(d.iso, 'N');
    EXPECT_EQ(d.min_qty, 100u);
    EXPECT_EQ(d.cross_type, 'N');
    EXPECT_EQ(d.customer_type, 'R');
    EXPECT_EQ(ouch::inbound_size('O'), 49u);
}

TEST(Ouch, ReplaceOrder) {
    ouch::ReplaceOrder m{kTok, kTok2, 500, 1'900'100, 0, 'Y', 'N', 0};
    auto bytes = enc(m, 47);
    Bytes ref;
    ref.c('U').a("ABC00000000001", 14).a("ABC00000000002", 14).u32(500).u32(1'900'100).u32(0).c('Y').c('N').u32(0);
    EXPECT_EQ(bytes, ref.v);
    auto d = ouch::decode_replace(bytes.data());
    EXPECT_EQ(d.existing, kTok);
    EXPECT_EQ(d.replacement, kTok2);
    EXPECT_EQ(d.shares, 500u);
    EXPECT_EQ(d.price, 1'900'100u);
    EXPECT_EQ(d.tif, 0u);
    EXPECT_EQ(d.display, 'Y');
    EXPECT_EQ(d.iso, 'N');
    EXPECT_EQ(d.min_qty, 0u);
    EXPECT_EQ(ouch::inbound_size('U'), 47u);
}

TEST(Ouch, CancelOrder) {
    ouch::CancelOrder m{kTok, 200};
    auto bytes = enc(m, 19);
    Bytes ref;
    ref.c('X').a("ABC00000000001", 14).u32(200);
    EXPECT_EQ(bytes, ref.v);
    auto d = ouch::decode_cancel(bytes.data());
    EXPECT_EQ(d.token, kTok);
    EXPECT_EQ(d.shares, 200u);
    EXPECT_EQ(ouch::inbound_size('X'), 19u);
    EXPECT_EQ(ouch::inbound_size('Q'), 0u);
}

TEST(Ouch, SystemEvent) {
    ouch::SystemEvent m{34'200'000'000'123, 'S'};
    auto bytes = enc(m, 10);
    Bytes ref;
    ref.c('S').u64(34'200'000'000'123).c('S');
    EXPECT_EQ(bytes, ref.v);
    auto d = ouch::decode_system_event(bytes.data());
    EXPECT_EQ(d.timestamp, m.timestamp);
    EXPECT_EQ(d.event, 'S');
}

TEST(Ouch, Accepted) {
    ouch::Accepted m{123456789, kTok, 'S', 1000, symbol_key("GOOGL"), 1'650'000, 1, firm("ABCD"), 'Y',
                     0x0102030405060708ull, 'P', 'N', 0, 'N', 'L', '0'};
    auto bytes = enc(m, 66);
    Bytes ref;
    ref.c('A').u64(123456789).a("ABC00000000001", 14).c('S').u32(1000).a("GOOGL", 8).u32(1'650'000).u32(1)
        .a("ABCD", 4).c('Y').u64(0x0102030405060708ull).c('P').c('N').u32(0).c('N').c('L').c('0');
    EXPECT_EQ(bytes, ref.v);
    auto d = ouch::decode_accepted(bytes.data());
    EXPECT_EQ(d.timestamp, m.timestamp);
    EXPECT_EQ(d.token, m.token);
    EXPECT_EQ(d.side, m.side);
    EXPECT_EQ(d.shares, m.shares);
    EXPECT_EQ(d.stock, m.stock);
    EXPECT_EQ(d.price, m.price);
    EXPECT_EQ(d.tif, m.tif);
    EXPECT_EQ(d.firm, m.firm);
    EXPECT_EQ(d.display, m.display);
    EXPECT_EQ(d.ref, m.ref);
    EXPECT_EQ(d.capacity, m.capacity);
    EXPECT_EQ(d.iso, m.iso);
    EXPECT_EQ(d.min_qty, m.min_qty);
    EXPECT_EQ(d.cross_type, m.cross_type);
    EXPECT_EQ(d.state, m.state);
    EXPECT_EQ(d.bbo_weight, m.bbo_weight);
    EXPECT_EQ(ouch::outbound_size('A'), 66u);
}

TEST(Ouch, Replaced) {
    ouch::Replaced m{987654321, kTok2, 'B', 700, symbol_key("SPY"), 5'500'000, 99'999, firm("ZZZZ"), 'Y', 42,
                     'A', 'N', 0, 'N', 'L', kTok, '0'};
    auto bytes = enc(m, 80);
    Bytes ref;
    ref.c('U').u64(987654321).a("ABC00000000002", 14).c('B').u32(700).a("SPY", 8).u32(5'500'000).u32(99'999)
        .a("ZZZZ", 4).c('Y').u64(42).c('A').c('N').u32(0).c('N').c('L').a("ABC00000000001", 14).c('0');
    EXPECT_EQ(bytes, ref.v);
    auto d = ouch::decode_replaced(bytes.data());
    EXPECT_EQ(d.timestamp, m.timestamp);
    EXPECT_EQ(d.token, m.token);
    EXPECT_EQ(d.side, m.side);
    EXPECT_EQ(d.shares, m.shares);
    EXPECT_EQ(d.stock, m.stock);
    EXPECT_EQ(d.price, m.price);
    EXPECT_EQ(d.tif, m.tif);
    EXPECT_EQ(d.firm, m.firm);
    EXPECT_EQ(d.display, m.display);
    EXPECT_EQ(d.ref, m.ref);
    EXPECT_EQ(d.capacity, m.capacity);
    EXPECT_EQ(d.iso, m.iso);
    EXPECT_EQ(d.min_qty, m.min_qty);
    EXPECT_EQ(d.cross_type, m.cross_type);
    EXPECT_EQ(d.state, m.state);
    EXPECT_EQ(d.previous, m.previous);
    EXPECT_EQ(d.bbo_weight, m.bbo_weight);
    EXPECT_EQ(ouch::outbound_size('U'), 80u);
}

TEST(Ouch, Canceled) {
    ouch::Canceled m{555, kTok, 300, 'I'};
    auto bytes = enc(m, 28);
    Bytes ref;
    ref.c('C').u64(555).a("ABC00000000001", 14).u32(300).c('I');
    EXPECT_EQ(bytes, ref.v);
    auto d = ouch::decode_canceled(bytes.data());
    EXPECT_EQ(d.timestamp, m.timestamp);
    EXPECT_EQ(d.token, m.token);
    EXPECT_EQ(d.decrement, m.decrement);
    EXPECT_EQ(d.reason, m.reason);
    EXPECT_EQ(ouch::outbound_size('C'), 28u);
}

TEST(Ouch, Executed) {
    ouch::Executed m{777, kTok, 100, 1'900'000, 'R', 0xfedcba9876543210ull};
    auto bytes = enc(m, 40);
    Bytes ref;
    ref.c('E').u64(777).a("ABC00000000001", 14).u32(100).u32(1'900'000).c('R').u64(0xfedcba9876543210ull);
    EXPECT_EQ(bytes, ref.v);
    auto d = ouch::decode_executed(bytes.data());
    EXPECT_EQ(d.timestamp, m.timestamp);
    EXPECT_EQ(d.token, m.token);
    EXPECT_EQ(d.shares, m.shares);
    EXPECT_EQ(d.price, m.price);
    EXPECT_EQ(d.liquidity, m.liquidity);
    EXPECT_EQ(d.match, m.match);
    EXPECT_EQ(ouch::outbound_size('E'), 40u);
}

TEST(Ouch, Rejected) {
    ouch::Rejected m{1, kTok, 'X'};
    auto bytes = enc(m, 24);
    Bytes ref;
    ref.c('J').u64(1).a("ABC00000000001", 14).c('X');
    EXPECT_EQ(bytes, ref.v);
    auto d = ouch::decode_rejected(bytes.data());
    EXPECT_EQ(d.timestamp, m.timestamp);
    EXPECT_EQ(d.token, m.token);
    EXPECT_EQ(d.reason, m.reason);
    EXPECT_EQ(ouch::outbound_size('J'), 24u);
    EXPECT_EQ(ouch::outbound_size('S'), 10u);
}

namespace {

template <class M>
std::vector<std::byte> ienc(const M& m, std::size_t expected) {
    std::vector<std::byte> out(64);
    std::size_t n = itch::encode(m, out.data());
    EXPECT_EQ(n, expected);
    EXPECT_EQ(itch::message_size(static_cast<char>(out[0])), expected);
    out.resize(n);
    return out;
}

constexpr std::uint64_t kTs = 0x0000123456789abcull;

}  // namespace

TEST(Itch, SystemEvent) {
    auto bytes = ienc(itch::SystemEvent{kTs, 'O'}, 12);
    Bytes ref;
    ref.c('S').u16(0).u16(0).u48(kTs).c('O');
    EXPECT_EQ(bytes, ref.v);
    auto d = itch::decode_system_event(bytes.data());
    EXPECT_EQ(d.timestamp, kTs);
    EXPECT_EQ(d.event, 'O');
}

TEST(Itch, StockDirectory) {
    itch::StockDirectory m;
    m.locate = 5;
    m.timestamp = kTs;
    m.stock = symbol_key("GOOGL");
    auto bytes = ienc(m, 39);
    Bytes ref;
    ref.c('R').u16(5).u16(0).u48(kTs).a("GOOGL", 8).c('Q').c('N').u32(100).c('N').c('C').c('Z').c(' ').c('P')
        .c('N').c('N').c('1').c('N').u32(0).c('N');
    EXPECT_EQ(bytes, ref.v);
    auto d = itch::decode_stock_directory(bytes.data());
    EXPECT_EQ(d.locate, 5);
    EXPECT_EQ(d.timestamp, kTs);
    EXPECT_EQ(d.stock, m.stock);
    EXPECT_EQ(d.round_lot, 100u);
    EXPECT_EQ(d.issue_subtype[0], 'Z');
    EXPECT_EQ(d.issue_subtype[1], ' ');
    EXPECT_EQ(d.luld_tier, '1');
    EXPECT_EQ(d.inverse, 'N');
}

TEST(Itch, AddOrder) {
    itch::AddOrder m{3, kTs, 0x1122334455667788ull, 'S', 400, symbol_key("NVDA"), 1'200'100};
    auto bytes = ienc(m, 36);
    Bytes ref;
    ref.c('A').u16(3).u16(0).u48(kTs).u64(0x1122334455667788ull).c('S').u32(400).a("NVDA", 8).u32(1'200'100);
    EXPECT_EQ(bytes, ref.v);
    auto d = itch::decode_add(bytes.data());
    EXPECT_EQ(d.locate, 3);
    EXPECT_EQ(d.timestamp, kTs);
    EXPECT_EQ(d.ref, m.ref);
    EXPECT_EQ(d.side, 'S');
    EXPECT_EQ(d.shares, 400u);
    EXPECT_EQ(d.stock, m.stock);
    EXPECT_EQ(d.price, m.price);
}

TEST(Itch, OrderExecuted) {
    itch::OrderExecuted m{8, kTs, 99, 100, 12345};
    auto bytes = ienc(m, 31);
    Bytes ref;
    ref.c('E').u16(8).u16(0).u48(kTs).u64(99).u32(100).u64(12345);
    EXPECT_EQ(bytes, ref.v);
    auto d = itch::decode_executed(bytes.data());
    EXPECT_EQ(d.locate, 8);
    EXPECT_EQ(d.timestamp, kTs);
    EXPECT_EQ(d.ref, 99u);
    EXPECT_EQ(d.shares, 100u);
    EXPECT_EQ(d.match, 12345u);
}

TEST(Itch, OrderCancel) {
    itch::OrderCancel m{1, kTs, 7, 300};
    auto bytes = ienc(m, 23);
    Bytes ref;
    ref.c('X').u16(1).u16(0).u48(kTs).u64(7).u32(300);
    EXPECT_EQ(bytes, ref.v);
    auto d = itch::decode_cancel(bytes.data());
    EXPECT_EQ(d.locate, 1);
    EXPECT_EQ(d.ref, 7u);
    EXPECT_EQ(d.shares, 300u);
}

TEST(Itch, OrderDelete) {
    itch::OrderDelete m{2, kTs, 8};
    auto bytes = ienc(m, 19);
    Bytes ref;
    ref.c('D').u16(2).u16(0).u48(kTs).u64(8);
    EXPECT_EQ(bytes, ref.v);
    auto d = itch::decode_delete(bytes.data());
    EXPECT_EQ(d.locate, 2);
    EXPECT_EQ(d.timestamp, kTs);
    EXPECT_EQ(d.ref, 8u);
}

TEST(Itch, OrderReplace) {
    itch::OrderReplace m{4, kTs, 10, 11, 600, 1'850'000};
    auto bytes = ienc(m, 35);
    Bytes ref;
    ref.c('U').u16(4).u16(0).u48(kTs).u64(10).u64(11).u32(600).u32(1'850'000);
    EXPECT_EQ(bytes, ref.v);
    auto d = itch::decode_replace(bytes.data());
    EXPECT_EQ(d.locate, 4);
    EXPECT_EQ(d.orig_ref, 10u);
    EXPECT_EQ(d.new_ref, 11u);
    EXPECT_EQ(d.shares, 600u);
    EXPECT_EQ(d.price, 1'850'000u);
    auto h = itch::decode_header(bytes.data());
    EXPECT_EQ(h.type, 'U');
    EXPECT_EQ(h.tracking, 0);
    EXPECT_EQ(itch::message_size('Z'), 0u);
}

TEST(Mold, PacketLayoutAndRoundTrip) {
    mold::Packetizer pk;
    std::byte msg[64];
    std::size_t n1 = itch::encode(itch::OrderDelete{1, kTs, 5}, msg);
    pk.add(msg, n1);
    std::size_t n2 = itch::encode(itch::OrderCancel{1, kTs, 6, 100}, msg);
    pk.add(msg, n2);
    std::size_t len = pk.finish();
    ASSERT_EQ(len, 20 + 2 + 19 + 2 + 23u);

    Bytes ref;
    ref.a("LLE0000001", 10).u64(1).u16(2).u16(19);
    std::vector<std::byte> got(pk.data(), pk.data() + len);
    EXPECT_TRUE(std::equal(ref.v.begin(), ref.v.end(), got.begin()));

    mold::PacketHeader h;
    std::vector<std::uint64_t> seqs;
    std::vector<char> types;
    ASSERT_TRUE(mold::parse(pk.data(), len, h, [&](std::uint64_t seq, const std::byte* m, std::size_t n) {
        seqs.push_back(seq);
        types.push_back(static_cast<char>(m[0]));
        EXPECT_EQ(itch::message_size(static_cast<char>(m[0])), n);
    }));
    EXPECT_EQ(std::string_view(h.session, 10), "LLE0000001");
    EXPECT_EQ(h.sequence, 1u);
    EXPECT_EQ(h.count, 2);
    EXPECT_EQ(seqs, (std::vector<std::uint64_t>{1, 2}));
    EXPECT_EQ(types, (std::vector<char>{'D', 'X'}));

    pk.add(msg, n2);
    len = pk.finish();
    ASSERT_TRUE(mold::parse(pk.data(), len, h, [](std::uint64_t, const std::byte*, std::size_t) {}));
    EXPECT_EQ(h.sequence, 3u);
    EXPECT_EQ(h.count, 1);
}

TEST(Mold, PayloadLimit) {
    mold::Packetizer pk;
    std::byte msg[64];
    std::size_t n = itch::encode(itch::AddOrder{1, kTs, 1, 'B', 100, symbol_key("AAPL"), 1'900'000}, msg);
    int count = 0;
    while (pk.fits(n)) {
        pk.add(msg, n);
        ++count;
    }
    EXPECT_EQ(count, (1400 - 20) / (2 + 36));
    EXPECT_LE(pk.finish(), 1400u);
}

TEST(Mold, HeartbeatAndEndOfSession) {
    mold::Packetizer pk;
    std::byte msg[64];
    std::size_t n = itch::encode(itch::OrderDelete{1, kTs, 5}, msg);
    pk.add(msg, n);
    pk.finish();
    std::byte out[mold::kHeaderSize];
    mold::PacketHeader h;
    ASSERT_EQ(pk.heartbeat(out), 20u);
    ASSERT_TRUE(mold::parse(out, 20, h, [](std::uint64_t, const std::byte*, std::size_t) { FAIL(); }));
    EXPECT_EQ(h.sequence, 2u);
    EXPECT_EQ(h.count, 0);
    ASSERT_EQ(pk.end_of_session(out), 20u);
    ASSERT_TRUE(mold::parse(out, 20, h, [](std::uint64_t, const std::byte*, std::size_t) { FAIL(); }));
    EXPECT_EQ(h.sequence, 2u);
    EXPECT_EQ(h.count, mold::kEndOfSession);
}

TEST(Mold, MalformedPacket) {
    mold::Packetizer pk;
    std::byte msg[64];
    pk.add(msg, itch::encode(itch::OrderDelete{1, kTs, 5}, msg));
    std::size_t len = pk.finish();
    mold::PacketHeader h;
    auto noop = [](std::uint64_t, const std::byte*, std::size_t) {};
    EXPECT_FALSE(mold::parse(pk.data(), len - 1, h, noop));
    EXPECT_FALSE(mold::parse(pk.data(), 10, h, noop));
}
