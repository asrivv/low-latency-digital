#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "lle/engine.hpp"
#include "lle/flow.hpp"

namespace lle::bench {

struct FeedbackSink {
    FlowModel* flow;

    template <class M>
    void feed(const M& m) {
        std::byte buf[ouch::kMaxOutboundSize];
        ouch::encode(m, buf);
        flow->on_ouch(buf);
    }
    void ouch_accepted(SessionId, const ouch::Accepted& m) { feed(m); }
    void ouch_executed(SessionId, const ouch::Executed& m) { feed(m); }
    void ouch_canceled(SessionId, const ouch::Canceled& m) { feed(m); }
    void ouch_replaced(SessionId, const ouch::Replaced& m) { feed(m); }
    void ouch_rejected(SessionId, const ouch::Rejected&) {}
    void itch_add(const itch::AddOrder&) {}
    void itch_executed(const itch::OrderExecuted&) {}
    void itch_cancel(const itch::OrderCancel&) {}
    void itch_delete(const itch::OrderDelete&) {}
    void itch_replace(const itch::OrderReplace&) {}
};

inline SessionId session_of(const FlowMsg& f) { return static_cast<SessionId>(f.client + 1); }

// Runs the flow model against a private engine so that cancels and replaces target orders
// that are actually live, and hands each message to `out` in order. With drain, finishes by
// cancelling everything the model still believes is resting.
template <class Out>
void generate_flow(std::uint64_t seed, std::uint32_t clients, std::size_t n, bool drain, Out&& out) {
    FlowModel flow(seed, clients);
    FeedbackSink sink{&flow};
    auto eng = std::make_unique<Engine<FeedbackSink>>(sink);
    auto apply = [&](const FlowMsg& f, std::uint64_t ts) {
        SessionId s = session_of(f);
        switch (f.kind) {
            case FlowMsg::Kind::Enter: eng->on_enter(EnterCmd{ts, s, f.enter}); break;
            case FlowMsg::Kind::Cancel: eng->on_cancel(CancelCmd{ts, s, f.cancel}); break;
            case FlowMsg::Kind::Replace: eng->on_replace(ReplaceCmd{ts, s, f.replace}); break;
        }
        out(f);
    };
    for (std::size_t i = 0; i < n; ++i) apply(flow.next(), i + 1);
    if (drain)
        for (const FlowMsg& f : flow.cancel_all()) apply(f, n + 1);
}

}  // namespace lle::bench
