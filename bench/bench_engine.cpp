#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "alloc_guard.hpp"
#include "corpus.hpp"
#include "lle/engine.hpp"
#include "lle/histogram.hpp"
#include "lle/sysinfo.hpp"

using namespace lle;

namespace {

struct CountingSink {
    std::uint64_t accepted = 0, executed = 0, canceled = 0, replaced = 0, rejected = 0;
    std::uint64_t add = 0, itch_executed_n = 0, cancel = 0, del = 0, replace = 0;

    void ouch_accepted(SessionId, const ouch::Accepted&) { ++accepted; }
    void ouch_executed(SessionId, const ouch::Executed&) { ++executed; }
    void ouch_canceled(SessionId, const ouch::Canceled&) { ++canceled; }
    void ouch_replaced(SessionId, const ouch::Replaced&) { ++replaced; }
    void ouch_rejected(SessionId, const ouch::Rejected&) { ++rejected; }
    void itch_add(const itch::AddOrder&) { ++add; }
    void itch_executed(const itch::OrderExecuted&) { ++itch_executed_n; }
    void itch_cancel(const itch::OrderCancel&) { ++cancel; }
    void itch_delete(const itch::OrderDelete&) { ++del; }
    void itch_replace(const itch::OrderReplace&) { ++replace; }
};

struct Cmd {
    Command::Kind kind;
    union {
        EnterCmd enter;
        CancelCmd cancel;
        ReplaceCmd replace;
    };
};

constexpr std::size_t kAhead = kPrefetchAhead;

template <class E>
inline void prefetch(E& e, const Cmd& c) {
    switch (c.kind) {
        case Command::Kind::Enter: e.prefetch(c.enter.session, c.enter.msg.token); break;
        case Command::Kind::Cancel: e.prefetch(c.cancel.session, c.cancel.msg.token); break;
        case Command::Kind::Replace: e.prefetch(c.replace.session, c.replace.msg.existing); break;
    }
}

template <class E>
inline void apply(E& e, const Cmd& c) {
    switch (c.kind) {
        case Command::Kind::Enter: e.on_enter(c.enter); break;
        case Command::Kind::Cancel: e.on_cancel(c.cancel); break;
        case Command::Kind::Replace: e.on_replace(c.replace); break;
    }
}

double clock_overhead_ns() {
    constexpr int kN = 10'000'000;
    std::int64_t t0 = now_ns();
    std::uint64_t x = 0;
    for (int i = 0; i < kN; ++i) x ^= static_cast<std::uint64_t>(now_ns());
    std::int64_t t1 = now_ns();
    if (x == 42) std::puts("");
    return static_cast<double>(t1 - t0) / kN;
}

struct Args {
    std::size_t warmup = 1'000'000;
    std::size_t messages = 20'000'000;
    std::uint64_t seed = 1;
    std::uint32_t clients = 4;
    std::string out = "bench/results/engine.json";
};

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        const char* v = argv[i + 1];
        if (k == "--warmup") a.warmup = std::strtoull(v, nullptr, 10);
        else if (k == "--messages") a.messages = std::strtoull(v, nullptr, 10);
        else if (k == "--seed") a.seed = std::strtoull(v, nullptr, 10);
        else if (k == "--clients") a.clients = static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10));
        else if (k == "--out") a.out = v;
        else {
            std::fprintf(stderr, "unknown flag %s\n", k.c_str());
            std::exit(2);
        }
    }
    return a;
}

}  // namespace

int main(int argc, char** argv) {
    Args args = parse(argc, argv);
    make_thread_hot(-1);

    std::size_t total = args.warmup + args.messages;
    std::vector<Cmd> cmds;
    cmds.reserve(total);
    std::fprintf(stderr, "generating %zu messages\n", total);
    bench::generate_flow(args.seed, args.clients, total, false, [&](const FlowMsg& f) {
        Cmd c;
        std::uint64_t ts = cmds.size() + 1;
        SessionId s = bench::session_of(f);
        switch (f.kind) {
            case FlowMsg::Kind::Enter: c.kind = Command::Kind::Enter; c.enter = {ts, s, f.enter}; break;
            case FlowMsg::Kind::Cancel: c.kind = Command::Kind::Cancel; c.cancel = {ts, s, f.cancel}; break;
            case FlowMsg::Kind::Replace: c.kind = Command::Kind::Replace; c.replace = {ts, s, f.replace}; break;
        }
        cmds.push_back(c);
    });

    // Run 1: throughput and mean cost from total elapsed time, no per-message clock reads.
    CountingSink sink;
    auto eng = std::make_unique<Engine<CountingSink>>(sink);
    for (std::size_t i = 0; i < args.warmup; ++i) apply(*eng, cmds[i]);
    std::uint64_t allocs0 = bench::allocations();
    std::int64_t t0 = now_ns();
    for (std::size_t i = args.warmup; i < total; ++i) {
        if (i + kAhead < total) prefetch(*eng, cmds[i + kAhead]);
        apply(*eng, cmds[i]);
    }
    std::int64_t t1 = now_ns();
    std::uint64_t allocs_throughput = bench::allocations() - allocs0;
    std::uint64_t checksum = eng->book_checksum();
    std::size_t live_end = eng->live_orders();
    std::uint64_t matches = eng->matches();
    CountingSink counts = sink;
    eng.reset();

    // Run 2: same stream into a fresh engine with steady_clock around every call.
    CountingSink sink2;
    auto eng2 = std::make_unique<Engine<CountingSink>>(sink2);
    auto hist = std::make_unique<Histogram>();
    for (std::size_t i = 0; i < args.warmup; ++i) apply(*eng2, cmds[i]);
    allocs0 = bench::allocations();
    for (std::size_t i = args.warmup; i < total; ++i) {
        if (i + kAhead < total) prefetch(*eng2, cmds[i + kAhead]);
        std::int64_t a = now_ns();
        apply(*eng2, cmds[i]);
        std::int64_t b = now_ns();
        hist->record(static_cast<std::uint64_t>(b - a));
    }
    std::uint64_t allocs_latency = bench::allocations() - allocs0;
    bool same = eng2->book_checksum() == checksum;
    eng2.reset();

    double elapsed = static_cast<double>(t1 - t0);
    double mean_ns = elapsed / static_cast<double>(args.messages);
    auto throughput = static_cast<std::uint64_t>(static_cast<double>(args.messages) * 1e9 / elapsed);
    double overhead = clock_overhead_ns();
    std::string host = host_description();
    double tick = timer_tick_ns();

    std::printf("engine: %llu msgs/s, %.1f ns/msg mean over %zu messages\n",
                static_cast<unsigned long long>(throughput), mean_ns, args.messages);
    std::printf("per-call latency (timer tick %.2f ns): %s\n", tick, hist->json().c_str());
    std::printf("allocations during measurement: %llu (throughput run), %llu (latency run)\n",
                static_cast<unsigned long long>(allocs_throughput), static_cast<unsigned long long>(allocs_latency));

    std::FILE* f = std::fopen(args.out.c_str(), "w");
    if (f == nullptr) {
        std::perror(args.out.c_str());
        return 1;
    }
    std::fprintf(f,
                 "{\n  \"bench\": \"engine\",\n  \"host\": \"%s\",\n  \"timer_tick_ns\": %.2f,\n"
                 "  \"clock_read_ns\": %.1f,\n  \"cache_line\": %zu,\n  \"symbols\": %zu,\n  \"pool_capacity\": %u,\n"
                 "  \"order_record_bytes\": %zu,\n  \"prefetch_ahead\": %zu,\n  \"seed\": %llu,\n  \"clients\": %u,\n"
                 "  \"warmup_messages\": %zu,\n  \"measured_messages\": %zu,\n"
                 "  \"throughput_msgs_per_s\": %llu,\n  \"mean_ns_per_msg\": %.1f,\n"
                 "  \"latency\": {%s},\n  \"allocations_during_measurement\": %llu,\n"
                 "  \"events\": {\"accepted\": %llu, \"executed\": %llu, \"canceled\": %llu, \"replaced\": %llu, "
                 "\"rejected\": %llu, \"itch_add\": %llu, \"itch_executed\": %llu, \"itch_cancel\": %llu, "
                 "\"itch_delete\": %llu, \"itch_replace\": %llu},\n"
                 "  \"matches\": %llu,\n  \"live_orders_end\": %zu,\n  \"book_checksum\": \"%016llx\"\n}\n",
                 host.c_str(), tick, overhead, kCacheLine, kNumSymbols, Engine<CountingSink>::kDefaultPool,
                 sizeof(Order), kPrefetchAhead, static_cast<unsigned long long>(args.seed), args.clients, args.warmup,
                 args.messages, static_cast<unsigned long long>(throughput), mean_ns, hist->json_fields().c_str(),
                 static_cast<unsigned long long>(allocs_throughput + allocs_latency),
                 static_cast<unsigned long long>(counts.accepted), static_cast<unsigned long long>(counts.executed),
                 static_cast<unsigned long long>(counts.canceled), static_cast<unsigned long long>(counts.replaced),
                 static_cast<unsigned long long>(counts.rejected), static_cast<unsigned long long>(counts.add),
                 static_cast<unsigned long long>(counts.itch_executed_n),
                 static_cast<unsigned long long>(counts.cancel), static_cast<unsigned long long>(counts.del),
                 static_cast<unsigned long long>(counts.replace), static_cast<unsigned long long>(matches), live_end,
                 static_cast<unsigned long long>(checksum));
    std::fclose(f);

    if (allocs_throughput + allocs_latency != 0) {
        std::fprintf(stderr, "FAIL: heap allocation inside the measured window\n");
        return 1;
    }
    if (!same) {
        std::fprintf(stderr, "FAIL: runs diverged\n");
        return 1;
    }
    return 0;
}
