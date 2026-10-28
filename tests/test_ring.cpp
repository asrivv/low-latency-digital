#include <gtest/gtest.h>

#include <cstdint>
#include <thread>

#include "lle/spsc_ring.hpp"

using namespace lle;

TEST(SpscRing, SingleThreadFullAndEmpty) {
    SpscRing<int, 8> r;
    int v;
    EXPECT_FALSE(r.try_pop(v));
    EXPECT_TRUE(r.empty());
    for (int i = 0; i < 8; ++i) EXPECT_TRUE(r.try_push(i));
    EXPECT_FALSE(r.try_push(99));
    EXPECT_EQ(r.claim(), nullptr);
    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(r.try_pop(v));
        EXPECT_EQ(v, i);
    }
    EXPECT_FALSE(r.try_pop(v));
    EXPECT_TRUE(r.empty());
}

TEST(SpscRing, ClaimCommitPeekRelease) {
    SpscRing<std::uint64_t, 4> r;
    for (std::uint64_t round = 0; round < 10; ++round) {
        for (std::uint64_t i = 0; i < 3; ++i) {
            std::uint64_t* s = r.claim();
            ASSERT_NE(s, nullptr);
            *s = round * 10 + i;
            r.commit();
        }
        for (std::uint64_t i = 0; i < 3; ++i) {
            std::uint64_t* s = r.peek();
            ASSERT_NE(s, nullptr);
            EXPECT_EQ(*s, round * 10 + i);
            r.release();
        }
        EXPECT_EQ(r.peek(), nullptr);
    }
}

TEST(SpscRing, PeekAt) {
    SpscRing<int, 8> r;
    EXPECT_EQ(r.peek_at(0), nullptr);
    for (int i = 0; i < 5; ++i) r.try_push(i);
    ASSERT_NE(r.peek_at(4), nullptr);
    EXPECT_EQ(*r.peek_at(4), 4);
    EXPECT_EQ(r.peek_at(5), nullptr);
    int v;
    r.try_pop(v);
    EXPECT_EQ(*r.peek_at(3), 4);
    EXPECT_EQ(r.peek_at(4), nullptr);
}

TEST(SpscRing, TwoThreadStress) {
    constexpr std::uint64_t kCount = 100'000'000;
    auto ring = std::make_unique<SpscRing<std::uint64_t, 1 << 16>>();
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kCount; ++i) {
            std::uint64_t* s;
            while ((s = ring->claim()) == nullptr) cpu_relax();
            *s = i;
            ring->commit();
        }
    });
    std::uint64_t received = 0;
    std::uint64_t mismatches = 0;
    while (received < kCount) {
        std::uint64_t* s = ring->peek();
        if (s == nullptr) {
            cpu_relax();
            continue;
        }
        mismatches += *s != received;
        ++received;
        ring->release();
    }
    producer.join();
    EXPECT_EQ(mismatches, 0u);
    EXPECT_TRUE(ring->empty());
}
