#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <memory>
#include <random>

#include "lle/engine.hpp"
#include "lle/flow.hpp"
#include "recording_sink.hpp"
#include "reference_engine.hpp"

using namespace lle;

namespace {

constexpr int kSeeds = 200;
constexpr int kMessages = 50'000;
constexpr std::uint32_t kClients = 4;

// Occasionally corrupts a flow message so rejects, duplicates, partial cancels and IOC
// replaces are covered too; the flow model alone only produces well-formed traffic.
void mutate(FlowMsg& f, std::mt19937_64& rng, const Token& recent) {
    std::uint64_t r = rng() % 1000;
    switch (f.kind) {
        case FlowMsg::Kind::Enter:
            if (r < 3) f.enter.price += 50;
            else if (r < 5) f.enter.price = 100'001 * kTickSize;
            else if (r < 7) f.enter.shares = r & 1 ? 0 : 1'000'001;
            else if (r < 9) f.enter.side = r & 1 ? 'T' : 'E';
            else if (r < 11) f.enter.stock = symbol_key("IBM");
            else if (r < 16) f.enter.token = recent;
            break;
        case FlowMsg::Kind::Cancel:
            if (r < 200) f.cancel.shares = 100 * static_cast<std::uint32_t>(1 + rng() % 5);
            else if (r < 210) f.cancel.token = Token::from("00009999999999");
            break;
        case FlowMsg::Kind::Replace:
            if (r < 20) f.replace.price += 1;
            else if (r < 40) f.replace.shares = 0;
            else if (r < 60) f.replace.replacement = f.replace.existing;
            else if (r < 160) f.replace.tif = ouch::kTifIoc;
            else if (r < 300) f.replace.price = (r & 1 ? 1 : kMaxTick) * kTickSize;
            break;
    }
}

template <class E>
void dispatch(E& e, const FlowMsg& f, std::uint64_t ts) {
    SessionId s = static_cast<SessionId>(f.client + 1);
    switch (f.kind) {
        case FlowMsg::Kind::Enter: e.on_enter(EnterCmd{ts, s, f.enter}); break;
        case FlowMsg::Kind::Cancel: e.on_cancel(CancelCmd{ts, s, f.cancel}); break;
        case FlowMsg::Kind::Replace: e.on_replace(ReplaceCmd{ts, s, f.replace}); break;
    }
}

}  // namespace

TEST(Differential, EngineMatchesReference) {
    std::uint64_t total_events = 0;
    for (int seed = 1; seed <= kSeeds; ++seed) {
        RecordingSink es, rs;
        auto eng = std::make_unique<Engine<RecordingSink>>(es, 1u << 18);
        ReferenceEngine<RecordingSink> ref(rs);
        FlowModel flow(static_cast<std::uint64_t>(seed), kClients);
        std::mt19937_64 mut(static_cast<std::uint64_t>(seed) * 7919);
        Token recent = FlowModel::make_token(0, 1);
        for (int i = 0; i < kMessages; ++i) {
            FlowMsg f = flow.next();
            mutate(f, mut, recent);
            if (f.kind == FlowMsg::Kind::Enter) recent = f.enter.token;
            auto ts = static_cast<std::uint64_t>(i + 1);
            dispatch(*eng, f, ts);
            dispatch(ref, f, ts);
            if (es.events != rs.events) {
                std::size_t k = 0;
                while (k < es.events.size() && k < rs.events.size() && es.events[k] == rs.events[k]) ++k;
                std::string got = k < es.events.size() ? es.events[k].describe() : "<none>";
                std::string want = k < rs.events.size() ? rs.events[k].describe() : "<none>";
                FAIL() << "seed " << seed << " message " << i << " event " << k << "\n  engine:    " << got
                       << "\n  reference: " << want;
            }
            for (const auto& e : es.events)
                if (e.stream == 'o') flow.on_ouch(e.data());
            total_events += es.events.size();
            es.clear();
            rs.clear();
        }
        ASSERT_EQ(eng->book_checksum(), ref.book_checksum()) << "seed " << seed;
        ASSERT_EQ(eng->live_orders(), ref.live_orders()) << "seed " << seed;
    }
    std::printf("%d seeds x %d messages, %llu events compared\n", kSeeds, kMessages,
                static_cast<unsigned long long>(total_events));
    if (const char* dir = std::getenv("LLE_RESULTS_DIR")) {
        std::string path = std::string(dir) + "/differential.json";
        if (std::FILE* f = std::fopen(path.c_str(), "w")) {
            std::fprintf(f,
                         "{\"seeds\": %d, \"messages_per_seed\": %d, \"clients\": %u, \"events_compared\": %llu, "
                         "\"mismatches\": 0}\n",
                         kSeeds, kMessages, kClients, static_cast<unsigned long long>(total_events));
            std::fclose(f);
        }
    }
}
