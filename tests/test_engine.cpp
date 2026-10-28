#include <gtest/gtest.h>

#include <memory>
#include <random>
#include <string>
#include <vector>

#include "lle/engine.hpp"
#include "recording_sink.hpp"

using namespace lle;

namespace {

constexpr std::uint32_t px(std::uint32_t cents) { return cents * 100; }
constexpr std::uint32_t kDay = 99'999;

using Kinds = std::vector<std::string>;

struct EngineTest : ::testing::Test {
    RecordingSink sink;
    std::unique_ptr<Engine<RecordingSink>> eng = std::make_unique<Engine<RecordingSink>>(sink, 4096);
    std::uint64_t ts = 1000;

    void enter(SessionId s, std::string_view tok, char side, std::uint32_t shares, std::uint32_t price,
               std::uint32_t tif = kDay, std::string_view sym = "AAPL") {
        eng->on_enter(EnterCmd{ts++, s,
                               ouch::EnterOrder{Token::from(tok), side, shares, symbol_key(sym), price, tif,
                                                0x46524d31, 'Y', 'A', 'N', 0, 'N', 'R'}});
    }
    void cancel(SessionId s, std::string_view tok, std::uint32_t shares = 0) {
        eng->on_cancel(CancelCmd{ts++, s, ouch::CancelOrder{Token::from(tok), shares}});
    }
    void replace(SessionId s, std::string_view old_tok, std::string_view new_tok, std::uint32_t shares,
                 std::uint32_t price, std::uint32_t tif = kDay) {
        eng->on_replace(ReplaceCmd{ts++, s,
                                   ouch::ReplaceOrder{Token::from(old_tok), Token::from(new_tok), shares, price, tif,
                                                      'Y', 'N', 0}});
    }
    const RecordingSink::Event& ev(std::size_t i) { return sink.events.at(i); }
    std::uint32_t best_bid() { return eng->book(1).best_bid(); }
    std::uint32_t best_ask() { return eng->book(1).best_ask(); }
    std::vector<std::uint64_t> fifo(char side, std::uint32_t tick) {
        std::vector<std::uint64_t> refs;
        for (std::uint32_t i = eng->book(1).level(side, tick).head; i != kNil; i = eng->pool()[i].next)
            refs.push_back(eng->pool()[i].ref);
        return refs;
    }
    char reject_reason() {
        EXPECT_EQ(sink.events.size(), 1u);
        EXPECT_EQ(sink.events.back().kind(), "oJ");
        char r = ouch::decode_rejected(sink.events.back().data()).reason;
        sink.clear();
        return r;
    }
};

}  // namespace

TEST_F(EngineTest, AcceptedEchoesFieldsAndRests) {
    enter(1, "A1", 'B', 300, px(19000));
    ASSERT_EQ(sink.kinds(), (Kinds{"oA", "iA"}));
    auto a = ouch::decode_accepted(ev(0).data());
    EXPECT_EQ(ev(0).session, 1);
    EXPECT_EQ(a.timestamp, 1000u);
    EXPECT_EQ(a.token, Token::from("A1"));
    EXPECT_EQ(a.side, 'B');
    EXPECT_EQ(a.shares, 300u);
    EXPECT_EQ(a.stock, symbol_key("AAPL"));
    EXPECT_EQ(a.price, px(19000));
    EXPECT_EQ(a.tif, kDay);
    EXPECT_EQ(a.firm, 0x46524d31u);
    EXPECT_EQ(a.ref, 1u);
    EXPECT_EQ(a.state, 'L');
    auto add = itch::decode_add(ev(1).data());
    EXPECT_EQ(add.locate, 1);
    EXPECT_EQ(add.ref, 1u);
    EXPECT_EQ(add.side, 'B');
    EXPECT_EQ(add.shares, 300u);
    EXPECT_EQ(add.price, px(19000));
    EXPECT_EQ(best_bid(), 19000u);
    EXPECT_EQ(eng->live_orders(), 1u);
}

TEST_F(EngineTest, PriceTimePriority) {
    enter(1, "S1", 'S', 100, px(10000));
    enter(2, "S2", 'S', 100, px(10000));
    enter(1, "S3", 'S', 100, px(9999));
    sink.clear();
    enter(3, "B1", 'B', 250, px(10000));
    ASSERT_EQ(sink.kinds(), (Kinds{"oA", "oE", "oE", "iE", "oE", "oE", "iE", "oE", "oE", "iE"}));
    struct Fill { SessionId s; const char* tok; std::uint32_t shares, price; char liq; std::uint64_t match; };
    Fill want[] = {{1, "S3", 100, px(9999), 'A', 1}, {3, "B1", 100, px(9999), 'R', 1},
                   {1, "S1", 100, px(10000), 'A', 2}, {3, "B1", 100, px(10000), 'R', 2},
                   {2, "S2", 50, px(10000), 'A', 3}, {3, "B1", 50, px(10000), 'R', 3}};
    std::size_t w = 0;
    for (std::size_t i = 1; i < sink.events.size(); ++i) {
        if (ev(i).stream != 'o') continue;
        auto e = ouch::decode_executed(ev(i).data());
        EXPECT_EQ(ev(i).session, want[w].s);
        EXPECT_EQ(e.token, Token::from(want[w].tok));
        EXPECT_EQ(e.shares, want[w].shares);
        EXPECT_EQ(e.price, want[w].price);
        EXPECT_EQ(e.liquidity, want[w].liq);
        EXPECT_EQ(e.match, want[w].match);
        ++w;
    }
    auto ie = itch::decode_executed(ev(9).data());
    EXPECT_EQ(ie.ref, 2u);
    EXPECT_EQ(ie.shares, 50u);
    EXPECT_EQ(ie.match, 3u);
    EXPECT_EQ(eng->find_order(2, Token::from("S2"))->open, 50u);
    EXPECT_EQ(eng->find_order(1, Token::from("S1")), nullptr);
    EXPECT_EQ(best_ask(), 10000u);
    EXPECT_EQ(best_bid(), OrderBook::kNoBid);
    EXPECT_EQ(eng->matches(), 3u);
}

TEST_F(EngineTest, PartialFillsAcrossLevelsThenRest) {
    enter(1, "S1", 'S', 100, px(10000));
    enter(1, "S2", 'S', 200, px(10001));
    enter(1, "S3", 'S', 300, px(10002));
    sink.clear();
    enter(2, "B1", 'B', 400, px(10001));
    ASSERT_EQ(sink.kinds(), (Kinds{"oA", "oE", "oE", "iE", "oE", "oE", "iE", "iA"}));
    auto add = itch::decode_add(ev(7).data());
    EXPECT_EQ(add.ref, 4u);
    EXPECT_EQ(add.side, 'B');
    EXPECT_EQ(add.shares, 100u);
    EXPECT_EQ(add.price, px(10001));
    EXPECT_EQ(best_bid(), 10001u);
    EXPECT_EQ(best_ask(), 10002u);
    EXPECT_EQ(eng->book(1).level('B', 10001).total_shares, 100u);
}

TEST_F(EngineTest, IocRemainderCanceled) {
    enter(1, "S1", 'S', 100, px(10000));
    sink.clear();
    enter(2, "I1", 'B', 300, px(10001), ouch::kTifIoc);
    ASSERT_EQ(sink.kinds(), (Kinds{"oA", "oE", "oE", "iE", "oC"}));
    auto c = ouch::decode_canceled(ev(4).data());
    EXPECT_EQ(ev(4).session, 2);
    EXPECT_EQ(c.token, Token::from("I1"));
    EXPECT_EQ(c.decrement, 200u);
    EXPECT_EQ(c.reason, 'I');
    EXPECT_EQ(best_bid(), OrderBook::kNoBid);
    EXPECT_EQ(eng->live_orders(), 0u);

    sink.clear();
    enter(2, "I2", 'S', 100, px(10000), ouch::kTifIoc);
    EXPECT_EQ(sink.kinds(), (Kinds{"oA", "oC"}));
    EXPECT_EQ(ouch::decode_canceled(ev(1).data()).decrement, 100u);
}

TEST_F(EngineTest, CancelReduceKeepsPriority) {
    enter(1, "S1", 'S', 300, px(10000));
    enter(2, "S2", 'S', 100, px(10000));
    sink.clear();
    cancel(1, "S1", 100);
    ASSERT_EQ(sink.kinds(), (Kinds{"oC", "iX"}));
    auto c = ouch::decode_canceled(ev(0).data());
    EXPECT_EQ(c.decrement, 200u);
    EXPECT_EQ(c.reason, 'U');
    auto x = itch::decode_cancel(ev(1).data());
    EXPECT_EQ(x.ref, 1u);
    EXPECT_EQ(x.shares, 200u);
    EXPECT_EQ(fifo('S', 10000), (std::vector<std::uint64_t>{1, 2}));
    EXPECT_EQ(eng->book(1).level('S', 10000).total_shares, 200u);

    sink.clear();
    cancel(1, "S1", 100);
    cancel(1, "S1", 500);
    cancel(1, "NOPE", 0);
    EXPECT_TRUE(sink.events.empty());

    enter(3, "B1", 'B', 100, px(10000));
    EXPECT_EQ(ouch::decode_executed(ev(1).data()).token, Token::from("S1"));
}

TEST_F(EngineTest, CancelToZero) {
    enter(1, "S1", 'S', 300, px(10000));
    sink.clear();
    cancel(1, "S1", 0);
    ASSERT_EQ(sink.kinds(), (Kinds{"oC", "iD"}));
    EXPECT_EQ(ouch::decode_canceled(ev(0).data()).decrement, 300u);
    EXPECT_EQ(itch::decode_delete(ev(1).data()).ref, 1u);
    EXPECT_EQ(best_ask(), OrderBook::kNoAsk);
    EXPECT_EQ(eng->live_orders(), 0u);
    EXPECT_EQ(eng->find_ref(1), nullptr);
    sink.clear();
    enter(1, "S1", 'S', 100, px(10000));
    EXPECT_EQ(sink.kinds(), (Kinds{"oA", "iA"}));
}

TEST_F(EngineTest, ReplaceLosesPriority) {
    enter(1, "S1", 'S', 100, px(10000));
    enter(2, "S2", 'S', 100, px(10000));
    sink.clear();
    replace(1, "S1", "S1b", 200, px(10000));
    ASSERT_EQ(sink.kinds(), (Kinds{"oU", "iU"}));
    auto r = ouch::decode_replaced(ev(0).data());
    EXPECT_EQ(r.token, Token::from("S1b"));
    EXPECT_EQ(r.previous, Token::from("S1"));
    EXPECT_EQ(r.side, 'S');
    EXPECT_EQ(r.shares, 200u);
    EXPECT_EQ(r.ref, 3u);
    EXPECT_EQ(r.stock, symbol_key("AAPL"));
    EXPECT_EQ(r.firm, 0x46524d31u);
    auto u = itch::decode_replace(ev(1).data());
    EXPECT_EQ(u.orig_ref, 1u);
    EXPECT_EQ(u.new_ref, 3u);
    EXPECT_EQ(u.shares, 200u);
    EXPECT_EQ(u.price, px(10000));
    EXPECT_EQ(fifo('S', 10000), (std::vector<std::uint64_t>{2, 3}));
    EXPECT_EQ(eng->find_order(1, Token::from("S1")), nullptr);
    EXPECT_EQ(eng->find_ref(3)->open, 200u);

    sink.clear();
    replace(1, "S1", "S1c", 100, px(10000));
    replace(1, "S1b", "S1b", 100, px(10000));
    replace(2, "S2", "S1b", 100, px(9000));
    EXPECT_EQ(sink.kinds(), (Kinds{"oU", "iU"}));
}

TEST_F(EngineTest, CrossingReplace) {
    enter(1, "B1", 'B', 100, px(9999));
    enter(2, "S1", 'S', 200, px(10001));
    sink.clear();
    replace(2, "S1", "S1b", 300, px(9999));
    ASSERT_EQ(sink.kinds(), (Kinds{"oU", "iD", "oE", "oE", "iE", "iA"}));
    EXPECT_EQ(itch::decode_delete(ev(1).data()).ref, 2u);
    auto e = ouch::decode_executed(ev(3).data());
    EXPECT_EQ(e.token, Token::from("S1b"));
    EXPECT_EQ(e.liquidity, 'R');
    EXPECT_EQ(e.price, px(9999));
    auto add = itch::decode_add(ev(5).data());
    EXPECT_EQ(add.ref, 3u);
    EXPECT_EQ(add.side, 'S');
    EXPECT_EQ(add.shares, 200u);
    EXPECT_EQ(best_ask(), 9999u);
    EXPECT_EQ(best_bid(), OrderBook::kNoBid);

    sink.clear();
    enter(1, "B2", 'B', 100, px(9000));
    replace(1, "B2", "B2b", 500, px(9999), ouch::kTifIoc);
    EXPECT_EQ(sink.kinds(), (Kinds{"oA", "iA", "oU", "iD", "oE", "oE", "iE", "oC"}));
    EXPECT_EQ(ouch::decode_canceled(sink.events.back().data()).decrement, 300u);
}

TEST_F(EngineTest, EveryRejectReason) {
    enter(1, "R1", 'B', 100, px(10000), kDay, "IBM");
    EXPECT_EQ(reject_reason(), 'S');
    enter(1, "R2", 'B', 100, 0);
    EXPECT_EQ(reject_reason(), 'X');
    enter(1, "R3", 'B', 100, px(100001));
    EXPECT_EQ(reject_reason(), 'X');
    enter(1, "R4", 'B', 100, 1'000'050);
    EXPECT_EQ(reject_reason(), 'X');
    enter(1, "R5", 'B', 0, px(10000));
    EXPECT_EQ(reject_reason(), 'Z');
    enter(1, "R6", 'B', 1'000'001, px(10000));
    EXPECT_EQ(reject_reason(), 'Z');
    enter(1, "R7", 'T', 100, px(10000));
    EXPECT_EQ(reject_reason(), 'O');
    enter(1, "R8", 'E', 100, px(10000));
    EXPECT_EQ(reject_reason(), 'O');
    enter(1, "OK1", 'B', 1'000'000, px(1));
    enter(1, "OK2", 'S', 1, px(100000));
    EXPECT_EQ(sink.kinds(), (Kinds{"oA", "iA", "oA", "iA"}));
    sink.clear();

    replace(1, "OK1", "OK1b", 100, px(100001));
    EXPECT_EQ(reject_reason(), 'X');
    replace(1, "OK1", "OK1b", 0, px(100));
    auto j = ouch::decode_rejected(ev(0).data());
    EXPECT_EQ(j.token, Token::from("OK1b"));
    EXPECT_EQ(reject_reason(), 'Z');
    EXPECT_EQ(eng->find_order(1, Token::from("OK1"))->open, 1'000'000u);
    EXPECT_EQ(eng->stats().rejects, 10u);
}

TEST_F(EngineTest, DuplicateTokenIgnored) {
    enter(1, "T1", 'B', 100, px(10000));
    sink.clear();
    enter(1, "T1", 'B', 100, px(10000));
    enter(1, "T1", 'S', 100, px(10000));
    EXPECT_TRUE(sink.events.empty());
    enter(2, "T1", 'B', 100, px(10000));
    EXPECT_EQ(sink.kinds(), (Kinds{"oA", "iA"}));
    EXPECT_EQ(eng->stats().ignored, 2u);
}

TEST_F(EngineTest, PoolSlotReuse) {
    RecordingSink small_sink;
    Engine<RecordingSink> small(small_sink, 4);
    auto put = [&](std::string_view tok, std::uint32_t cents) {
        small.on_enter(EnterCmd{1, 1,
                                ouch::EnterOrder{Token::from(tok), 'B', 100, symbol_key("MSFT"), px(cents), kDay, 0,
                                                 'Y', 'A', 'N', 0, 'N', 'R'}});
    };
    for (int i = 0; i < 4; ++i) put("P" + std::to_string(i), 40000 + i);
    EXPECT_EQ(small.free_slots(), 0u);
    small_sink.clear();
    put("P4", 40010);
    ASSERT_EQ(small_sink.kinds(), (Kinds{"oJ"}));
    EXPECT_EQ(ouch::decode_rejected(small_sink.events[0].data()).reason, 'O');

    small.on_cancel(CancelCmd{2, 1, ouch::CancelOrder{Token::from("P2"), 0}});
    EXPECT_EQ(small.free_slots(), 1u);
    const Order* freed = small.pool() + 2;
    small_sink.clear();
    put("P5", 40020);
    EXPECT_EQ(small_sink.kinds(), (Kinds{"oA", "iA"}));
    EXPECT_EQ(small.find_order(1, Token::from("P5")), freed);
    EXPECT_EQ(small.book(2).best_bid(), 40020u);

    small_sink.clear();
    small.on_enter(EnterCmd{3, 2,
                            ouch::EnterOrder{Token::from("X"), 'S', 100, symbol_key("MSFT"), px(40000), kDay, 0,
                                             'Y', 'A', 'N', 0, 'N', 'R'}});
    ASSERT_EQ(small_sink.kinds(), (Kinds{"oJ"}));

    small.on_cancel(CancelCmd{4, 1, ouch::CancelOrder{Token::from("P5"), 0}});
    small.on_enter(EnterCmd{5, 2,
                            ouch::EnterOrder{Token::from("Y"), 'S', 300, symbol_key("MSFT"), px(40000), kDay, 0,
                                             'Y', 'A', 'N', 0, 'N', 'R'}});
    EXPECT_EQ(small.free_slots(), 4u);
    EXPECT_EQ(small.live_orders(), 0u);
}

TEST_F(EngineTest, BestPriceRecoveryAfterSweep) {
    std::mt19937_64 rng(5);
    std::vector<std::uint32_t> ticks;
    for (std::uint32_t t = 5000; t < 40000; t += 1 + static_cast<std::uint32_t>(rng() % 150)) ticks.push_back(t);
    std::uint64_t total = 0;
    int n = 0;
    for (auto t : ticks) {
        enter(1, "S" + std::to_string(n++), 'S', 100, px(t));
        total += 100;
    }
    enter(1, "FAR", 'S', 100, px(99000));
    enter(1, "BID", 'B', 100, px(10));
    EXPECT_EQ(best_ask(), ticks.front());
    sink.clear();
    enter(2, "SWEEP", 'B', static_cast<std::uint32_t>(total), px(ticks.back()));
    EXPECT_EQ(best_ask(), 99000u);
    EXPECT_EQ(eng->matches(), ticks.size());
    EXPECT_EQ(sink.kinds().back(), "iE");

    for (std::uint32_t i = 0; i < 300; ++i) enter(3, "b" + std::to_string(i), 'B', 100, px(20000 + i * 97));
    enter(4, "SWEEP2", 'S', 298 * 100 + 50, px(20000 + 97));
    EXPECT_EQ(best_bid(), 20000u + 97);
    EXPECT_EQ(eng->book(1).level('B', 20097).total_shares, 50u);
    enter(4, "SWEEP3", 'S', 1000, px(5));
    EXPECT_EQ(best_bid(), OrderBook::kNoBid);
    EXPECT_EQ(best_ask(), 5u);
}

TEST_F(EngineTest, ChecksumTracksBook) {
    EXPECT_EQ(eng->book_checksum(), kFnvOffset);
    enter(1, "A", 'B', 100, px(10000));
    enter(1, "B", 'S', 100, px(10100), kDay, "SPY");
    std::uint64_t h = kFnvOffset;
    h = checksum_order(h, 1, 'B', 10000, 100, 1);
    h = checksum_order(h, 8, 'S', 10100, 100, 2);
    EXPECT_EQ(eng->book_checksum(), h);
    cancel(1, "A");
    cancel(1, "B");
    EXPECT_EQ(eng->book_checksum(), kFnvOffset);
}
