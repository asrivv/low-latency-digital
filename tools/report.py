#!/usr/bin/env python3
"""Renders bench/results/*.json into the marked blocks of README.md.

Every number in a block is copied verbatim from the JSON text. Afterwards every number in the
whole README is checked against the results files, and the script fails if one is missing.
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RESULTS = ROOT / "bench" / "results"
README = ROOT / "README.md"
PCTS = ["p50_ns", "p90_ns", "p99_ns", "p99_9_ns", "p99_99_ns", "max_ns"]
E2E = ["100k", "500k", "1m", "max"]


def load(name):
    path = RESULTS / name
    return json.loads(path.read_text()) if path.exists() else None


def num(v):
    return json.dumps(v)


def row(label, load_, achieved, lat):
    cells = [label, load_, num(achieved)] + [num(lat[p]) for p in PCTS]
    return "| " + " | ".join(cells) + " |"


def render():
    engine = load("engine.json")
    pipeline = load("pipeline.json")
    exchange = load("exchange.json")
    listener = load("itch_listener.json")
    e2e = [(name, load(f"e2e_{name}.json")) for name in E2E]
    host = (engine or pipeline or {}).get("host", "unknown")
    tick = (engine or pipeline or {}).get("timer_tick_ns")

    out = [f"Machine: {host}. Latencies in ns.", ""]
    if tick is not None:
        out += [
            f"steady_clock on this machine advances in {num(tick)} ns ticks, so a single latency sample is a "
            "multiple of that tick; mean costs are total elapsed time divided by message count.",
            "",
        ]
    out += [
        "| Benchmark | Offered load (msg/s) | Achieved (msg/s) | p50 | p90 | p99 | p99.9 | p99.99 | max |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    if engine:
        out.append(row("Engine, one call", "replay", engine["throughput_msgs_per_s"], engine["latency"]))
    if pipeline:
        for r in pipeline["runs"]:
            offered = num(r["target_rate"]) if r["target_rate"] else "unthrottled"
            out.append(row("Pipeline, decode to first response", offered, r["achieved_rate"], r["latency"]))
    for name, r in e2e:
        if r:
            offered = num(r["target_rate"]) if r["target_rate"] else "unthrottled"
            out.append(row("Loopback TCP, send to first response", offered, r["achieved_send_rate"], r["latency"]))
    out.append("")
    if engine:
        out.append(
            f"Engine: {num(engine['mean_ns_per_msg'])} ns mean per message over "
            f"{num(engine['measured_messages'])} messages, {num(engine['allocations_during_measurement'])} heap "
            "allocations in the measured window."
        )
    if exchange and listener:
        verdict = "equals" if exchange["book_checksum"] == listener["book_checksum"] else "differs from"
        out.append(
            f"Market data: {num(listener['messages'])} ITCH messages, {num(listener['gaps'])} gaps; the book "
            f"rebuilt from ITCH has checksum `{listener['book_checksum']}`, which {verdict} the exchange's."
        )
    return "\n".join(out)


def render_correctness():
    diff = load("differential.json")
    listener = load("itch_listener.json")
    out = []
    if diff:
        out.append(
            f"- Differential test: {num(diff['seeds'])} seeds of {num(diff['messages_per_seed'])} flow messages "
            f"from {num(diff['clients'])} clients, a small fraction mutated into rejects, duplicate tokens, partial "
            "cancels and IOC replaces, go through both the engine and `tests/reference_engine.hpp` (std::map "
            "levels, std::list FIFOs). Every event must match byte for byte, and the final book checksums must "
            f"agree; {num(diff['events_compared'])} events were compared, {num(diff['mismatches'])} mismatches."
        )
    if listener:
        out.append(
            f"- Market data: `itch_listener` rebuilt the books from {num(listener['messages'])} ITCH messages with "
            f"{num(listener['gaps'])} sequence gaps across the whole loopback run and computed the same FNV-1a "
            "checksum the exchange prints at shutdown. `run_all.sh` fails if they differ while gaps is 0."
        )
    return "\n".join(out)


def numbers(text):
    text = re.sub(r"`[0-9a-f]{16}`", "", text)
    text = re.sub(r"p99\.99|p99\.9|p99|p90|p50", "", text)
    return re.findall(r"(?<![A-Za-z_])\d+(?:\.\d+)?", text)


def check(text):
    corpus = "\n".join(p.read_text() for p in sorted(RESULTS.glob("*.json")))
    missing = sorted({n for n in numbers(text) if n not in corpus}, key=float)
    if missing:
        sys.exit(f"README numbers not found in bench/results: {missing}")


def splice(readme, name, block):
    begin, end = f"<!-- {name} -->", f"<!-- /{name} -->"
    if begin not in readme or end not in readme:
        sys.exit(f"README.md has no {begin} ... {end} block")
    head, rest = readme.split(begin, 1)
    _, tail = rest.split(end, 1)
    return f"{head}{begin}\n{block}\n{end}{tail}"


def main():
    readme = README.read_text()
    block = render()
    readme = splice(readme, "results", block)
    readme = splice(readme, "correctness", render_correctness())
    check(readme)
    README.write_text(readme)
    print(block)


if __name__ == "__main__":
    main()
