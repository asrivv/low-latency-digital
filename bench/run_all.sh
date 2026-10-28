#!/usr/bin/env bash
# Builds, tests, runs every benchmark and regenerates the README results table.
set -euo pipefail
cd "$(dirname "$0")/.."

RESULTS=bench/results
REL=build/release
DBG=build/debug
DURATION=${DURATION:-10}
PORT=15000

cmake -S . -B "$REL" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$REL"
ctest --test-dir "$REL" --output-on-failure
cmake -S . -B "$DBG" -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build "$DBG"
ctest --test-dir "$DBG" --output-on-failure

mkdir -p "$RESULTS"
LLE_RESULTS_DIR="$RESULTS" "$REL/test_differential" >/dev/null
"$REL/bench_engine" --out "$RESULTS/engine.json"
"$REL/bench_pipeline" --out "$RESULTS/pipeline.json"

LISTENER=
EXCHANGE=
cleanup() {
    [[ -n "$EXCHANGE" ]] && kill -INT "$EXCHANGE" 2>/dev/null || true
    [[ -n "$LISTENER" ]] && kill "$LISTENER" 2>/dev/null || true
}
trap cleanup EXIT

"$REL/itch_listener" --port 16000 --timeout 30 --out "$RESULTS/itch_listener.json" >/dev/null &
LISTENER=$!
sleep 0.5
"$REL/exchange" --port "$PORT" --md-addr 127.0.0.1 --md-port 16000 >"$RESULTS/exchange.json" &
EXCHANGE=$!
for _ in $(seq 50); do nc -z 127.0.0.1 "$PORT" 2>/dev/null && break; sleep 0.1; done

for spec in 100000:100k 500000:500k 1000000:1m 0:max; do
    rate=${spec%%:*}
    name=${spec##*:}
    "$REL/loadgen" --host 127.0.0.1 --port "$PORT" --sessions 4 --rate "$rate" --duration "$DURATION" \
        --seed 1 --out "$RESULTS/e2e_$name.json"
done

kill -INT "$EXCHANGE"
wait "$EXCHANGE"
EXCHANGE=
wait "$LISTENER"
LISTENER=

python3 - "$RESULTS/exchange.json" "$RESULTS/itch_listener.json" <<'PY'
import json, sys
ex = json.load(open(sys.argv[1]))
ls = json.load(open(sys.argv[2]))
print(f"exchange checksum {ex['book_checksum']}, listener checksum {ls['book_checksum']}, gaps {ls['gaps']}")
if ls["gaps"] == 0 and ex["book_checksum"] != ls["book_checksum"]:
    sys.exit("market data book does not match the exchange")
PY

python3 tools/report.py
