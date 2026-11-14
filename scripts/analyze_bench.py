#!/usr/bin/env python3
"""Analyze cpu_bench output (.out) and legacy bench_one.sh logs."""
import glob
import re
import sys


def parse(path):
    before = {}
    after = {}
    section = None
    temps = []
    volts = []
    direct_freq = {}
    clusters = {}
    result = {}
    threads = []

    with open(path, "r", errors="replace") as f:
        for line in f:
            line = line.strip()

            if line.startswith("### snapshot"):
                section = line.split()[-1]
                continue

            m = re.match(r"tis (cpu\d+) (\d+) (\d+)$", line)
            if m:
                cpu, freq, ticks = m.group(1), int(m.group(2)), int(m.group(3))
                (before if section == "before" else after).setdefault(cpu, {})[freq] = ticks
                continue

            m = re.match(r"temp_millic before=(-?\d+) after=(-?\d+)", line)
            if m:
                temps.extend([int(m.group(1)), int(m.group(2))])
                continue

            m = re.match(r"vddcpu_uv before=(-?\d+) after=(-?\d+)", line)
            if m:
                volts.extend([int(m.group(1)), int(m.group(2))])
                continue

            m = re.match(r"sample .*t=(\d+) v=(\d+)", line)
            if m:
                temps.append(int(m.group(1)))
                volts.append(int(m.group(2)))
                continue

            m = re.match(r"freq (cpu\d+) avg_mhz=([\d.]+)", line)
            if m:
                direct_freq[m.group(1)] = float(m.group(2))
                continue

            m = re.match(r"freq_cluster (\w+)_avg_mhz=([\d.]+)", line)
            if m:
                clusters[m.group(1)] = float(m.group(2))
                continue

            m = re.match(r"RESULT mode=(\w+) threads=(\d+) wall_s=([\d.]+) "
                         r"total_iters=(\d+) throughput_mips=([\d.]+) checksum=(\w+)", line)
            if m:
                result = {
                    "mode": m.group(1), "threads": int(m.group(2)),
                    "wall_s": float(m.group(3)), "iters": int(m.group(4)),
                    "mips": float(m.group(5)), "checksum": m.group(6),
                }
                continue

            m = re.match(r"thread(\d+) cpu=(\d+) iters=(\d+) wall_s=([\d.]+) "
                         r"cpu_s=([\d.]+) mips=([\d.]+) checksum=(\w+)", line)
            if m:
                threads.append({
                    "tid": int(m.group(1)), "cpu": int(m.group(2)),
                    "iters": int(m.group(3)),
                    "wall_s": float(m.group(4)), "cpu_s": float(m.group(5)),
                    "mips": float(m.group(6)),
                })

    freq = {}
    for cpu in sorted(before, key=lambda c: int(c[3:])):
        b, a = before[cpu], after.get(cpu, {})
        delta = {f: a.get(f, 0) - b.get(f, 0) for f in b}
        delta = {f: d for f, d in delta.items() if d > 0}
        total = sum(delta.values())
        if total:
            avg = sum(f * d for f, d in delta.items()) / total
            top = sorted(delta.items(), key=lambda kv: -kv[1])[:3]
            freq[cpu] = (avg, total, top)

    return {
        "result": result, "threads": threads, "freq": freq,
        "temps": temps, "volts": volts,
        "direct_freq": direct_freq, "clusters": clusters,
    }


def main(paths):
    runs = []
    for p in sorted(paths):
        d = parse(p)
        if not d["result"]:
            print(f"{p}: no RESULT line")
            continue
        runs.append((p, d))

    for p, d in runs:
        r = d["result"]
        print("=" * 78)
        print(p)
        print(f"mode={r['mode']} threads={r['threads']} wall={r['wall_s']:.3f}s "
              f"throughput={r['mips']:.2f} Miter/s checksum={r['checksum']}")
        if d["temps"]:
            print(f"temp: start={d['temps'][0]/1000:.2f}C end={d['temps'][-1]/1000:.2f}C "
                  f"min={min(d['temps'])/1000:.2f}C max={max(d['temps'])/1000:.2f}C")
        if d["volts"]:
            print(f"vddcpu: start={d['volts'][0]/1000:.1f}mV end={d['volts'][-1]/1000:.1f}mV")
        if d["clusters"]:
            print("cluster averages: " + ", ".join(
                f"{k}={v:.1f} MHz" for k, v in sorted(d["clusters"].items())))
        for cpu, mhz in sorted(d["direct_freq"].items(), key=lambda kv: int(kv[0][3:])):
            print(f"  {cpu}: avg={mhz:.1f} MHz")
        for t in sorted(d["threads"], key=lambda x: x["tid"]):
            print(f"  thread{t['tid']} cpu{t['cpu']}: {t['mips']:6.2f} Miter/s "
                  f"({t['iters']/1e6:.0f}M in {t['wall_s']:.3f}s)")

    singles = [d["result"] for _, d in runs if d["result"]["mode"] == "single"]
    alls = [d["result"] for _, d in runs if d["result"]["mode"] == "all"]
    if singles and alls:
        s = max(x["mips"] for x in singles)
        a = max(x["mips"] for x in alls)
        print("=" * 78)
        print(f"best single: {s:.2f} Miter/s")
        print(f"best all:    {a:.2f} Miter/s")
        print(f"speedup:     {a/s:.2f}x")
        print(f"per-thread efficiency: {a/s/8*100:.1f}% of single-core rate (8 threads)")
        print(f"fraction of 4x2.0GHz + 4x1.6GHz ideal: {a/s/7.2*100:.1f}%")


if __name__ == "__main__":
    args = sys.argv[1:] or glob.glob("bench_logs/bench_*.out")
    main(args)
