#!/usr/bin/env python3
"""Renders bench/results/*.json into the README results block.

Every number in the block is copied verbatim from the JSON text, and the script refuses to
write a block containing a number that does not appear in some results file.
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RESULTS = ROOT / "bench" / "results"
README = ROOT / "README.md"
BEGIN, END = "<!-- results -->", "<!-- /results -->"
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


def check(block):
    corpus = "\n".join(p.read_text() for p in sorted(RESULTS.glob("*.json")))
    text = re.sub(r"`[^`]*`", "", block)
    text = text.replace("p99.99", "").replace("p99.9", "").replace("p99", "").replace("p90", "").replace("p50", "")
    missing = [n for n in re.findall(r"\d+(?:\.\d+)?", text) if n not in corpus]
    if missing:
        sys.exit(f"numbers not found in bench/results: {sorted(set(missing))}")


def main():
    block = render()
    check(block)
    readme = README.read_text() if README.exists() else ""
    if BEGIN in readme and END in readme:
        head, rest = readme.split(BEGIN, 1)
        _, tail = rest.split(END, 1)
        readme = f"{head}{BEGIN}\n{block}\n{END}{tail}"
    else:
        readme = readme.rstrip("\n") + f"\n\n## Results\n\n{BEGIN}\n{block}\n{END}\n"
    README.write_text(readme)
    print(block)


if __name__ == "__main__":
    main()
