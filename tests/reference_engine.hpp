#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <string>
#include <unordered_map>

#include "lle/common.hpp"
#include "lle/engine.hpp"
#include "lle/itch.hpp"
#include "lle/ouch.hpp"

// Straightforward matcher with the same observable semantics as lle::Engine, built on
// std::map price levels and std::list FIFOs. No pool, so it never rejects for capacity.
template <class Sink>
class ReferenceEngine {
public:
    explicit ReferenceEngine(Sink& sink) : sink_(sink) {}

    void on_enter(const lle::EnterCmd& c) {
        const auto& m = c.msg;
        if (by_token_.count(key(c.session, m.token))) return;
        std::uint8_t loc = lle::locate_of(m.stock);
        char reason = 0;
        if (loc == 0) reason = 'S';
        else if (!price_ok(m.price)) reason = 'X';
        else if (m.shares < 1 || m.shares > 1'000'000) reason = 'Z';
        else if (m.side != 'B' && m.side != 'S') reason = 'O';
        if (reason) {
            sink_.ouch_rejected(c.session, {c.ts, m.token, reason});
            return;
        }
        std::uint64_t ref = next_ref_++;
        sink_.ouch_accepted(c.session, {c.ts, m.token, m.side, m.shares, m.stock, m.price, m.tif, m.firm, m.display,
                                        ref, m.capacity, m.iso, m.min_qty, m.cross_type, 'L', '0'});
        Order o{ref, m.shares, c.session, m.token, m.side, loc, m.price / 100, m.firm, m.capacity, m.cross_type};
        place(o, c.ts, m.tif);
    }

    void on_cancel(const lle::CancelCmd& c) {
        auto t = by_token_.find(key(c.session, c.msg.token));
        if (t == by_token_.end()) return;
        Where& w = by_ref_.at(t->second);
        Order& o = *w.it;
        if (c.msg.shares >= o.open) return;
        std::uint32_t dec = o.open - c.msg.shares;
        sink_.ouch_canceled(o.session, {c.ts, o.token, dec, 'U'});
        if (c.msg.shares == 0) {
            sink_.itch_delete({o.locate, c.ts, o.ref});
            erase(o.ref);
        } else {
            o.open = c.msg.shares;
            sink_.itch_cancel({o.locate, c.ts, o.ref, dec});
        }
    }

    void on_replace(const lle::ReplaceCmd& c) {
        const auto& m = c.msg;
        auto t = by_token_.find(key(c.session, m.existing));
        if (t == by_token_.end() || by_token_.count(key(c.session, m.replacement))) return;
        if (!price_ok(m.price) || m.shares < 1 || m.shares > 1'000'000) {
            sink_.ouch_rejected(c.session, {c.ts, m.replacement, !price_ok(m.price) ? 'X' : 'Z'});
            return;
        }
        Order old = *by_ref_.at(t->second).it;
        erase(old.ref);
        Order o{next_ref_++, m.shares, c.session, m.replacement, old.side, old.locate, m.price / 100,
                old.firm, old.capacity, old.cross_type};
        sink_.ouch_replaced(c.session, {c.ts, m.replacement, o.side, m.shares, lle::key_of_locate(o.locate), m.price,
                                        m.tif, o.firm, m.display, o.ref, o.capacity, m.iso, m.min_qty, o.cross_type,
                                        'L', m.existing, '0'});
        Book& b = books_[o.locate - 1];
        bool crosses = o.side == 'B' ? (!b.asks.empty() && b.asks.begin()->first <= o.tick)
                                     : (!b.bids.empty() && b.bids.begin()->first >= o.tick);
        if (!crosses && m.tif != 0) {
            insert(o);
            sink_.itch_replace({o.locate, c.ts, old.ref, o.ref, m.shares, m.price});
            return;
        }
        sink_.itch_delete({o.locate, c.ts, old.ref});
        place(o, c.ts, m.tif);
    }

    std::uint64_t book_checksum() const {
        std::uint64_t h = lle::kFnvOffset;
        for (const Book& b : books_) {
            for (const auto& [tick, q] : b.bids)
                for (const Order& o : q) h = lle::checksum_order(h, o.locate, o.side, tick, o.open, o.ref);
            for (const auto& [tick, q] : b.asks)
                for (const Order& o : q) h = lle::checksum_order(h, o.locate, o.side, tick, o.open, o.ref);
        }
        return h;
    }

    std::size_t live_orders() const { return by_ref_.size(); }

private:
    struct Order {
        std::uint64_t ref;
        std::uint32_t open;
        lle::SessionId session;
        lle::Token token;
        char side;
        std::uint8_t locate;
        std::uint32_t tick;
        std::uint32_t firm;
        char capacity;
        char cross_type;
    };
    using Queue = std::list<Order>;
    struct Book {
        std::map<std::uint32_t, Queue, std::greater<>> bids;
        std::map<std::uint32_t, Queue> asks;
    };
    struct Where {
        Queue::iterator it;
    };

    static bool price_ok(std::uint32_t p) { return p % 100 == 0 && p >= 100 && p <= 100'000 * 100; }
    static std::string key(lle::SessionId s, const lle::Token& t) {
        return std::to_string(s) + ":" + std::string(t.view());
    }

    template <class Side>
    std::uint32_t sweep(Side& side, const Order& agg, std::uint32_t qty, std::uint64_t ts) {
        while (qty > 0 && !side.empty()) {
            auto lvl = side.begin();
            if (agg.side == 'B' ? lvl->first > agg.tick : lvl->first < agg.tick) break;
            Queue& q = lvl->second;
            while (qty > 0 && !q.empty()) {
                Order& r = q.front();
                std::uint32_t fill = std::min(qty, r.open);
                std::uint64_t match = next_match_++;
                std::uint32_t px = lvl->first * 100;
                sink_.ouch_executed(r.session, {ts, r.token, fill, px, 'A', match});
                sink_.ouch_executed(agg.session, {ts, agg.token, fill, px, 'R', match});
                sink_.itch_executed({r.locate, ts, r.ref, fill, match});
                r.open -= fill;
                qty -= fill;
                if (r.open == 0) {
                    by_token_.erase(key(r.session, r.token));
                    by_ref_.erase(r.ref);
                    q.pop_front();
                }
            }
            if (q.empty()) side.erase(lvl);
        }
        return qty;
    }

    void place(Order o, std::uint64_t ts, std::uint32_t tif) {
        Book& b = books_[o.locate - 1];
        o.open = o.side == 'B' ? sweep(b.asks, o, o.open, ts) : sweep(b.bids, o, o.open, ts);
        if (o.open == 0) return;
        if (tif == 0) {
            sink_.ouch_canceled(o.session, {ts, o.token, o.open, 'I'});
            return;
        }
        insert(o);
        sink_.itch_add({o.locate, ts, o.ref, o.side, o.open, lle::key_of_locate(o.locate), o.tick * 100});
    }

    void insert(const Order& o) {
        Book& b = books_[o.locate - 1];
        Queue& q = o.side == 'B' ? b.bids[o.tick] : b.asks[o.tick];
        q.push_back(o);
        by_ref_[o.ref] = Where{std::prev(q.end())};
        by_token_[key(o.session, o.token)] = o.ref;
    }

    void erase(std::uint64_t ref) {
        auto w = by_ref_.find(ref);
        Order& o = *w->second.it;
        Book& b = books_[o.locate - 1];
        by_token_.erase(key(o.session, o.token));
        if (o.side == 'B') {
            auto lvl = b.bids.find(o.tick);
            lvl->second.erase(w->second.it);
            if (lvl->second.empty()) b.bids.erase(lvl);
        } else {
            auto lvl = b.asks.find(o.tick);
            lvl->second.erase(w->second.it);
            if (lvl->second.empty()) b.asks.erase(lvl);
        }
        by_ref_.erase(w);
    }

    Sink& sink_;
    std::array<Book, lle::kNumSymbols> books_;
    std::unordered_map<std::uint64_t, Where> by_ref_;
    std::unordered_map<std::string, std::uint64_t> by_token_;
    std::uint64_t next_ref_ = 1;
    std::uint64_t next_match_ = 1;
};
