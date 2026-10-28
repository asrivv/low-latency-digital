#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "lle/common.hpp"

namespace lle {

inline constexpr std::uint32_t kNil = UINT32_MAX;
inline constexpr std::uint32_t kNumLevels = kMaxTick + 1;

// One cache line per order. firm, capacity and cross_type live in what would otherwise be
// padding so a Replaced message can be built without a second lookup.
struct alignas(64) Order {
    std::uint64_t ref;
    std::uint32_t open;
    std::uint32_t tick;
    std::uint32_t prev;
    std::uint32_t next;
    SessionId session;
    char side;
    std::uint8_t locate;
    Token token;
    char capacity;
    char cross_type;
    std::uint32_t firm;
};
static_assert(sizeof(Order) == 64);
static_assert(offsetof(Order, token) == 28 && offsetof(Order, firm) == 44);

struct Level {
    std::uint32_t head = kNil;
    std::uint32_t tail = kNil;
    std::uint64_t total_shares = 0;
};
static_assert(sizeof(Level) == 16);

// Two-level occupancy bitmap over ticks: 1,563 words of tick bits and a 25-word summary with
// one bit per non-empty word. Any next-level search touches at most two words plus the summary.
class LevelBitmap {
public:
    static constexpr std::size_t kWords = 1563;
    static constexpr std::size_t kSummaryWords = 25;
    static constexpr std::uint32_t kNone = UINT32_MAX;
    static_assert(kWords * 64 >= kNumLevels && kSummaryWords * 64 >= kWords);

    void set(std::uint32_t t) noexcept {
        std::size_t w = t >> 6;
        words_[w] |= 1ull << (t & 63);
        summary_[w >> 6] |= 1ull << (w & 63);
    }

    void clear(std::uint32_t t) noexcept {
        std::size_t w = t >> 6;
        words_[w] &= ~(1ull << (t & 63));
        if (words_[w] == 0) summary_[w >> 6] &= ~(1ull << (w & 63));
    }

    bool test(std::uint32_t t) const noexcept { return (words_[t >> 6] >> (t & 63)) & 1; }

    // Lowest set tick >= t, or kNone.
    std::uint32_t find_ge(std::uint32_t t) const noexcept {
        std::size_t w = t >> 6;
        if (w >= kWords) return kNone;
        std::uint64_t m = words_[w] & (~0ull << (t & 63));
        if (m) return static_cast<std::uint32_t>(w * 64 + std::countr_zero(m));
        std::size_t sw = w + 1;
        for (std::size_t s = sw >> 6; s < kSummaryWords; ++s) {
            std::uint64_t sm = summary_[s];
            if (s == sw >> 6) sm &= ~0ull << (sw & 63);
            if (sm) {
                std::size_t word = s * 64 + static_cast<std::size_t>(std::countr_zero(sm));
                return static_cast<std::uint32_t>(word * 64 + std::countr_zero(words_[word]));
            }
        }
        return kNone;
    }

    // Highest set tick <= t, or 0. Tick 0 is never a valid price, so 0 doubles as "none".
    std::uint32_t find_le(std::uint32_t t) const noexcept {
        std::size_t w = t >> 6;
        std::uint64_t m = words_[w] & (~0ull >> (63 - (t & 63)));
        if (m) return static_cast<std::uint32_t>(w * 64 + 63 - std::countl_zero(m));
        if (w == 0) return 0;
        std::size_t sw = w - 1;
        for (std::size_t s = (sw >> 6) + 1; s-- > 0;) {
            std::uint64_t sm = summary_[s];
            if (s == sw >> 6) sm &= ~0ull >> (63 - (sw & 63));
            if (sm) {
                std::size_t word = s * 64 + 63 - static_cast<std::size_t>(std::countl_zero(sm));
                return static_cast<std::uint32_t>(word * 64 + 63 - std::countl_zero(words_[word]));
            }
        }
        return 0;
    }

private:
    std::uint64_t words_[kWords] = {};
    std::uint64_t summary_[kSummaryWords] = {};
};

// Per-symbol book. Orders live in a pool owned by the engine and are linked into per-level
// FIFOs by pool index. Best prices are cached; empty book sides read kNoBid / kNoAsk, chosen so
// that the crossing comparisons need no emptiness check.
class OrderBook {
public:
    static constexpr std::uint32_t kNoBid = 0;
    static constexpr std::uint32_t kNoAsk = LevelBitmap::kNone;

    OrderBook() : bids_(new Level[kNumLevels]), asks_(new Level[kNumLevels]) {}

    std::uint32_t best_bid() const noexcept { return best_bid_; }
    std::uint32_t best_ask() const noexcept { return best_ask_; }

    bool crosses(char side, std::uint32_t tick) const noexcept {
        return side == 'B' ? tick >= best_ask_ : tick <= best_bid_;
    }

    Level& level(char side, std::uint32_t tick) noexcept { return side == 'B' ? bids_[tick] : asks_[tick]; }
    const Level& level(char side, std::uint32_t tick) const noexcept {
        return side == 'B' ? bids_[tick] : asks_[tick];
    }

    void append(Order* pool, std::uint32_t idx) noexcept {
        Order& o = pool[idx];
        bool bid = o.side == 'B';
        Level& lv = bid ? bids_[o.tick] : asks_[o.tick];
        o.next = kNil;
        o.prev = lv.tail;
        if (lv.tail != kNil) {
            pool[lv.tail].next = idx;
        } else {
            lv.head = idx;
            if (bid) {
                bid_bits_.set(o.tick);
                if (o.tick > best_bid_) best_bid_ = o.tick;
            } else {
                ask_bits_.set(o.tick);
                if (o.tick < best_ask_) best_ask_ = o.tick;
            }
        }
        lv.tail = idx;
        lv.total_shares += o.open;
    }

    void unlink(Order* pool, std::uint32_t idx) noexcept {
        Order& o = pool[idx];
        bool bid = o.side == 'B';
        Level& lv = bid ? bids_[o.tick] : asks_[o.tick];
        if (o.prev != kNil) pool[o.prev].next = o.next; else lv.head = o.next;
        if (o.next != kNil) pool[o.next].prev = o.prev; else lv.tail = o.prev;
        lv.total_shares -= o.open;
        if (lv.head != kNil) return;
        if (bid) {
            bid_bits_.clear(o.tick);
            if (o.tick == best_bid_) best_bid_ = bid_bits_.find_le(o.tick - 1);
        } else {
            ask_bits_.clear(o.tick);
            if (o.tick == best_ask_) best_ask_ = ask_bits_.find_ge(o.tick + 1);
        }
    }

    void reduce(Order& o, std::uint32_t by) noexcept {
        level(o.side, o.tick).total_shares -= by;
        o.open -= by;
    }

    // Sweeps the side opposite to an aggressor on `Agg` with limit `limit`, oldest first at each
    // price. on_fill(resting, shares) runs before the resting order is reduced; on_done(idx) runs
    // once a resting order is fully filled and already detached. Returns the unfilled quantity.
    template <char Agg, class OnFill, class OnDone>
    std::uint32_t match(Order* pool, std::uint32_t limit, std::uint32_t qty, OnFill&& on_fill,
                        OnDone&& on_done) noexcept {
        constexpr bool buy = Agg == 'B';
        Level* levels = buy ? asks_.get() : bids_.get();
        LevelBitmap& bits = buy ? ask_bits_ : bid_bits_;
        std::uint32_t& best = buy ? best_ask_ : best_bid_;
        while (qty != 0 && (buy ? best <= limit : best >= limit)) {
            Level& lv = levels[best];
            std::uint32_t idx = lv.head;
            while (idx != kNil) {
                Order& o = pool[idx];
                std::uint32_t fill = o.open < qty ? o.open : qty;
                on_fill(o, fill);
                o.open -= fill;
                lv.total_shares -= fill;
                qty -= fill;
                if (o.open != 0) break;
                std::uint32_t next = o.next;
                on_done(idx);
                idx = next;
                if (qty == 0) break;
            }
            lv.head = idx;
            if (idx != kNil) {
                pool[idx].prev = kNil;
            } else {
                lv.tail = kNil;
                bits.clear(best);
                best = buy ? bits.find_ge(best + 1) : bits.find_le(best - 1);
            }
        }
        return qty;
    }

    // Bids best to worst, then asks best to worst, FIFO within a level.
    template <class F>
    void for_each_resting(const Order* pool, F&& f) const noexcept {
        for (std::uint32_t t = best_bid_; t != kNoBid; t = bid_bits_.find_le(t - 1))
            for (std::uint32_t i = bids_[t].head; i != kNil; i = pool[i].next) f(pool[i]);
        for (std::uint32_t t = best_ask_; t != kNoAsk; t = ask_bits_.find_ge(t + 1))
            for (std::uint32_t i = asks_[t].head; i != kNil; i = pool[i].next) f(pool[i]);
    }

private:
    std::unique_ptr<Level[]> bids_;
    std::unique_ptr<Level[]> asks_;
    LevelBitmap bid_bits_;
    LevelBitmap ask_bits_;
    std::uint32_t best_bid_ = kNoBid;
    std::uint32_t best_ask_ = kNoAsk;
};

}  // namespace lle
