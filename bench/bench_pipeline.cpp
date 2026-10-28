#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "alloc_guard.hpp"
#include "corpus.hpp"
#include "gateway.hpp"
#include "lle/engine.hpp"
#include "lle/histogram.hpp"
#include "lle/soupbin.hpp"
#include "lle/sysinfo.hpp"

using namespace lle;

namespace {

constexpr std::uint32_t kClients = 4;

// Pre-encoded SoupBin 'U' frames. The flow ends by cancelling everything it left resting, so
// the corpus can be replayed back to back, each pass under fresh session ids.
struct Corpus {
    std::vector<std::byte> bytes;
    std::vector<std::uint32_t> offset;
    std::vector<std::uint8_t> client;

    std::size_t size() const { return client.size(); }
};

Corpus build_corpus(std::uint64_t seed, std::size_t n) {
    Corpus c;
    c.bytes.reserve(n * 48);
    c.offset.reserve(n + n / 8);
    c.client.reserve(n + n / 8);
    bench::generate_flow(seed, kClients, n, true, [&](const FlowMsg& f) {
        std::byte msg[64];
        std::size_t len = f.encode(msg);
        std::size_t at = c.bytes.size();
        c.offset.push_back(static_cast<std::uint32_t>(at));
        c.client.push_back(static_cast<std::uint8_t>(f.client));
        c.bytes.resize(at + soupbin::kHeaderSize + len);
        soupbin::encode_frame(c.bytes.data() + at, 'U', msg, len);
    });
    c.offset.push_back(static_cast<std::uint32_t>(c.bytes.size()));
    return c;
}

struct Result {
    std::uint64_t target_rate;
    double achieved_rate;
    double response_rate;
    std::uint64_t commands;
    std::uint64_t first_responses;
    std::uint64_t allocations;
    Histogram hist;
};

struct Run {
    const Corpus& corpus;
    std::uint64_t rate;
    std::int64_t warmup_ns;
    std::int64_t measure_ns;
};

void run_one(const Run& run, Result& res) {
    auto in = std::make_unique<InboundRing>();
    auto out = std::make_unique<OutboundRing>();
    auto md = std::make_unique<MdRing>();
    RingSink sink(*out, *md);
    auto engine = std::make_unique<Engine<RingSink>>(sink);

    std::atomic<bool> producer_done{false}, engine_done{false};
    std::atomic<int> ready{0};
    std::int64_t offset = midnight_offset_ns();
    const std::int64_t start = now_ns() + 200'000'000;
    const std::int64_t w0 = start + run.warmup_ns, w1 = w0 + run.measure_ns;
    std::uint64_t in_window = 0, responses_in_window = 0, allocs0 = 0, allocs1 = 0;

    std::thread engine_thread([&] {
        make_thread_hot(-1);
        ready.fetch_add(1);
        for (;;) {
            if (step_engine(*engine, sink, *in)) continue;
            if (producer_done.load(std::memory_order_acquire) && in->peek() == nullptr) break;
            cpu_relax();
        }
        engine_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        make_thread_hot(-1);
        ready.fetch_add(1);
        std::uint64_t last_seq = 0;
        for (;;) {
            bool idle = true;
            while (OutMsg* m = out->peek()) {
                idle = false;
                if (m->cmd_seq != last_seq) {
                    last_seq = m->cmd_seq;
                    if (m->t_origin >= w0 && m->t_origin < w1) {
                        res.hist.record(static_cast<std::uint64_t>(now_ns() - m->t_origin));
                        ++responses_in_window;
                    }
                }
                out->release();
            }
            while (md->peek() != nullptr) {
                idle = false;
                md->release();
            }
            if (idle) {
                if (engine_done.load(std::memory_order_acquire) && out->empty() && md->empty()) break;
                cpu_relax();
            }
        }
    });

    // Producer: the calling thread. Paced open-loop against the schedule, or flat out when rate is 0.
    make_thread_hot(-1);
    while (ready.load() < 2) cpu_relax();
    const Corpus& c = run.corpus;
    const std::size_t n = c.size();
    std::uint64_t i = 0, seq = 0;
    bool counting = false;
    while (now_ns() < start) cpu_relax();
    for (;;) {
        std::int64_t now = now_ns();
        if (now >= w1) break;
        if (!counting && now >= w0) {
            counting = true;
            allocs0 = bench::allocations();
        }
        std::uint64_t due = run.rate ? static_cast<std::uint64_t>(static_cast<double>(now - start) *
                                                                  static_cast<double>(run.rate) * 1e-9) + 1
                                     : i + 64;
        if (i >= due) {
            cpu_relax();
            continue;
        }
        for (; i < due; ++i) {
            std::size_t k = i % n;
            auto session = static_cast<SessionId>((i / n) * kClients + c.client[k] + 1);
            soupbin::parse(c.bytes.data() + c.offset[k], c.offset[k + 1] - c.offset[k],
                           [&](char type, const std::byte* p, std::size_t len) {
                               if (type != 'U') return true;
                               Command* cmd;
                               while ((cmd = in->claim()) == nullptr) cpu_relax();
                               std::int64_t t = now_ns();
                               if (!decode_command(p, len, session, since_midnight(t, offset), *cmd)) return true;
                               cmd->t_decode = t;
                               cmd->seq = ++seq;
                               in->commit();
                               if (t >= w0 && t < w1) ++in_window;
                               return true;
                           });
        }
    }
    allocs1 = bench::allocations();
    producer_done.store(true, std::memory_order_release);
    engine_thread.join();
    consumer.join();

    double secs = static_cast<double>(run.measure_ns) * 1e-9;
    res.target_rate = run.rate;
    res.commands = in_window;
    res.first_responses = responses_in_window;
    res.achieved_rate = static_cast<double>(in_window) / secs;
    res.response_rate = static_cast<double>(responses_in_window) / secs;
    res.allocations = allocs1 - allocs0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_path = "bench/results/pipeline.json";
    double warmup = 2.0, duration = 10.0;
    std::size_t corpus_size = 4'000'000;
    std::vector<std::uint64_t> rates = {1'000'000, 5'000'000, 10'000'000, 0};
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        const char* v = argv[i + 1];
        if (k == "--out") out_path = v;
        else if (k == "--warmup") warmup = std::atof(v);
        else if (k == "--duration") duration = std::atof(v);
        else if (k == "--corpus") corpus_size = std::strtoull(v, nullptr, 10);
        else if (k == "--rates") {
            rates.clear();
            for (const char* p = v; *p;) {
                char* end;
                rates.push_back(std::strtoull(p, &end, 10));
                p = *end == ',' ? end + 1 : end;
            }
        } else {
            std::fprintf(stderr, "unknown flag %s\n", k.c_str());
            return 2;
        }
    }

    std::fprintf(stderr, "building corpus of %zu messages\n", corpus_size);
    Corpus corpus = build_corpus(1, corpus_size);

    std::vector<std::unique_ptr<Result>> results;
    std::uint64_t total_allocs = 0;
    for (std::uint64_t rate : rates) {
        auto r = std::make_unique<Result>();
        run_one(Run{corpus, rate, static_cast<std::int64_t>(warmup * 1e9), static_cast<std::int64_t>(duration * 1e9)},
                *r);
        std::printf("rate %llu: achieved %.0f cmd/s, %s, allocations %llu\n",
                    static_cast<unsigned long long>(rate), r->achieved_rate, r->hist.json().c_str(),
                    static_cast<unsigned long long>(r->allocations));
        std::fflush(stdout);
        total_allocs += r->allocations;
        results.push_back(std::move(r));
    }

    std::FILE* f = std::fopen(out_path.c_str(), "w");
    if (f == nullptr) {
        std::perror(out_path.c_str());
        return 1;
    }
    std::fprintf(f,
                 "{\n  \"bench\": \"pipeline\",\n  \"host\": \"%s\",\n  \"timer_tick_ns\": %.2f,\n"
                 "  \"warmup_s\": %.1f,\n  \"duration_s\": %.1f,\n  \"corpus_messages\": %zu,\n"
                 "  \"ring_slots\": %zu,\n  \"runs\": [\n",
                 host_description().c_str(), timer_tick_ns(), warmup, duration, corpus.size(), kRingSlots);
    for (std::size_t i = 0; i < results.size(); ++i) {
        const Result& r = *results[i];
        std::fprintf(f,
                     "    {\"target_rate\": %llu, \"achieved_rate\": %.0f, \"response_rate\": %.0f, "
                     "\"commands\": %llu, \"first_responses\": %llu, \"allocations_during_measurement\": %llu, "
                     "\"latency\": {%s}}%s\n",
                     static_cast<unsigned long long>(r.target_rate), r.achieved_rate, r.response_rate,
                     static_cast<unsigned long long>(r.commands), static_cast<unsigned long long>(r.first_responses),
                     static_cast<unsigned long long>(r.allocations), r.hist.json_fields().c_str(),
                     i + 1 < results.size() ? "," : "");
    }
    std::fprintf(f, "  ]\n}\n");
    std::fclose(f);

    if (total_allocs != 0) {
        std::fprintf(stderr, "FAIL: heap allocation inside the measured window\n");
        return 1;
    }
    return 0;
}
