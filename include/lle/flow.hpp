#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include "lle/common.hpp"
#include "lle/ouch.hpp"

// Seeded synthetic order flow. Draws come straight from mt19937_64 rather than <random>
// distributions so a seed produces the same stream with libc++ and libstdc++.
namespace lle {

struct FlowMsg {
    enum class Kind : std::uint8_t { Enter, Cancel, Replace };
    Kind kind;
    std::uint32_t client;
    ouch::EnterOrder enter;
    ouch::CancelOrder cancel;
    ouch::ReplaceOrder replace;

    std::size_t encode(std::byte* out) const noexcept {
        switch (kind) {
            case Kind::Enter: return ouch::encode(enter, out);
            case Kind::Cancel: return ouch::encode(cancel, out);
            case Kind::Replace: return ouch::encode(replace, out);
        }
        return 0;
    }
};

inline constexpr std::array<std::uint32_t, kNumSymbols> kStartTicks = {19000, 42000, 12000, 18500,
                                                                       16500, 50000, 25000, 55000};

class FlowModel {
public:
    static constexpr double kWalkProb = 0.05;
    static constexpr double kNewDay = 0.55;
    static constexpr double kNewIoc = 0.05;
    static constexpr double kCancel = 0.35;
    static constexpr double kGeomP = 0.3;
    static constexpr std::uint32_t kDayTif = 99'999;

    FlowModel(std::uint64_t seed, std::uint32_t num_clients, std::uint32_t first_client = 0,
              std::size_t max_live = 50'000)
        : rng_(seed), clients_(num_clients), first_client_(first_client), max_live_(max_live) {
        ref_ticks_ = kStartTicks;
    }

    FlowMsg next() {
        std::uint32_t c = clients_.size() == 1 ? 0 : uniform(static_cast<std::uint32_t>(clients_.size()));
        auto sym = static_cast<std::uint8_t>(uniform(kNumSymbols));
        if (u01() < kWalkProb) {
            std::uint32_t& t = ref_ticks_[sym];
            if (rng_() & 1) {
                if (t < kMaxTick - 100) ++t;
            } else if (t > 100) {
                --t;
            }
        }
        Client& cl = clients_[c];
        double u = u01();
        if (u < kNewDay + kNewIoc) {
            bool ioc = u >= kNewDay;
            if (!ioc && cl.live.size() >= max_live_) return make_cancel(c);
            return make_enter(c, sym, ioc);
        }
        if (cl.live.empty()) return make_enter(c, sym, false);
        return u < kNewDay + kNewIoc + kCancel ? make_cancel(c) : make_replace(c);
    }

    // Feed back one OUCH message addressed to any client this model generates for.
    void on_ouch(const std::byte* msg) {
        switch (static_cast<char>(msg[0])) {
            case 'A': {
                auto m = ouch::decode_accepted(msg);
                if (m.tif != ouch::kTifIoc) add_live(m.token, m.stock, m.side, m.shares);
                break;
            }
            case 'E': {
                auto m = ouch::decode_executed(msg);
                reduce(m.token, m.shares);
                break;
            }
            case 'C': {
                auto m = ouch::decode_canceled(msg);
                reduce(m.token, m.decrement);
                break;
            }
            case 'U': {
                auto m = ouch::decode_replaced(msg);
                remove(m.previous);
                if (m.tif != ouch::kTifIoc) add_live(m.token, m.stock, m.side, m.shares);
                break;
            }
            default: break;
        }
    }

    std::size_t live(std::uint32_t client) const { return clients_[client].live.size(); }
    std::uint32_t ref_tick(std::size_t sym) const { return ref_ticks_[sym]; }

    // Full cancels for every order still believed live, for draining a book at the end of a run.
    std::vector<FlowMsg> cancel_all() {
        std::vector<FlowMsg> out;
        for (std::uint32_t c = 0; c < clients_.size(); ++c)
            while (!clients_[c].live.empty()) out.push_back(make_cancel(c));
        return out;
    }

    static Token make_token(std::uint32_t client, std::uint64_t seq) {
        Token t;
        for (int i = 3; i >= 0; --i, client /= 10) t.b[static_cast<std::size_t>(i)] = static_cast<char>('0' + client % 10);
        for (int i = 13; i >= 4; --i, seq /= 10) t.b[static_cast<std::size_t>(i)] = static_cast<char>('0' + seq % 10);
        return t;
    }

    static void parse_token(const Token& t, std::uint32_t& client, std::uint64_t& seq) {
        client = 0;
        seq = 0;
        for (std::size_t i = 0; i < 4; ++i) client = client * 10 + static_cast<std::uint32_t>(t.b[i] - '0');
        for (std::size_t i = 4; i < kTokenLen; ++i) seq = seq * 10 + static_cast<std::uint64_t>(t.b[i] - '0');
    }

private:
    struct Live {
        std::size_t pos;
        std::uint8_t sym;
        char side;
        std::uint32_t open;
    };

    struct Client {
        std::uint64_t next_seq = 1;
        std::vector<std::uint64_t> live;
        std::unordered_map<std::uint64_t, Live> orders;
    };

    double u01() { return static_cast<double>(rng_() >> 11) * 0x1.0p-53; }
    std::uint32_t uniform(std::uint32_t n) { return static_cast<std::uint32_t>((rng_() >> 32) * n >> 32); }
    std::uint32_t geometric() {
        return static_cast<std::uint32_t>(std::floor(std::log1p(-u01()) / std::log1p(-kGeomP)));
    }
    std::uint32_t shares() { return 100 * (1 + uniform(10)); }

    std::uint32_t passive_price(std::uint8_t sym, char side) {
        std::uint32_t off = 1 + geometric();
        std::uint32_t ref = ref_ticks_[sym];
        std::uint32_t t = side == 'B' ? (ref > off ? ref - off : 1) : (ref + off <= kMaxTick ? ref + off : kMaxTick);
        return t * kTickSize;
    }

    Token new_token(std::uint32_t c) { return make_token(first_client_ + c, clients_[c].next_seq++); }

    FlowMsg make_enter(std::uint32_t c, std::uint8_t sym, bool ioc) {
        FlowMsg f{};
        f.kind = FlowMsg::Kind::Enter;
        f.client = first_client_ + c;
        char side = (rng_() & 1) ? 'B' : 'S';
        std::uint32_t price;
        if (ioc) {
            std::uint32_t cross = 1 + uniform(3);
            std::uint32_t ref = ref_ticks_[sym];
            price = (side == 'B' ? ref + cross : ref - cross) * kTickSize;
        } else {
            price = passive_price(sym, side);
        }
        f.enter = ouch::EnterOrder{new_token(c), side, shares(), kSymbolKeys[sym], price, ioc ? ouch::kTifIoc : kDayTif,
                                   kFirm, 'Y', 'A', 'N', 0, 'N', 'R'};
        return f;
    }

    std::uint64_t take_random_live(std::uint32_t c) {
        Client& cl = clients_[c];
        std::size_t i = uniform(static_cast<std::uint32_t>(cl.live.size()));
        std::uint64_t seq = cl.live[i];
        detach(cl, seq);
        return seq;
    }

    FlowMsg make_cancel(std::uint32_t c) {
        FlowMsg f{};
        f.kind = FlowMsg::Kind::Cancel;
        f.client = first_client_ + c;
        std::uint64_t seq = take_random_live(c);
        f.cancel = ouch::CancelOrder{make_token(first_client_ + c, seq), 0};
        return f;
    }

    FlowMsg make_replace(std::uint32_t c) {
        FlowMsg f{};
        f.kind = FlowMsg::Kind::Replace;
        f.client = first_client_ + c;
        Client& cl = clients_[c];
        std::uint64_t seq = cl.live[uniform(static_cast<std::uint32_t>(cl.live.size()))];
        Live lv = cl.orders.at(seq);
        detach(cl, seq);
        f.replace = ouch::ReplaceOrder{make_token(first_client_ + c, seq), new_token(c), shares(),
                                       passive_price(lv.sym, lv.side), kDayTif, 'Y', 'N', 0};
        return f;
    }

    Client* owner(const Token& t, std::uint64_t& seq) {
        std::uint32_t client;
        parse_token(t, client, seq);
        if (client < first_client_ || client - first_client_ >= clients_.size()) return nullptr;
        return &clients_[client - first_client_];
    }

    void add_live(const Token& t, std::uint64_t stock, char side, std::uint32_t shares) {
        std::uint64_t seq;
        Client* cl = owner(t, seq);
        if (cl == nullptr) return;
        std::uint8_t loc = locate_of(stock);
        if (loc == 0) return;
        if (cl->orders.emplace(seq, Live{cl->live.size(), static_cast<std::uint8_t>(loc - 1), side, shares}).second)
            cl->live.push_back(seq);
    }

    void reduce(const Token& t, std::uint32_t by) {
        std::uint64_t seq;
        Client* cl = owner(t, seq);
        if (cl == nullptr) return;
        auto it = cl->orders.find(seq);
        if (it == cl->orders.end()) return;
        if (it->second.open <= by) detach(*cl, seq); else it->second.open -= by;
    }

    void remove(const Token& t) {
        std::uint64_t seq;
        Client* cl = owner(t, seq);
        if (cl != nullptr && cl->orders.count(seq)) detach(*cl, seq);
    }

    static void detach(Client& cl, std::uint64_t seq) {
        auto it = cl.orders.find(seq);
        std::size_t pos = it->second.pos;
        std::uint64_t last = cl.live.back();
        cl.live[pos] = last;
        cl.orders.at(last).pos = pos;
        cl.live.pop_back();
        cl.orders.erase(it);
    }

    static constexpr std::uint32_t kFirm = 0x4c4c4546;  // "LLEF"

    std::mt19937_64 rng_;
    std::array<std::uint32_t, kNumSymbols> ref_ticks_;
    std::vector<Client> clients_;
    std::uint32_t first_client_;
    std::size_t max_live_;
};

}  // namespace lle
