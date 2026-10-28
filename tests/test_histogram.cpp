#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include "lle/histogram.hpp"

using namespace lle;

TEST(Histogram, BucketEdgesAreContiguous) {
    for (std::size_t i = 1; i < 5000; ++i) EXPECT_EQ(Histogram::lower_edge(i), Histogram::upper_edge(i - 1) + 1);
}

TEST(Histogram, ExactBelow256) {
    Histogram h;
    for (std::uint64_t v = 1; v <= 100; ++v) h.record(v);
    EXPECT_EQ(h.percentile(0.5), 50u);
    EXPECT_EQ(h.percentile(0.99), 99u);
    EXPECT_EQ(h.percentile(1.0), 100u);
    EXPECT_EQ(h.min(), 1u);
    EXPECT_EQ(h.max(), 100u);
    EXPECT_DOUBLE_EQ(h.mean(), 50.5);
}

TEST(Histogram, RelativeErrorUnderOnePercent) {
    std::mt19937_64 rng(7);
    std::lognormal_distribution<double> dist(8.0, 2.0);
    std::vector<std::uint64_t> values;
    Histogram h;
    for (int i = 0; i < 200'000; ++i) {
        auto v = std::min<std::uint64_t>(static_cast<std::uint64_t>(dist(rng)) + 1, 10'000'000'000ull);
        values.push_back(v);
        h.record(v);
    }
    std::sort(values.begin(), values.end());
    for (double q : {0.5, 0.9, 0.99, 0.999, 0.9999}) {
        auto exact = values[static_cast<std::size_t>(std::ceil(q * values.size())) - 1];
        auto got = h.percentile(q);
        EXPECT_GE(got, exact);
        EXPECT_LE(static_cast<double>(got - exact), 0.01 * static_cast<double>(exact)) << q;
    }
    EXPECT_EQ(h.max(), values.back());
}

TEST(Histogram, MergeAndClamp) {
    Histogram a, b;
    a.record(10);
    b.record(20'000'000'000ull);
    a.merge(b);
    EXPECT_EQ(a.count(), 2u);
    EXPECT_EQ(a.max(), 10'000'000'000ull);
    EXPECT_EQ(a.percentile(0.5), 10u);
    EXPECT_NE(a.json().find("\"p99_ns\""), std::string::npos);
}
