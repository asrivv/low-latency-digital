# low-latency-digital

A single-node equity exchange that speaks Nasdaq OUCH 4.2 over SoupBinTCP 3.0 for order entry and ITCH 5.0 over MoldUDP64 for market data. It has a price-time matching engine, an open-loop OUCH load generator, and a market data verifier that rebuilds every book from ITCH and checks it against the exchange.

## Results

Every number below is copied from `bench/results/*.json` by `tools/report.py`; `bench/run_all.sh` regenerates the whole set.

- **Engine:** `bench_engine` replays pre-decoded flow straight into `Engine` with a counting sink.
- **Pipeline:** `bench_pipeline` runs gateway framing and decode, the engine and an output consumer on three threads joined by the production rings, with no sockets involved.
- **Loopback TCP:** `loadgen` drives the real `exchange` binary with 4 sessions.

<!-- results -->
Machine: Apple M4 Pro. Latencies in ns.

steady_clock on this machine advances in 41.67 ns ticks, so a single latency sample is a multiple of that tick; mean costs are total elapsed time divided by message count.

| Benchmark | Offered load (msg/s) | Achieved (msg/s) | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Engine, one call | replay | 26670658 | 41 | 83 | 167 | 209 | 293 | 29500 |
| Pipeline, decode to first response | 1000000 | 1000000 | 250 | 459 | 3215 | 5983 | 11839 | 32375 |
| Pipeline, decode to first response | 5000000 | 5000000 | 250 | 1127 | 3423 | 13055 | 96767 | 306334 |
| Pipeline, decode to first response | 10000000 | 10000000 | 667 | 518143 | 5570559 | 10027007 | 10789417 | 10789417 |
| Pipeline, decode to first response | unthrottled | 11180432 | 5636095 | 6324223 | 10027007 | 10878975 | 11580666 | 11580666 |
| Loopback TCP, send to first response | 100000 | 100000 | 40447 | 57599 | 65023 | 137215 | 1220607 | 2050000 |
| Loopback TCP, send to first response | 500000 | 500000 | 60671 | 84991 | 100863 | 160767 | 1335295 | 1782958 |
| Loopback TCP, send to first response | 1000000 | 1000000 | 62975 | 87551 | 103935 | 602111 | 3276799 | 5960625 |
| Loopback TCP, send to first response | unthrottled | 10184103 | 7798783 | 27525119 | 123731967 | 189792255 | 205520895 | 208602875 |

Engine: 37.5 ns mean per message over 20000000 messages, 0 heap allocations in the measured window.
Market data: 156471623 ITCH messages, 0 gaps; the book rebuilt from ITCH has checksum `860a04e6b3178ed8`, which equals the exchange's.
<!-- /results -->

## Architecture

```
  loadgen                    exchange                                                   itch_listener
 +-----------+   TCP   +--------------+  inbound ring  +--------------+  md ring  +-----------+  UDP   +---------------+
 | session 0 |-------->|   gateway    |--------------->|    engine    |---------->| market    |------->| MoldUDP64     |
 | session 1 |  OUCH / | thread       |   Command      | thread       |  ITCH     | data      | ITCH / | parse, books  |
 |    ...    |  Soup   | busy poll,   |                | Engine<      |           | thread    | Mold   | from A/E/X/D/U|
 | (open     |<--------| SoupBin,     |<---------------| RingSink>    |           | packetize |        | checksum      |
 |  loop)    |         | decode OUCH  |  outbound ring |              |           | + pace    |        |               |
 +-----------+         +--------------+   OUCH + sess  +--------------+           +-----------+        +---------------+
```

Each ring is an `SpscRing` of 65536 slots of 128 bytes. The gateway is the only producer of the inbound ring and the only consumer of the outbound ring. The engine is the only consumer of the inbound ring and the only producer of the other two rings. Nothing on the path from socket read to socket write allocates, throws, locks, takes a virtual call or makes a syscall on the engine thread.

## Design decisions

- **Single-writer engine.** One thread owns every book, the order pool and both indices, so matching needs no locks or atomics. Parallelism comes from pipelining: framing on the gateway thread, encoding and sending on the market data thread.
- **Level arrays and a two-level bitmap.** Each side of each book is a flat array indexed by tick, with a bitmap of non-empty ticks and a summary bitmap of non-empty words. Recovering the best price after a level empties is a `countr_zero`/`countl_zero` over at most two words plus the summary. No code path scans levels.
- **Intrusive FIFO in a preallocated pool.** Orders are 64-byte records in one pool shared by all books, linked into per-level FIFOs by 32-bit indices. A LIFO free list hands back the most recently freed, still-cached slot.
- **Flat maps.** Both indices use open addressing with linear probing and backward-shift deletion. Order references are a dense counter, so their map uses the identity as hash and consecutive orders touch consecutive slots. Tokens hash `(session, token)`, and a hit is confirmed against the pool record. The engine loop prefetches the token slot of the command 4 positions ahead, because that probe is the one access per command that usually misses.
- **SPSC rings with cached indices.** Head and tail sit on separate cache lines. Each side keeps a private copy of the other's index and reloads the shared one only when its copy says full or empty. Only acquire/release ordering is used. The engine's sink writes every event of a command in place and publishes them with one release store per ring.
- **128-byte padding on Apple arm64.** On Apple Silicon a cache line is 128 bytes, so `kCacheLine` is 128 there and 64 elsewhere. Every `alignas` uses it.
- **Batched sends.** The gateway appends SoupBin frames to a per-session buffer and flushes each session with at most one `send` per pass. The market data thread fills MoldUDP64 packets and sends a partial one only once the ring is empty. Both are paced by a GCRA (`SendPacer`): an idle socket writes immediately, and a busy one writes once per interval. I added the pacer after measuring: macOS loopback stalls into a standing queue of tens of milliseconds once it carries roughly one packet per message in each direction, well before any thread in the exchange is busy.
- **Open-loop load generation.** Every order has a send time fixed in advance, and latency runs from that scheduled time to the first response (Accepted, Rejected, Canceled or Replaced). A stalled exchange therefore shows up as latency rather than as a lower send rate, which avoids coordinated omission. Responses feed back into the flow model, so cancels and replaces target live orders.

## Correctness

<!-- correctness -->
- Differential test: 200 seeds of 50000 flow messages from 4 clients, a small fraction mutated into rejects, duplicate tokens, partial cancels and IOC replaces, go through both the engine and `tests/reference_engine.hpp` (std::map levels, std::list FIFOs). Every event must match byte for byte, and the final book checksums must agree; 24926544 events were compared, 0 mismatches.
- Market data: `itch_listener` rebuilt the books from 156471623 ITCH messages with 0 sequence gaps across the whole loopback run and computed the same FNV-1a checksum the exchange prints at shutdown. `run_all.sh` fails if they differ while gaps is 0.
<!-- /correctness -->
- Unit tests cover price-time priority, partial fills across levels, IOC remainders, cancel-to-reduce keeping priority, cancel to zero, replace losing priority, crossing replaces, every reject reason, duplicate tokens, pool slot reuse and best-price recovery after sweeping many levels. A loopback test checks exact SoupBin/OUCH bytes through the real gateway. `ctest` runs everything under both Release and Debug with AddressSanitizer and UBSan.
- `bench_engine` and `bench_pipeline` replace the global `operator new` and fail if anything allocates inside the measured window.

## Simplifications

- A replace always loses time priority, even when only the size goes down.
- No session replay. The requested sequence number in a Login Request is ignored and every session starts at sequence 1.
- No trading halts, auctions or crossing sessions.
- Displayed limit orders only. Display, minimum quantity, intermarket sweep and cross type are echoed back but do not change matching.
- A single node: no persistence, no failover, no drop copy.
- If the order pool is full, a new order is rejected with reason `O`.

## Build and run

Requirements: CMake, Ninja, a C++20 compiler (Apple clang on macOS, clang or gcc on Linux) and Python 3. GoogleTest is fetched by CMake.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build

cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug   # ASan + UBSan
cmake --build build/debug && ctest --test-dir build/debug
```

Run the system by hand:

```sh
./build/itch_listener --port 16000 &
./build/exchange --port 15000 --md-addr 127.0.0.1 --md-port 16000 &
./build/loadgen --host 127.0.0.1 --port 15000 --sessions 4 --rate 1000000 --duration 10 --seed 1 --out bench/results/e2e_1m.json
kill -INT %2   # exchange prints its book checksum and counters as one JSON line
```

Linux also accepts `--pin-gateway N --pin-engine N --pin-md N`. On macOS, hot threads get `QOS_CLASS_USER_INTERACTIVE` instead.

Run everything: build both configurations, test, run both benchmarks, run the loopback runs at 100000, 500000 and 1000000 msg/s plus unthrottled, check the market data checksum, and regenerate the tables:

```sh
bench/run_all.sh
```
