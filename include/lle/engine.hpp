#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#include "lle/common.hpp"
#include "lle/flat_map.hpp"
#include "lle/itch.hpp"
#include "lle/order_book.hpp"
#include "lle/ouch.hpp"

namespace lle {

struct EnterCmd {
    std::uint64_t ts;
    SessionId session;
    ouch::EnterOrder msg;
};

struct CancelCmd {
    std::uint64_t ts;
    SessionId session;
    ouch::CancelOrder msg;
};

struct ReplaceCmd {
    std::uint64_t ts;
    SessionId session;
    ouch::ReplaceOrder msg;
};

// Inbound ring slot. t_decode is the steady-clock time at which the gateway decoded the
// message; seq identifies the command in outbound slots for latency attribution.
struct alignas(128) Command {
    enum class Kind : std::uint8_t { Enter, Cancel, Replace };

    std::int64_t t_decode;
    std::uint64_t seq;
    Kind kind;
    union {
        EnterCmd enter;
        CancelCmd cancel;
        ReplaceCmd replace;
    };
};
static_assert(sizeof(Command) == 128);

inline std::uint64_t token_key(SessionId session, const Token& t) noexcept {
    std::uint64_t a, b;
    std::memcpy(&a, t.b.data(), 8);
    std::memcpy(&b, t.b.data() + 6, 8);
    std::uint64_t h = (a ^ (std::uint64_t{session} << 48)) * 0x9e3779b97f4a7c15ull;
    h = (h ^ (h >> 32) ^ b) * 0xff51afd7ed558ccdull;
    h ^= h >> 29;
    return h != 0 ? h : 1;
}

// How far ahead of the current command the engine loop prefetches; enough to cover a DRAM miss
// at the engine's per-command cost.
inline constexpr std::size_t kPrefetchAhead = 4;

struct EngineStats {
    std::uint64_t enters = 0;
    std::uint64_t cancels = 0;
    std::uint64_t replaces = 0;
    std::uint64_t rejects = 0;
    std::uint64_t ignored = 0;
};

// Single-threaded price-time matcher for kNumSymbols books sharing one order pool. Every
// observable effect goes through Sink, whose callbacks are expected to inline.
template <class Sink>
class Engine {
public:
    static constexpr std::uint32_t kDefaultPool = 1u << 22;

    explicit Engine(Sink& sink, std::uint32_t pool_capacity = kDefaultPool)
        : sink_(sink),
          books_(new OrderBook[kNumSymbols]),
          pool_(new Order[pool_capacity]()),
          free_(new std::uint32_t[pool_capacity]),
          free_top_(pool_capacity),
          refs_(std::size_t{pool_capacity} * 2),
          tokens_(std::size_t{pool_capacity} * 2) {
        for (std::uint32_t i = 0; i < pool_capacity; ++i) free_[i] = pool_capacity - 1 - i;
    }

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void on_command(const Command& c) noexcept {
        switch (c.kind) {
            case Command::Kind::Enter: on_enter(c.enter); break;
            case Command::Kind::Cancel: on_cancel(c.cancel); break;
            case Command::Kind::Replace: on_replace(c.replace); break;
        }
    }

    // Starts loading the token-index slot a later command will probe. That probe is the one
    // access per command that is almost always a cache miss.
    void prefetch(SessionId session, const Token& t) const noexcept { tokens_.prefetch(token_key(session, t)); }

    void prefetch(const Command& c) const noexcept {
        switch (c.kind) {
            case Command::Kind::Enter: prefetch(c.enter.session, c.enter.msg.token); break;
            case Command::Kind::Cancel: prefetch(c.cancel.session, c.cancel.msg.token); break;
            case Command::Kind::Replace: prefetch(c.replace.session, c.replace.msg.existing); break;
        }
    }

    void on_enter(const EnterCmd& c) noexcept {
        const ouch::EnterOrder& m = c.msg;
        ++stats_.enters;
        if (find_token(c.session, m.token) != kNil) {
            ++stats_.ignored;
            return;
        }
        std::uint8_t loc = locate_of(m.stock);
        char reason = loc == 0                  ? 'S'
                      : !valid_price(m.price)   ? 'X'
                      : !valid_shares(m.shares) ? 'Z'
                      : (m.side != 'B' && m.side != 'S') || free_top_ == 0 ? 'O'
                                                                           : 0;
        if (reason != 0) {
            ++stats_.rejects;
            sink_.ouch_rejected(c.session, ouch::Rejected{c.ts, m.token, reason});
            return;
        }
        std::uint64_t ref = next_ref_++;
        sink_.ouch_accepted(c.session, ouch::Accepted{c.ts, m.token, m.side, m.shares, m.stock, m.price, m.tif, m.firm,
                                                      m.display, ref, m.capacity, m.iso, m.min_qty, m.cross_type, 'L',
                                                      kBboWeight});
        Aggressor a{c.ts, c.session, m.token, m.side, loc, m.price / kTickSize, m.tif,
                    m.firm, m.capacity, m.cross_type, ref};
        execute(a, m.shares);
    }

    void on_cancel(const CancelCmd& c) noexcept {
        ++stats_.cancels;
        std::uint32_t idx = find_token(c.session, c.msg.token);
        if (idx == kNil || c.msg.shares >= pool_[idx].open) {
            ++stats_.ignored;
            return;
        }
        Order& o = pool_[idx];
        std::uint32_t dec = o.open - c.msg.shares;
        sink_.ouch_canceled(o.session, ouch::Canceled{c.ts, o.token, dec, 'U'});
        if (c.msg.shares == 0) {
            sink_.itch_delete(itch::OrderDelete{o.locate, c.ts, o.ref});
            books_[o.locate - 1].unlink(pool_.get(), idx);
            release(idx);
        } else {
            books_[o.locate - 1].reduce(o, dec);
            sink_.itch_cancel(itch::OrderCancel{o.locate, c.ts, o.ref, dec});
        }
    }

    void on_replace(const ReplaceCmd& c) noexcept {
        const ouch::ReplaceOrder& m = c.msg;
        ++stats_.replaces;
        std::uint32_t idx = find_token(c.session, m.existing);
        if (idx == kNil || find_token(c.session, m.replacement) != kNil) {
            ++stats_.ignored;
            return;
        }
        char reason = !valid_price(m.price) ? 'X' : !valid_shares(m.shares) ? 'Z' : 0;
        if (reason != 0) {
            ++stats_.rejects;
            sink_.ouch_rejected(c.session, ouch::Rejected{c.ts, m.replacement, reason});
            return;
        }
        const Order& o = pool_[idx];
        std::uint64_t old_ref = o.ref;
        Aggressor a{c.ts, c.session, m.replacement, o.side, o.locate, m.price / kTickSize, m.tif,
                    o.firm, o.capacity, o.cross_type, next_ref_++};
        books_[a.locate - 1].unlink(pool_.get(), idx);
        release(idx);
        sink_.ouch_replaced(c.session,
                            ouch::Replaced{c.ts, m.replacement, a.side, m.shares, key_of_locate(a.locate), m.price,
                                           m.tif, a.firm, m.display, a.ref, a.capacity, m.iso, m.min_qty,
                                           a.cross_type, 'L', m.existing, kBboWeight});
        if (!books_[a.locate - 1].crosses(a.side, a.tick) && m.tif != ouch::kTifIoc) {
            rest(a, m.shares);
            sink_.itch_replace(itch::OrderReplace{a.locate, c.ts, old_ref, a.ref, m.shares, m.price});
            return;
        }
        sink_.itch_delete(itch::OrderDelete{a.locate, c.ts, old_ref});
        execute(a, m.shares);
    }

    // FNV-1a over (locate, side, tick, open shares, reference) of every resting order: symbols in
    // locate order, bids best to worst then asks best to worst, FIFO within a level.
    std::uint64_t book_checksum() const noexcept {
        std::uint64_t h = kFnvOffset;
        for (std::size_t s = 0; s < kNumSymbols; ++s)
            books_[s].for_each_resting(pool_.get(), [&](const Order& o) {
                h = checksum_order(h, o.locate, o.side, o.tick, o.open, o.ref);
            });
        return h;
    }

    const OrderBook& book(std::uint8_t locate) const noexcept { return books_[locate - 1]; }
    const Order* find_order(SessionId session, const Token& t) const noexcept {
        std::uint32_t idx = find_token(session, t);
        return idx == kNil ? nullptr : &pool_[idx];
    }
    const Order* find_ref(std::uint64_t ref) const noexcept {
        const std::uint32_t* idx = refs_.find(ref);
        return idx ? &pool_[*idx] : nullptr;
    }
    const Order* pool() const noexcept { return pool_.get(); }
    std::size_t live_orders() const noexcept { return refs_.size(); }
    std::uint32_t free_slots() const noexcept { return free_top_; }
    std::uint64_t matches() const noexcept { return next_match_ - 1; }
    std::uint64_t next_ref() const noexcept { return next_ref_; }
    const EngineStats& stats() const noexcept { return stats_; }

private:
    static constexpr char kBboWeight = '0';

    struct Aggressor {
        std::uint64_t ts;
        SessionId session;
        Token token;
        char side;
        std::uint8_t locate;
        std::uint32_t tick;
        std::uint32_t tif;
        std::uint32_t firm;
        char capacity;
        char cross_type;
        std::uint64_t ref;
    };

    static bool valid_price(std::uint32_t price) noexcept {
        return price % kTickSize == 0 && price != 0 && price / kTickSize <= kMaxTick;
    }
    static bool valid_shares(std::uint32_t shares) noexcept { return shares >= kMinShares && shares <= kMaxShares; }

    std::uint32_t find_token(SessionId session, const Token& t) const noexcept {
        const std::uint32_t* idx = tokens_.find(token_key(session, t));
        if (idx == nullptr) return kNil;
        const Order& o = pool_[*idx];
        // A 64-bit key collision between two live tokens is treated as a duplicate.
        return o.session == session && o.token == t ? *idx : kNil;
    }

    void release(std::uint32_t idx) noexcept {
        const Order& o = pool_[idx];
        refs_.erase(o.ref);
        tokens_.erase(token_key(o.session, o.token));
        free_[free_top_++] = idx;
    }

    void execute(const Aggressor& a, std::uint32_t shares) noexcept {
        OrderBook& book = books_[a.locate - 1];
        std::uint32_t price = a.tick * kTickSize;
        auto on_fill = [&](const Order& r, std::uint32_t fill) {
            std::uint64_t match = next_match_++;
            std::uint32_t px = r.tick * kTickSize;
            sink_.ouch_executed(r.session, ouch::Executed{a.ts, r.token, fill, px, 'A', match});
            sink_.ouch_executed(a.session, ouch::Executed{a.ts, a.token, fill, px, 'R', match});
            sink_.itch_executed(itch::OrderExecuted{r.locate, a.ts, r.ref, fill, match});
        };
        auto on_done = [&](std::uint32_t idx) { release(idx); };
        std::uint32_t rem = a.side == 'B' ? book.template match<'B'>(pool_.get(), a.tick, shares, on_fill, on_done)
                                          : book.template match<'S'>(pool_.get(), a.tick, shares, on_fill, on_done);
        if (rem == 0) return;
        if (a.tif == ouch::kTifIoc) {
            sink_.ouch_canceled(a.session, ouch::Canceled{a.ts, a.token, rem, 'I'});
            return;
        }
        rest(a, rem);
        sink_.itch_add(itch::AddOrder{a.locate, a.ts, a.ref, a.side, rem, key_of_locate(a.locate), price});
    }

    void rest(const Aggressor& a, std::uint32_t shares) noexcept {
        std::uint32_t idx = free_[--free_top_];
        Order& o = pool_[idx];
        o.ref = a.ref;
        o.open = shares;
        o.tick = a.tick;
        o.session = a.session;
        o.side = a.side;
        o.locate = a.locate;
        o.token = a.token;
        o.capacity = a.capacity;
        o.cross_type = a.cross_type;
        o.firm = a.firm;
        books_[a.locate - 1].append(pool_.get(), idx);
        refs_.insert(a.ref, idx);
        tokens_.insert(token_key(a.session, a.token), idx);
    }

    Sink& sink_;
    std::unique_ptr<OrderBook[]> books_;
    std::unique_ptr<Order[]> pool_;
    std::unique_ptr<std::uint32_t[]> free_;
    std::uint32_t free_top_;
    FlatMap<std::uint64_t, std::uint32_t, IdentityHash> refs_;
    FlatMap<std::uint64_t, std::uint32_t> tokens_;
    std::uint64_t next_ref_ = 1;
    std::uint64_t next_match_ = 1;
    EngineStats stats_;
};

}  // namespace lle
