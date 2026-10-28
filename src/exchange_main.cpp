#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <pthread.h>
#include <thread>

#include "gateway.hpp"
#include "lle/engine.hpp"
#include "lle/itch.hpp"
#include "md_publisher.hpp"

using namespace lle;

namespace {

struct Options {
    std::uint16_t port = 15000;
    const char* md_addr = "127.0.0.1";
    std::uint16_t md_port = 16000;
    int pin_gateway = -1;
    int pin_engine = -1;
    int pin_md = -1;
    std::int64_t gw_pace_ns = Gateway::kDefaultPaceNs;
    std::int64_t md_pace_ns = MdPublisher::kDefaultPaceNs;
};

[[noreturn]] void usage() {
    std::fprintf(stderr,
                 "usage: exchange [--port N] [--md-addr A] [--md-port N] [--pace-us N] [--md-pace-us N]"
#if defined(__linux__)
                 " [--pin-gateway CPU] [--pin-engine CPU] [--pin-md CPU]"
#endif
                 "\n");
    std::exit(2);
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const char* k = argv[i];
        if (i + 1 >= argc) usage();
        const char* v = argv[++i];
        if (!std::strcmp(k, "--port")) o.port = static_cast<std::uint16_t>(std::atoi(v));
        else if (!std::strcmp(k, "--md-addr")) o.md_addr = v;
        else if (!std::strcmp(k, "--md-port")) o.md_port = static_cast<std::uint16_t>(std::atoi(v));
        else if (!std::strcmp(k, "--pace-us")) o.gw_pace_ns = std::atoll(v) * 1000;
        else if (!std::strcmp(k, "--md-pace-us")) o.md_pace_ns = std::atoll(v) * 1000;
#if defined(__linux__)
        else if (!std::strcmp(k, "--pin-gateway")) o.pin_gateway = std::atoi(v);
        else if (!std::strcmp(k, "--pin-engine")) o.pin_engine = std::atoi(v);
        else if (!std::strcmp(k, "--pin-md")) o.pin_md = std::atoi(v);
#endif
        else usage();
    }
    return o;
}

void system_event(RingSink& sink, std::int64_t offset, char code) {
    sink.put_itch(itch::SystemEvent{since_midnight(now_ns(), offset), code});
}

}  // namespace

int main(int argc, char** argv) {
    Options opt = parse(argc, argv);

    // Signals are taken synchronously by main via sigwait; no thread ever runs a handler.
    // Dispositions are reset first: a background job started by a non-interactive shell
    // inherits SIGINT as ignored, and an ignored signal is discarded rather than left pending.
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    auto in = std::make_unique<InboundRing>();
    auto out = std::make_unique<OutboundRing>();
    auto md = std::make_unique<MdRing>();
    RingSink sink(*out, *md);
    auto engine = std::make_unique<Engine<RingSink>>(sink);
    auto gateway = std::make_unique<Gateway>(*in, *out);
    auto publisher = std::make_unique<MdPublisher>(*md);

    gateway->set_pace_ns(opt.gw_pace_ns);
    publisher->set_pace_ns(opt.md_pace_ns);
    if (!gateway->listen(opt.port)) {
        std::perror("listen");
        return 1;
    }
    if (!publisher->open(opt.md_addr, opt.md_port)) {
        std::perror("market data socket");
        return 1;
    }

    // Main is the market data producer until the engine thread starts, and again after it joins.
    std::int64_t offset = midnight_offset_ns();
    system_event(sink, offset, 'O');
    for (std::uint8_t loc = 1; loc <= kNumSymbols; ++loc) {
        itch::StockDirectory d;
        d.locate = loc;
        d.timestamp = since_midnight(now_ns(), offset);
        d.stock = key_of_locate(loc);
        sink.put_itch(d);
    }
    system_event(sink, offset, 'Q');

    std::atomic<bool> stop_input{false}, input_stopped{false}, stop_gateway{false};
    std::atomic<bool> stop_engine{false}, stop_md{false};

    std::thread gw_thread([&] {
        make_thread_hot(opt.pin_gateway);
        while (!stop_gateway.load(std::memory_order_acquire)) {
            if (stop_input.load(std::memory_order_relaxed) && !input_stopped.load(std::memory_order_relaxed)) {
                gateway->stop_input();
                input_stopped.store(true, std::memory_order_release);
            }
            gateway->poll();
        }
        gateway->finish();
    });

    std::thread engine_thread([&] {
        make_thread_hot(opt.pin_engine);
        for (;;) {
            if (step_engine(*engine, sink, *in)) continue;
            if (stop_engine.load(std::memory_order_acquire) && in->peek() == nullptr) break;
            cpu_relax();
        }
    });

    std::thread md_thread([&] {
        make_thread_hot(opt.pin_md);
        while (!stop_md.load(std::memory_order_acquire)) publisher->poll();
        publisher->finish();
    });

    std::fprintf(stderr, "exchange: order entry on port %u, market data to %s:%u\n", gateway->port(), opt.md_addr,
                 opt.md_port);

    int sig = 0;
    sigwait(&sigs, &sig);

    stop_input.store(true, std::memory_order_relaxed);
    while (!input_stopped.load(std::memory_order_acquire)) std::this_thread::yield();
    stop_engine.store(true, std::memory_order_release);
    engine_thread.join();

    system_event(sink, offset, 'M');
    system_event(sink, offset, 'C');
    stop_md.store(true, std::memory_order_release);
    md_thread.join();

    stop_gateway.store(true, std::memory_order_release);
    gw_thread.join();

    const EngineStats& es = engine->stats();
    const GatewayStats& gs = gateway->stats();
    const MdStats& ms = publisher->stats();
    std::printf("{\"book_checksum\": \"%016llx\", \"commands\": %llu, \"enters\": %llu, \"cancels\": %llu, "
                "\"replaces\": %llu, \"rejects\": %llu, \"ignored\": %llu, \"matches\": %llu, \"live_orders\": %zu, "
                "\"ouch_out\": %llu, \"itch_out\": %llu, \"md_messages\": %llu, \"md_packets\": %llu, "
                "\"md_send_retries\": %llu, \"sessions\": %llu, \"frames_out\": %llu, \"dropped_out\": %llu}\n",
                static_cast<unsigned long long>(engine->book_checksum()),
                static_cast<unsigned long long>(gs.commands), static_cast<unsigned long long>(es.enters),
                static_cast<unsigned long long>(es.cancels), static_cast<unsigned long long>(es.replaces),
                static_cast<unsigned long long>(es.rejects), static_cast<unsigned long long>(es.ignored),
                static_cast<unsigned long long>(engine->matches()), engine->live_orders(),
                static_cast<unsigned long long>(sink.ouch_out()), static_cast<unsigned long long>(sink.itch_out()),
                static_cast<unsigned long long>(ms.messages), static_cast<unsigned long long>(ms.packets),
                static_cast<unsigned long long>(ms.send_retries),
                static_cast<unsigned long long>(gs.sessions_accepted), static_cast<unsigned long long>(gs.frames_out),
                static_cast<unsigned long long>(gs.dropped_out));
    std::fflush(stdout);
    return 0;
}
