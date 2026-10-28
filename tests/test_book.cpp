#include <gtest/gtest.h>

#include <memory>
#include <random>
#include <set>
#include <vector>

#include "lle/order_book.hpp"

using namespace lle;

TEST(LevelBitmap, FindAgainstBruteForce) {
    auto bits = std::make_unique<LevelBitmap>();
    std::set<std::uint32_t> ref;
    std::mt19937_64 rng(11);
    std::uniform_int_distribution<std::uint32_t> tick(1, kMaxTick);
    for (int op = 0; op < 200'000; ++op) {
        std::uint32_t t = tick(rng);
        if (rng() % 2) {
            bits->set(t);
            ref.insert(t);
        } else if (!ref.empty()) {
            auto it = ref.lower_bound(t);
            if (it == ref.end()) --it;
            bits->clear(*it);
            ref.erase(it);
        }
        std::uint32_t q = tick(rng);
        auto ge = ref.lower_bound(q);
        ASSERT_EQ(bits->find_ge(q), ge == ref.end() ? LevelBitmap::kNone : *ge);
        auto le = ref.upper_bound(q);
        ASSERT_EQ(bits->find_le(q), le == ref.begin() ? 0u : *std::prev(le));
    }
}

TEST(LevelBitmap, Edges) {
    auto bits = std::make_unique<LevelBitmap>();
    EXPECT_EQ(bits->find_ge(1), LevelBitmap::kNone);
    EXPECT_EQ(bits->find_le(kMaxTick), 0u);
    bits->set(1);
    bits->set(kMaxTick);
    EXPECT_EQ(bits->find_ge(0), 1u);
    EXPECT_EQ(bits->find_ge(2), kMaxTick);
    EXPECT_EQ(bits->find_ge(kMaxTick + 1), LevelBitmap::kNone);
    EXPECT_EQ(bits->find_le(kMaxTick - 1), 1u);
    EXPECT_EQ(bits->find_le(0), 0u);
    bits->clear(1);
    EXPECT_EQ(bits->find_le(kMaxTick - 1), 0u);
    EXPECT_TRUE(bits->test(kMaxTick));
}

namespace {

struct BookFixture : ::testing::Test {
    std::unique_ptr<OrderBook> book = std::make_unique<OrderBook>();
    std::vector<Order> pool = std::vector<Order>(64);

    std::uint32_t add(std::uint32_t idx, char side, std::uint32_t tick, std::uint32_t shares) {
        Order& o = pool[idx];
        o = Order{};
        o.ref = idx + 100;
        o.open = shares;
        o.tick = tick;
        o.side = side;
        o.locate = 1;
        book->append(pool.data(), idx);
        return idx;
    }

    std::vector<std::uint64_t> fifo(char side, std::uint32_t tick) {
        std::vector<std::uint64_t> refs;
        for (std::uint32_t i = book->level(side, tick).head; i != kNil; i = pool[i].next) refs.push_back(pool[i].ref);
        return refs;
    }
};

}  // namespace

TEST_F(BookFixture, AppendUnlinkMaintainsFifoAndBest) {
    add(0, 'B', 1000, 100);
    add(1, 'B', 1000, 200);
    add(2, 'B', 1001, 300);
    add(3, 'S', 1005, 100);
    add(4, 'S', 1003, 100);
    EXPECT_EQ(book->best_bid(), 1001u);
    EXPECT_EQ(book->best_ask(), 1003u);
    EXPECT_EQ(fifo('B', 1000), (std::vector<std::uint64_t>{100, 101}));
    EXPECT_EQ(book->level('B', 1000).total_shares, 300u);

    book->unlink(pool.data(), 2);
    EXPECT_EQ(book->best_bid(), 1000u);
    book->unlink(pool.data(), 0);
    EXPECT_EQ(fifo('B', 1000), (std::vector<std::uint64_t>{101}));
    EXPECT_EQ(book->level('B', 1000).total_shares, 200u);
    book->unlink(pool.data(), 1);
    EXPECT_EQ(book->best_bid(), OrderBook::kNoBid);
    book->unlink(pool.data(), 4);
    EXPECT_EQ(book->best_ask(), 1005u);
    book->unlink(pool.data(), 3);
    EXPECT_EQ(book->best_ask(), OrderBook::kNoAsk);
    EXPECT_FALSE(book->crosses('B', kMaxTick));
    EXPECT_FALSE(book->crosses('S', 1));
}

TEST_F(BookFixture, UnlinkMiddleKeepsNeighbours) {
    add(0, 'S', 500, 1);
    add(1, 'S', 500, 2);
    add(2, 'S', 500, 3);
    book->unlink(pool.data(), 1);
    EXPECT_EQ(fifo('S', 500), (std::vector<std::uint64_t>{100, 102}));
    add(1, 'S', 500, 4);
    EXPECT_EQ(fifo('S', 500), (std::vector<std::uint64_t>{100, 102, 101}));
    EXPECT_EQ(book->level('S', 500).total_shares, 8u);
}

TEST_F(BookFixture, MatchSweepsLevelsInPriceTimeOrder) {
    add(0, 'S', 1002, 100);
    add(1, 'S', 1001, 100);
    add(2, 'S', 1001, 100);
    add(3, 'S', 1003, 100);
    std::vector<std::uint64_t> fills;
    std::vector<std::uint32_t> done;
    std::uint32_t rem = book->match<'B'>(
        pool.data(), 1002, 250, [&](const Order& o, std::uint32_t f) { fills.push_back(o.ref * 1000 + f); },
        [&](std::uint32_t idx) { done.push_back(idx); });
    EXPECT_EQ(rem, 0u);
    EXPECT_EQ(fills, (std::vector<std::uint64_t>{101100, 102100, 100050}));
    EXPECT_EQ(done, (std::vector<std::uint32_t>{1, 2}));
    EXPECT_EQ(book->best_ask(), 1002u);
    EXPECT_EQ(pool[0].open, 50u);
    EXPECT_EQ(book->level('S', 1002).total_shares, 50u);
    EXPECT_EQ(book->level('S', 1001).head, kNil);

    rem = book->match<'B'>(pool.data(), 1003, 500, [](const Order&, std::uint32_t) {}, [](std::uint32_t) {});
    EXPECT_EQ(rem, 350u);
    EXPECT_EQ(book->best_ask(), OrderBook::kNoAsk);
}
