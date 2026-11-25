#!/usr/bin/env python3
"""Run AWSY (./mach awsy-test, tp6) on every allocator build and summarize it.

usage: run.py [--rounds N] [--builds a,b] [--out DIR] [--quick]
              [--checkpoint NAME] [--rescore] ...

Each round runs every build once, with the build order rotated from round to
round. Each run keeps its memory reports in <out>/reports/<tag>/ and appends
one JSON line to <out>/raw.jsonl; re-running with the same --out skips the
runs already recorded. <out>/awsy.csv has one row per build and one column per
metric, each the median over the runs, read from the checkpoint's report.

awsy-test itself exits with an error on system-malloc, because its perfherder
summary needs heap-allocated, which only MOZ_MEMORY builds report. That summary
is not used here: a run counts as good when the checkpoint's report was written
and no process crashed.
"""

import argparse
import collections
import csv
import datetime
import glob
import gzip
import json
import os
import re
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FIREFOX = os.path.normpath(os.path.join(HERE, "..", ".."))

# name -> (objdir, mozconfig)
BUILDS = {
    "baseline": ("obj-host-opt", "mozconfig-baseline"),
    "system-malloc": ("obj-system-malloc", "mozconfig-system-malloc"),
    "jemalloc5": ("obj-jemalloc5", "mozconfig-jemalloc5"),
    "libpas": ("obj-libpas", "mozconfig-libpas"),
    "mimalloc": ("obj-mimalloc", "mozconfig-mimalloc"),
    "partition_alloc": ("obj-partition_alloc", "mozconfig-partition_alloc"),
    "rpmalloc": ("obj-rpmalloc", "mozconfig-rpmalloc"),
    "snmalloc": ("obj-snmalloc", "mozconfig-snmalloc"),
    "tcmalloc": ("obj-tcmalloc", "mozconfig-tcmalloc"),
}

METRICS = ["resident_mb", "rss_mb", "uss_mb", "footprint_mb", "processes"]


def log(msg):
    print(f"[{datetime.datetime.now():%H:%M:%S}] {msg}", flush=True)


def exe_path(a, build):
    return os.path.join(
        a.firefox,
        BUILDS[build][0],
        "dist",
        "Nightly.app",
        "Contents",
        "MacOS",
        "firefox",
    )


def checkpoint_report(dirpath, checkpoint):
    """The checkpoint's report from the last AWSY iteration, or None."""
    pat = re.compile(rf"memory-report-{checkpoint}-(\d+)\.json\.gz$")
    found = (
        [(int(m.group(1)), f) for f in os.listdir(dirpath) if (m := pat.match(f))]
        if os.path.isdir(dirpath)
        else []
    )
    return os.path.join(dirpath, max(found)[1]) if found else None


def report_metrics(path):
    """Per-checkpoint memory totals in MiB, from the top-level reporters.

    resident_mb is AWSY's "Resident Memory": the main process's RSS plus the
    USS of every child process. rss_mb, uss_mb and footprint_mb sum resident,
    resident-unique and resident-phys-footprint over all processes."""
    with gzip.open(path, "rt") as f:
        reports = json.load(f)["reports"]
    procs = collections.defaultdict(dict)
    for r in reports:
        if r["path"] in ("resident", "resident-unique", "resident-phys-footprint"):
            procs[r["process"]][r["path"]] = r["amount"]
    mib = 2**20
    main = [v for p, v in procs.items() if p.startswith("Main")]
    children = [v for p, v in procs.items() if not p.startswith("Main")]
    if len(main) != 1:
        raise ValueError(f"{path}: expected one main process, found {len(main)}")
    return {
        "resident_mb": (
            main[0]["resident"] + sum(v.get("resident-unique", 0) for v in children)
        )
        / mib,
        "rss_mb": sum(v.get("resident", 0) for v in procs.values()) / mib,
        "uss_mb": sum(v.get("resident-unique", 0) for v in procs.values()) / mib,
        "footprint_mb": sum(v.get("resident-phys-footprint", 0) for v in procs.values())
        / mib,
        "processes": len(procs),
    }


def run_awsy(a, build, tag):
    results = os.path.join(a.out, "reports", tag)
    os.makedirs(results, exist_ok=True)
    os.makedirs(os.path.dirname(a.web_root), exist_ok=True)
    cmd = [
        "./mach",
        "awsy-test",
        "--binary",
        exe_path(a, build),
        "--results",
        results,
        "--web-root",
        a.web_root,
    ] + a.awsy_arg
    if a.quick:
        cmd.append("--quick")
    env = dict(os.environ, MOZCONFIG=os.path.join(a.firefox, BUILDS[build][1]))
    logpath = os.path.join(a.out, "logs", f"{tag}.log")
    t0 = time.time()
    with open(logpath, "w") as logf:
        p = subprocess.run(
            cmd, cwd=a.firefox, env=env, stdout=logf, stderr=subprocess.STDOUT
        )
    elapsed = time.time() - t0
    with open(logpath, errors="replace") as f:
        crashed = "PROCESS-CRASH" in f.read()
    report = checkpoint_report(results, a.checkpoint)
    rec = {
        "ok": report is not None and not crashed,
        "returncode": p.returncode,
        "elapsed_s": round(elapsed, 1),
        "checkpoint": a.checkpoint,
    }
    if crashed:
        rec["crashed"] = True
    if report:
        rec["report"] = os.path.relpath(report, a.out)
        rec["metrics"] = report_metrics(report)
    return rec


def summarize(recs, builds, path):
    data = collections.defaultdict(lambda: collections.defaultdict(list))
    for r in recs:
        if r["ok"]:
            for m, v in r["metrics"].items():
                data[r["build"]][m].append(v)
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["allocator"] + METRICS)
        for b in builds:
            w.writerow(
                [b]
                + [
                    f"{statistics.median(data[b][c]):.3f}" if data[b][c] else ""
                    for c in METRICS
                ]
            )
    width = max(len(b) for b in builds)
    print(f"{'allocator':{width}s} " + " ".join(f"{c:>17s}" for c in METRICS))
    for b in builds:
        cells = []
        for c in METRICS:
            xs = data[b][c]
            cells.append(
                f"{statistics.median(xs):11.1f} (n={len(xs):2d})" if xs else " " * 17
            )
        print(f"{b:{width}s} " + " ".join(cells))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--builds", help="comma-separated subset of: " + ",".join(BUILDS))
    ap.add_argument("--out", help="default results/awsy-<timestamp>")
    ap.add_argument("--firefox", default=FIREFOX, help="checkout holding the objdirs")
    ap.add_argument(
        "--checkpoint",
        default="TabsOpenForceGC",
        help="memory report to read (default TabsOpenForceGC)",
    )
    ap.add_argument(
        "--quick",
        action="store_true",
        help="pass --quick to awsy-test (3 pages, short waits)",
    )
    ap.add_argument(
        "--web-root",
        help="awsy-test web root, where the tp6 recordings are downloaded "
        "once for all builds (default: obj-host-opt/_tests/awsy/html)",
    )
    ap.add_argument(
        "--awsy-arg",
        action="append",
        default=[],
        help="extra argument for ./mach awsy-test (repeatable)",
    )
    ap.add_argument(
        "--rescore", action="store_true", help="only recompute awsy.csv from raw.jsonl"
    )
    a = ap.parse_args()

    builds = a.builds.split(",") if a.builds else list(BUILDS)
    for b in builds:
        if b not in BUILDS:
            sys.exit(f"unknown build {b}")
    a.out = os.path.abspath(
        a.out or os.path.join(HERE, "results", f"awsy-{time.strftime('%Y%m%d-%H%M')}")
    )
    a.web_root = os.path.abspath(
        a.web_root
        or os.path.join(a.firefox, BUILDS["baseline"][0], "_tests", "awsy", "html")
    )
    os.makedirs(os.path.join(a.out, "logs"), exist_ok=True)
    raw_path = os.path.join(a.out, "raw.jsonl")
    recs = []
    if os.path.exists(raw_path):
        with open(raw_path) as f:
            recs = [json.loads(l) for l in f if l.strip()]

    if not a.rescore:
        for b in builds:
            if not os.access(exe_path(a, b), os.X_OK):
                sys.exit(f"{b}: no build at {exe_path(a, b)}")
        done = {(r["build"], r["round"]) for r in recs}
        for r in range(a.rounds):
            k = r % len(builds)
            for b in builds[k:] + builds[:k]:
                if (b, r) in done:
                    continue
                tag = f"{b}-r{r}"
                rec = {
                    "build": b,
                    "round": r,
                    "time": datetime.datetime.now().isoformat(timespec="seconds"),
                }
                rec.update(run_awsy(a, b, tag))
                with open(raw_path, "a") as f:
                    f.write(json.dumps(rec) + "\n")
                recs.append(rec)
                log(
                    f"{tag} {'ok' if rec['ok'] else 'FAILED, see logs/' + tag + '.log'}"
                    f" ({rec['elapsed_s']} s)"
                )

    failed = collections.Counter(r["build"] for r in recs if not r["ok"])
    for b, n in failed.items():
        print(f"warning: {b}: {n} failed runs (left out)")
    csv_path = os.path.join(a.out, "awsy.csv")
    summarize([r for r in recs if r["build"] in builds], builds, csv_path)
    print(f"\nwrote {csv_path}")


if __name__ == "__main__":
    main()
