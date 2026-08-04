#!/usr/bin/env python3
"""Rebuild a benchmark results CSV from the per-case dvledtx logs.

Usage: parse_bench_logs.py <run-dir> [output.csv]

<run-dir> is a benchmark_results/<timestamp> directory containing logs/.
Log files must be named <W>x<H>_<FPS>fps_<FMT>.log, which is what
benchmark.sh produces. Re-parsing is non-destructive: it only reads logs, so
it can be re-run at any time to add new derived columns.
"""
import csv
import os
import re
import sys

WARMUP_SAMPLES = 2

NAME_RE = re.compile(r"^(\d+)x(\d+)_(\d+)fps_([a-z0-9]+)\.log$")
SESSION_RE = re.compile(r"TX_VIDEO_SESSION\(0,0[:)]")
FPS_RE = re.compile(r"fps ([0-9]+\.?[0-9]*)")
BW_RE = re.compile(r"throughput ([0-9]+\.?[0-9]*)")
DROP_RE = re.compile(r"epoch drop ([0-9]+)")
STARVE_RE = re.compile(r"no ready frame from user ([0-9]+)")

FIELDS = [
    "width", "height", "fps_target", "fmt", "samples",
    "fps_avg", "fps_min", "fps_max",
    "mbps_avg", "mbps_min", "mbps_max",
    "epoch_drop", "starved_frames", "status",
]


def parse_log(path, warmup=WARMUP_SAMPLES):
    fps, bw, drop, starved = [], [], 0, 0
    with open(path, errors="replace") as fh:
        for line in fh:
            if not SESSION_RE.search(line):
                continue
            m = FPS_RE.search(line)
            if m:
                fps.append(float(m.group(1)))
            m = BW_RE.search(line)
            if m:
                bw.append(float(m.group(1)))
            m = DROP_RE.search(line)
            if m:
                drop += int(m.group(1))
            m = STARVE_RE.search(line)
            if m:
                starved += int(m.group(1))

    fps = [v for v in fps[warmup:] if v > 0]
    bw = [v for v in bw[warmup:] if v > 0]
    if not fps:
        return None
    return {
        "samples": len(fps),
        "fps_avg": round(sum(fps) / len(fps), 3),
        "fps_min": round(min(fps), 3),
        "fps_max": round(max(fps), 3),
        "mbps_avg": round(sum(bw) / len(bw), 2) if bw else 0.0,
        "mbps_min": round(min(bw), 2) if bw else 0.0,
        "mbps_max": round(max(bw), 2) if bw else 0.0,
        "epoch_drop": drop,
        "starved_frames": starved,
    }


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    run_dir = sys.argv[1]
    out_csv = sys.argv[2] if len(sys.argv) > 2 else os.path.join(run_dir, "results.csv")
    log_dir = os.path.join(run_dir, "logs")
    if not os.path.isdir(log_dir):
        print(f"ERROR: no logs/ directory in {run_dir}", file=sys.stderr)
        return 1

    rows = []
    for name in sorted(os.listdir(log_dir)):
        m = NAME_RE.match(name)
        if not m:
            continue
        w, h, fps_t, fmt = int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4)
        stats = parse_log(os.path.join(log_dir, name))
        row = {"width": w, "height": h, "fps_target": fps_t, "fmt": fmt}
        if stats is None:
            row.update({k: 0 for k in FIELDS[4:-1]})
            row["status"] = "no_stats"
        else:
            row.update(stats)
            row["status"] = "ok"
        rows.append(row)

    rows.sort(key=lambda r: (r["width"], r["height"], r["fps_target"], r["fmt"]))
    with open(out_csv, "w", newline="") as fh:
        wr = csv.DictWriter(fh, fieldnames=FIELDS)
        wr.writeheader()
        wr.writerows(rows)

    ok = sum(1 for r in rows if r["status"] == "ok")
    print(f"wrote {out_csv}: {len(rows)} cases ({ok} with statistics)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
