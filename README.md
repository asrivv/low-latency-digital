# low-latency-digital

## Results

<!-- results -->
Machine: Apple M4 Pro. Latencies in ns.

steady_clock on this machine advances in 41.67 ns ticks, so a single latency sample is a multiple of that tick; mean costs are total elapsed time divided by message count.

| Benchmark | Offered load (msg/s) | Achieved (msg/s) | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Engine, one call | replay | 14632141 | 83 | 125 | 167 | 250 | 375 | 73625 |
| Pipeline, decode to first response | 1000000 | 1000000 | 250 | 417 | 2383 | 8127 | 100863 | 752875 |
| Pipeline, decode to first response | 5000000 | 5000000 | 293 | 19711 | 393215 | 6225919 | 7241727 | 7663916 |
| Pipeline, decode to first response | 10000000 | 7192492 | 8519679 | 11403263 | 14155775 | 17694719 | 18874367 | 19145916 |
| Pipeline, decode to first response | unthrottled | 7117493 | 8585215 | 11665407 | 14024703 | 19136511 | 22806527 | 23024958 |
| Loopback TCP, send to first response | 100000 | 100000 | 38399 | 56319 | 65535 | 160767 | 1736703 | 2441792 |
| Loopback TCP, send to first response | 500000 | 500000 | 57087 | 81407 | 99839 | 509951 | 3457023 | 4273333 |
| Loopback TCP, send to first response | 1000000 | 1000000 | 63487 | 89599 | 189439 | 3096575 | 8716287 | 10844917 |
| Loopback TCP, send to first response | unthrottled | 6201025 | 96468991 | 226492415 | 295698431 | 381681663 | 499122175 | 505637625 |

Engine: 68.3 ns mean per message over 20000000 messages, 0 heap allocations in the measured window.
Market data: 101045384 ITCH messages, 0 gaps; the book rebuilt from ITCH has checksum `d1aec7d2d533b7dd`, which equals the exchange's.
<!-- /results -->
