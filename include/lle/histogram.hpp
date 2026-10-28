#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace lle {

// Log-linear histogram over [1 ns, 10 s]. Values below 256 get exact buckets; above that each
// power of two is split into 128 buckets, so a bucket is at most 1/128 of its lower edge wide.
namespace detail {
inline constexpr unsigned kHistSubBits = 7;
inline constexpr std::uint64_t kHistSub = 1u << kHistSubBits;

constexpr std::size_t hist_index(std::uint64_t v) noexcept {
    if (v < 2 * kHistSub) return static_cast<std::size_t>(v);
    unsigned shift = static_cast<unsigned>(63 - std::countl_zero(v)) - kHistSubBits;
    return static_cast<std::size_t>(shift * kHistSub + (v >> shift));
}
}  // namespace detail

class Histogram {
    static constexpr std::uint64_t kSub = detail::kHistSub;
    static constexpr std::uint64_t kMaxValue = 10'000'000'000ull;
    static constexpr std::size_t kBuckets = detail::hist_index(kMaxValue) + 1;
    static constexpr std::size_t index(std::uint64_t v) noexcept { return detail::hist_index(v); }

public:
    static constexpr std::uint64_t lower_edge(std::size_t i) noexcept {
        if (i < 2 * kSub) return i;
        unsigned shift = static_cast<unsigned>(i / kSub - 1);
        return (i - shift * kSub) << shift;
    }
    static constexpr std::uint64_t upper_edge(std::size_t i) noexcept {
        return i + 1 < 2 * kSub ? i : lower_edge(i + 1) - 1;
    }

    void record(std::uint64_t v) noexcept {
        if (v > kMaxValue) v = kMaxValue;
        ++counts_[index(v)];
        ++count_;
        sum_ += v;
        if (v < min_) min_ = v;
        if (v > max_) max_ = v;
    }

    void merge(const Histogram& o) noexcept {
        for (std::size_t i = 0; i < kBuckets; ++i) counts_[i] += o.counts_[i];
        count_ += o.count_;
        sum_ += o.sum_;
        if (o.min_ < min_) min_ = o.min_;
        if (o.max_ > max_) max_ = o.max_;
    }

    void reset() noexcept { *this = Histogram{}; }

    std::uint64_t count() const noexcept { return count_; }
    std::uint64_t min() const noexcept { return count_ ? min_ : 0; }
    std::uint64_t max() const noexcept { return max_; }
    double mean() const noexcept { return count_ ? static_cast<double>(sum_) / static_cast<double>(count_) : 0.0; }

    // Upper edge of the bucket holding the q-quantile, clamped to the observed range.
    std::uint64_t percentile(double q) const noexcept {
        if (count_ == 0) return 0;
        auto rank = static_cast<std::uint64_t>(std::ceil(q * static_cast<double>(count_)));
        if (rank == 0) rank = 1;
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < kBuckets; ++i) {
            seen += counts_[i];
            if (seen >= rank) {
                std::uint64_t v = upper_edge(i);
                return v < min_ ? min_ : (v > max_ ? max_ : v);
            }
        }
        return max_;
    }

    // JSON object body (no braces) with integer nanosecond percentiles.
    std::string json_fields() const {
        char buf[384];
        std::snprintf(buf, sizeof buf,
                      "\"count\": %llu, \"mean_ns\": %.1f, \"min_ns\": %llu, \"p50_ns\": %llu, \"p90_ns\": %llu, "
                      "\"p99_ns\": %llu, \"p99_9_ns\": %llu, \"p99_99_ns\": %llu, \"max_ns\": %llu",
                      static_cast<unsigned long long>(count_), mean(), static_cast<unsigned long long>(min()),
                      static_cast<unsigned long long>(percentile(0.50)),
                      static_cast<unsigned long long>(percentile(0.90)),
                      static_cast<unsigned long long>(percentile(0.99)),
                      static_cast<unsigned long long>(percentile(0.999)),
                      static_cast<unsigned long long>(percentile(0.9999)), static_cast<unsigned long long>(max_));
        return buf;
    }

    std::string json() const { return "{" + json_fields() + "}"; }

private:
    std::array<std::uint64_t, kBuckets> counts_{};
    std::uint64_t count_ = 0;
    std::uint64_t sum_ = 0;
    std::uint64_t min_ = UINT64_MAX;
    std::uint64_t max_ = 0;
};

}  // namespace lle
