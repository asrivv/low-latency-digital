#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <unordered_map>

#include "lle/flat_map.hpp"

using namespace lle;

namespace {

template <class Map>
void differential(Map& map, std::uint64_t key_range, std::uint64_t seed, int ops) {
    std::unordered_map<std::uint64_t, std::uint32_t> ref;
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<std::uint64_t> key(1, key_range);
    for (int op = 0; op < ops; ++op) {
        std::uint64_t k = key(rng);
        switch (rng() % 3) {
            case 0: {
                auto v = static_cast<std::uint32_t>(rng());
                bool inserted = ref.emplace(k, v).second;
                ASSERT_EQ(map.insert(k, v), inserted) << "op " << op;
                break;
            }
            case 1:
                ASSERT_EQ(map.erase(k), ref.erase(k) == 1) << "op " << op;
                break;
            default: {
                auto* got = map.find(k);
                auto it = ref.find(k);
                ASSERT_EQ(got != nullptr, it != ref.end()) << "op " << op;
                if (got) ASSERT_EQ(*got, it->second);
            }
        }
        ASSERT_EQ(map.size(), ref.size());
    }
    for (auto& [k, v] : ref) {
        auto* got = map.find(k);
        ASSERT_NE(got, nullptr);
        EXPECT_EQ(*got, v);
    }
}

}  // namespace

TEST(FlatMap, RandomOpsAgainstUnorderedMap) {
    FlatMap<std::uint64_t, std::uint32_t> map(1 << 19);
    differential(map, 300'000, 1, 1'000'000);
}

TEST(FlatMap, HighLoadWithWraparound) {
    FlatMap<std::uint64_t, std::uint32_t> map(1 << 12);
    differential(map, 3'900, 2, 1'000'000);
}

TEST(FlatMap, ClusteredKeysBackwardShift) {
    FlatMap<std::uint64_t, std::uint32_t, IdentityHash> map(1 << 10);
    differential(map, 1'000, 3, 1'000'000);
}

TEST(FlatMap, Basics) {
    FlatMap<std::uint64_t, std::uint32_t> map(16);
    EXPECT_EQ(map.capacity(), 16u);
    EXPECT_TRUE(map.insert(5, 50));
    EXPECT_FALSE(map.insert(5, 51));
    EXPECT_EQ(*map.find(5), 50u);
    EXPECT_EQ(map.find(6), nullptr);
    EXPECT_TRUE(map.erase(5));
    EXPECT_FALSE(map.erase(5));
    EXPECT_EQ(map.size(), 0u);
}
