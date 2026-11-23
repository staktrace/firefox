#!/usr/bin/env python3
"""Run the allocator memory-error probes on every build and score them.

The probes live in firefox/memory/probes and are built into each build's
firefox; `firefox --alloc-probe NAME TRIAL` runs one. This runs every probe
TRIALS times on each build below, appends one JSON line per trial to
<out>/raw.jsonl, and writes <out>/scores.csv: one row per build, one column
per error category, higher is better.

usage: run.py [-n TRIALS] [--builds a,b] [--probes REGEX] [--out DIR]
              [--jobs N] [--firefox DIR] [--rescore]

Re-running with the same --out skips trials already in raw.jsonl.
"""
import argparse
import collections
import concurrent.futures
import csv
import json
import os
import re
import signal
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FIREFOX = os.path.normpath(os.path.join(HERE, "..", ".."))

BUILDS = {
    "baseline": "obj-host-opt",
    "system-malloc": "obj-system-malloc",
    "jemalloc5": "obj-jemalloc5",
    "libpas": "obj-libpas",
    "mimalloc": "obj-mimalloc",
    "partition_alloc": "obj-partition_alloc",
    "rpmalloc": "obj-rpmalloc",
    "snmalloc": "obj-snmalloc",
    "tcmalloc": "obj-tcmalloc",
}

CATEGORIES = ["oob_write", "oob_read", "uaf", "uninit", "double_free", "invalid_free"]

# What one trial is worth. The allocator stopped the attacker (1), or the
# primitive worked (0). exploitable_then_crash: the primitive worked but the
# process died shortly after, so the attacker has to finish inside that
# window. error trials are left out.
TRIAL_SCORE = {
    "detected": 1.0,
    "detected_late": 1.0,
    "neutralized": 1.0,
    "neutralized_then_crash": 1.0,
    "exploitable_then_crash": 0.5,
    "exploitable": 0.0,
}

# Longer than the probes' own 10 s alarm, so only a harness problem hits it.
TIMEOUT_S = 30


def binary(firefox, objdir):
    return os.path.join(firefox, objdir, "dist", "bin", "firefox")


def list_probes(exe):
    p = subprocess.run([exe, "--alloc-probe", "list"], capture_output=True,
                       text=True, timeout=TIMEOUT_S)
    probes = [l.split() for l in p.stdout.splitlines() if l.strip()]
    if p.returncode != 0 or not probes or any(len(x) != 2 for x in probes):
        return None
    return probes


def run_trial(exe, probe, trial):
    try:
        p = subprocess.run([exe, "--alloc-probe", probe, str(trial)],
                           capture_output=True, text=True, timeout=TIMEOUT_S,
                           env=dict(os.environ, MOZ_CRASHREPORTER_DISABLE="1"))
    except subprocess.TimeoutExpired:
        return {"outcome": "error", "how": "timeout"}
    lines = p.stdout.splitlines()
    began, ended = "BUG_BEGIN" in lines, "BUG_END" in lines
    res = next((l for l in lines if l.startswith("RESULT ")), None)
    crash = next((l for l in lines if l.startswith("CRASH ")), None)
    sig = int(crash.split()[1]) if crash else (-p.returncode if p.returncode < 0 else 0)
    how = signal.Signals(sig).name if sig else (f"exit{p.returncode}" if p.returncode else "")
    rec = {"how": how}
    err = (p.stderr.strip().splitlines() or [""])[0][:160]
    if err:
        rec["stderr"] = err
    if res:
        f = res.split(None, 3)
        rec["frac"] = float(f[2])
        if len(f) > 3:
            rec["detail"] = f[3]
        rec["outcome"] = f[1] + ("_then_crash" if how else "")
    elif not began:
        rec["outcome"] = "error"
    else:
        rec["outcome"] = "detected_late" if ended else "detected"
    return rec


def load_raw(path):
    recs = []
    if os.path.exists(path):
        with open(path) as f:
            recs = [json.loads(l) for l in f if l.strip()]
    return recs


def score(recs, builds):
    """Per build and category, the sum over probes of the mean trial score.

    A probe that one build has and another lacks (the moz_arena probes are
    missing without MOZ_MEMORY) scores 0 where it is missing: that build has
    no such isolation."""
    trials = collections.defaultdict(list)
    cat_of = {}
    for r in recs:
        cat_of[r["probe"]] = r["category"]
        if r["outcome"] in TRIAL_SCORE:
            trials[(r["build"], r["probe"])].append(TRIAL_SCORE[r["outcome"]])
    probes_by_cat = collections.defaultdict(set)
    for probe, cat in cat_of.items():
        probes_by_cat[cat].add(probe)
    table = {}
    for b in builds:
        table[b] = {}
        for cat in CATEGORIES:
            total = 0.0
            for probe in probes_by_cat[cat]:
                xs = trials.get((b, probe))
                if xs:
                    total += sum(xs) / len(xs)
            table[b][cat] = total
    maxima = {cat: len(probes_by_cat[cat]) for cat in CATEGORIES}
    return table, maxima


def write_scores(path, table, maxima):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["allocator"] + CATEGORIES)
        for b, row in table.items():
            w.writerow([b] + [f"{row[c]:.2f}" for c in CATEGORIES])
    width = max(len(b) for b in table) if table else 10
    print(f"{'allocator':{width}s} " + " ".join(f"{c:>12s}" for c in CATEGORIES))
    print(f"{'(max)':{width}s} " + " ".join(f"{maxima[c]:12d}" for c in CATEGORIES))
    for b, row in table.items():
        print(f"{b:{width}s} " + " ".join(f"{row[c]:12.2f}" for c in CATEGORIES))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-n", "--trials", type=int, default=50)
    ap.add_argument("--builds", help="comma-separated subset of: " + ",".join(BUILDS))
    ap.add_argument("--probes", default=".", help="regex selecting probe names")
    ap.add_argument("--out", default=os.path.join(HERE, "results", time.strftime("%Y%m%d-%H%M")))
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--firefox", default=FIREFOX, help="checkout holding the objdirs")
    ap.add_argument("--rescore", action="store_true",
                    help="only recompute scores.csv from raw.jsonl")
    a = ap.parse_args()

    builds = a.builds.split(",") if a.builds else list(BUILDS)
    for b in builds:
        if b not in BUILDS:
            sys.exit(f"unknown build {b}")
    os.makedirs(a.out, exist_ok=True)
    raw_path = os.path.join(a.out, "raw.jsonl")
    recs = load_raw(raw_path)

    if not a.rescore:
        done = {(r["build"], r["probe"], r["trial"]) for r in recs}
        jobs = []
        for b in builds:
            exe = binary(a.firefox, BUILDS[b])
            probes = list_probes(exe) if os.path.exists(exe) else None
            if probes is None:
                sys.exit(f"{b}: {exe} does not run --alloc-probe; "
                         f"rebuild it with MOZCONFIG=mozconfig-* ./mach build")
            for name, cat in probes:
                if not re.search(a.probes, name):
                    continue
                for t in range(a.trials):
                    if (b, name, t) not in done:
                        jobs.append((b, exe, name, cat, t))
        print(f"{len(jobs)} trials to run ({len(done)} already in {raw_path})", flush=True)

        lock = threading.Lock()
        progress = collections.Counter()
        totals = collections.Counter(j[0] for j in jobs)
        t0 = time.time()
        with open(raw_path, "a") as out, \
                concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
            def work(job):
                b, exe, name, cat, t = job
                rec = {"build": b, "probe": name, "category": cat, "trial": t}
                rec.update(run_trial(exe, name, t))
                with lock:
                    out.write(json.dumps(rec) + "\n")
                    recs.append(rec)
                    progress[b] += 1
                    if progress[b] == totals[b]:
                        out.flush()
                        print(f"  {b}: {totals[b]} trials done ({time.time() - t0:.0f} s)",
                              flush=True)
            list(pool.map(work, jobs))

        errors = collections.Counter(r["build"] for r in recs if r["outcome"] == "error")
        for b, n in errors.items():
            print(f"warning: {b}: {n} error trials (left out of the scores)")

    table, maxima = score([r for r in recs if r["build"] in builds and re.search(a.probes, r["probe"])],
                          builds)
    scores_path = os.path.join(a.out, "scores.csv")
    write_scores(scores_path, table, maxima)
    print(f"\nwrote {scores_path}")


if __name__ == "__main__":
    main()
