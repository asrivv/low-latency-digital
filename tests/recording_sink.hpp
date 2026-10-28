#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "lle/itch.hpp"
#include "lle/ouch.hpp"

// Records every engine event as its encoded wire bytes, so two engines can be compared exactly.
struct RecordingSink {
    struct Event {
        char stream;  // 'o' OUCH, 'i' ITCH
        lle::SessionId session;
        std::string bytes;

        bool operator==(const Event&) const = default;
        char type() const { return bytes[0]; }
        const std::byte* data() const { return reinterpret_cast<const std::byte*>(bytes.data()); }
        std::string kind() const { return std::string{stream, type()}; }

        std::string describe() const {
            char buf[256];
            const std::byte* p = data();
            if (stream == 'o') {
                switch (type()) {
                    case 'A': {
                        auto m = lle::ouch::decode_accepted(p);
                        std::snprintf(buf, sizeof buf, "s%u OUCH Accepted %.14s %c %u @%u ref=%llu", session,
                                      m.token.b.data(), m.side, m.shares, m.price, (unsigned long long)m.ref);
                        break;
                    }
                    case 'E': {
                        auto m = lle::ouch::decode_executed(p);
                        std::snprintf(buf, sizeof buf, "s%u OUCH Executed %.14s %u @%u %c match=%llu", session,
                                      m.token.b.data(), m.shares, m.price, m.liquidity, (unsigned long long)m.match);
                        break;
                    }
                    case 'C': {
                        auto m = lle::ouch::decode_canceled(p);
                        std::snprintf(buf, sizeof buf, "s%u OUCH Canceled %.14s dec=%u %c", session, m.token.b.data(),
                                      m.decrement, m.reason);
                        break;
                    }
                    case 'U': {
                        auto m = lle::ouch::decode_replaced(p);
                        std::snprintf(buf, sizeof buf, "s%u OUCH Replaced %.14s<-%.14s %u @%u ref=%llu", session,
                                      m.token.b.data(), m.previous.b.data(), m.shares, m.price,
                                      (unsigned long long)m.ref);
                        break;
                    }
                    case 'J': {
                        auto m = lle::ouch::decode_rejected(p);
                        std::snprintf(buf, sizeof buf, "s%u OUCH Rejected %.14s %c", session, m.token.b.data(),
                                      m.reason);
                        break;
                    }
                    default: std::snprintf(buf, sizeof buf, "OUCH %c", type());
                }
            } else {
                switch (type()) {
                    case 'A': {
                        auto m = lle::itch::decode_add(p);
                        std::snprintf(buf, sizeof buf, "ITCH Add loc=%u ref=%llu %c %u @%u", m.locate,
                                      (unsigned long long)m.ref, m.side, m.shares, m.price);
                        break;
                    }
                    case 'E': {
                        auto m = lle::itch::decode_executed(p);
                        std::snprintf(buf, sizeof buf, "ITCH Executed loc=%u ref=%llu %u match=%llu", m.locate,
                                      (unsigned long long)m.ref, m.shares, (unsigned long long)m.match);
                        break;
                    }
                    case 'X': {
                        auto m = lle::itch::decode_cancel(p);
                        std::snprintf(buf, sizeof buf, "ITCH Cancel loc=%u ref=%llu %u", m.locate,
                                      (unsigned long long)m.ref, m.shares);
                        break;
                    }
                    case 'D': {
                        auto m = lle::itch::decode_delete(p);
                        std::snprintf(buf, sizeof buf, "ITCH Delete loc=%u ref=%llu", m.locate,
                                      (unsigned long long)m.ref);
                        break;
                    }
                    case 'U': {
                        auto m = lle::itch::decode_replace(p);
                        std::snprintf(buf, sizeof buf, "ITCH Replace loc=%u %llu->%llu %u @%u", m.locate,
                                      (unsigned long long)m.orig_ref, (unsigned long long)m.new_ref, m.shares, m.price);
                        break;
                    }
                    default: std::snprintf(buf, sizeof buf, "ITCH %c", type());
                }
            }
            return buf;
        }
    };

    std::vector<Event> events;

    template <class M>
    void put_ouch(lle::SessionId s, const M& m) {
        char buf[128];
        std::size_t n = lle::ouch::encode(m, reinterpret_cast<std::byte*>(buf));
        events.push_back({'o', s, std::string(buf, n)});
    }
    template <class M>
    void put_itch(const M& m) {
        char buf[64];
        std::size_t n = lle::itch::encode(m, reinterpret_cast<std::byte*>(buf));
        events.push_back({'i', 0, std::string(buf, n)});
    }

    void ouch_accepted(lle::SessionId s, const lle::ouch::Accepted& m) { put_ouch(s, m); }
    void ouch_executed(lle::SessionId s, const lle::ouch::Executed& m) { put_ouch(s, m); }
    void ouch_canceled(lle::SessionId s, const lle::ouch::Canceled& m) { put_ouch(s, m); }
    void ouch_replaced(lle::SessionId s, const lle::ouch::Replaced& m) { put_ouch(s, m); }
    void ouch_rejected(lle::SessionId s, const lle::ouch::Rejected& m) { put_ouch(s, m); }
    void itch_add(const lle::itch::AddOrder& m) { put_itch(m); }
    void itch_executed(const lle::itch::OrderExecuted& m) { put_itch(m); }
    void itch_cancel(const lle::itch::OrderCancel& m) { put_itch(m); }
    void itch_delete(const lle::itch::OrderDelete& m) { put_itch(m); }
    void itch_replace(const lle::itch::OrderReplace& m) { put_itch(m); }

    std::vector<std::string> kinds() const {
        std::vector<std::string> k;
        for (auto& e : events) k.push_back(e.kind());
        return k;
    }
    void clear() { events.clear(); }
};
