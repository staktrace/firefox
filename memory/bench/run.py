#!/usr/bin/env python3
"""Run a performance benchmark on every allocator build and summarize it.

usage: run.py BENCHMARK [--rounds N] [--builds a,b] [--out DIR]
              [--passes timing,resources] [--rescore] ...

BENCHMARK is alloc-micro (the page in alloc-micro/) or sp3 (Speedometer 3
through ./mach raptor). Each build runs in rounds: every round runs each build
once, with the build order rotated from round to round. A timing pass runs
without monitoring, and a resource pass runs again with procmon.py sampling the
CPU time and memory of every process of the build. One JSON line per run is
appended to <out>/raw.jsonl; re-running with the same --out skips the runs
already recorded. <out>/BENCHMARK.csv has one row per build and one column per
metric, each the median over the runs.
"""
import argparse
import collections
import csv
import datetime
import json
import os
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
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

# benchmark -> CSV columns, in order
METRICS = {
    "alloc-micro": ["wall_s", "work_s", "cpu_s", "peak_rss_mb", "peak_footprint_mb"],
    "sp3": ["score", "wall_s", "cpu_s", "peak_rss_mb", "peak_footprint_mb"],
}

DEFAULT_ROUNDS = {"alloc-micro": 10, "sp3": 5}

# Monitored sessions with fewer processes than this are not a browser running
# the benchmark (raptor starts lone firefox processes, for example).
MIN_SESSION_PROCESSES = 3

MICRO_PREFS = """\
user_pref("browser.dom.window.dump.enabled", true);
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("browser.aboutwelcome.enabled", false);
user_pref("browser.startup.page", 0);
user_pref("browser.startup.homepage_override.mstone", "ignore");
user_pref("browser.sessionstore.resume_from_crash", false);
user_pref("browser.tabs.warnOnClose", false);
user_pref("browser.warnOnQuit", false);
user_pref("datareporting.policy.dataSubmissionEnabled", false);
user_pref("toolkit.telemetry.enabled", false);
user_pref("app.update.enabled", false);
user_pref("app.normandy.enabled", false);
user_pref("browser.newtabpage.enabled", false);
user_pref("layout.frame_rate", 0);
user_pref("docshell.event_starvation_delay_hint", 1);
user_pref("marionette.port", %d);
"""
MARIONETTE_PORT = 2839
MICRO_TIMEOUT_S = 300


def log(msg):
    print(f"[{datetime.datetime.now():%H:%M:%S}] {msg}", flush=True)


def app_path(a, build):
    return os.path.join(a.firefox, BUILDS[build][0], "dist", "Nightly.app")


def exe_path(a, build):
    return os.path.join(app_path(a, build), "Contents", "MacOS", "firefox")


class Monitor:
    def __init__(self, a, build, enabled):
        self.proc = None
        if enabled:
            fd, self.path = tempfile.mkstemp(suffix=".json")
            os.close(fd)
            self.proc = subprocess.Popen(
                [sys.executable, os.path.join(HERE, "procmon.py"), "--app",
                 app_path(a, build), "--out", self.path, "--interval", str(a.interval)])
            time.sleep(0.5)

    def stop(self):
        """The monitored sessions, as procmon.py summarized them."""
        if not self.proc:
            return None
        self.proc.send_signal(signal.SIGTERM)
        self.proc.wait(timeout=60)
        with open(self.path) as f:
            data = json.load(f)
        os.remove(self.path)
        return data["sessions"]


def marionette_quit(port):
    """Asks the browser to quit through Marionette, so shutdown runs."""
    try:
        sock = socket.create_connection(("127.0.0.1", port), timeout=30)
    except OSError:
        return
    buf = b""

    def recv():
        nonlocal buf
        while b":" not in buf:
            chunk = sock.recv(65536)
            if not chunk:
                raise ConnectionError
            buf += chunk
        length, _, rest = buf.partition(b":")
        while len(rest) < int(length):
            chunk = sock.recv(65536)
            if not chunk:
                raise ConnectionError
            rest += chunk
        buf = rest[int(length):]

    def send(msg_id, name, params):
        data = json.dumps([0, msg_id, name, params]).encode()
        sock.sendall(str(len(data)).encode() + b":" + data)
        recv()

    try:
        recv()
        send(1, "WebDriver:NewSession", {})
        send(2, "Marionette:Quit", {"flags": ["eForceQuit"]})
    except (OSError, ValueError, ConnectionError):
        pass
    finally:
        sock.close()


def run_micro(a, build, tag, monitor):
    page = os.path.join(HERE, "alloc-micro", "index.html")
    url = f"file://{page}?tag={tag}" + (f"&steps={a.steps}" if a.steps else "")
    logpath = os.path.join(a.out, "logs", f"{tag}.log")
    with tempfile.TemporaryDirectory(prefix="alloc-micro-") as prof:
        with open(os.path.join(prof, "user.js"), "w") as f:
            f.write(MICRO_PREFS % MARIONETTE_PORT)
        cmd = [exe_path(a, build), "-no-remote", "-marionette", "-profile", prof,
               "-headless" if a.headless else "-foreground", url]
        mon = Monitor(a, build, monitor)
        t0 = time.time()
        with open(logpath, "w") as logf:
            p = subprocess.Popen(cmd, stdout=logf, stderr=subprocess.STDOUT,
                                 env=dict(os.environ, MOZ_CRASHREPORTER_DISABLE="1"))
            result = None
            while result is None and p.poll() is None and time.time() - t0 < MICRO_TIMEOUT_S:
                time.sleep(1)
                with open(logpath, errors="replace") as f:
                    for line in f:
                        if line.startswith("ALLOC_MICRO_RESULT "):
                            result = json.loads(line.split(" ", 1)[1])
                            break
            marionette_quit(MARIONETTE_PORT)
            try:
                p.wait(timeout=60)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        elapsed = time.time() - t0
        sessions = mon.stop()
    rec = {"ok": result is not None, "elapsed_s": round(elapsed, 1), "sessions": sessions}
    if result:
        rec["metrics"] = {"wall_s": result["wallMs"] / 1000, "work_s": result["workMs"] / 1000}
        rec["opMs"] = result["opMs"]
    return rec


def run_sp3(a, build, tag, monitor):
    build_dir = os.path.join(a.firefox, "testing", "mozharness", "build")
    raptor_json = os.path.join(build_dir, "raptor.json")
    upload = os.path.join(build_dir, "blobber_upload_dir")

    def dumps():
        return set(f for f in os.listdir(upload) if f.endswith(".dmp")) \
            if os.path.isdir(upload) else set()

    if os.path.exists(raptor_json):
        os.remove(raptor_json)
    dumps_before = dumps()
    cmd = ["./mach", "raptor", "-t", "speedometer3", "--binary", exe_path(a, build),
           "--browser-cycles", str(a.cycles)] + a.raptor_arg
    env = dict(os.environ, MOZCONFIG=os.path.join(a.firefox, BUILDS[build][1]))
    logpath = os.path.join(a.out, "logs", f"{tag}.log")
    mon = Monitor(a, build, monitor)
    t0 = time.time()
    with open(logpath, "w") as logf:
        p = subprocess.run(cmd, cwd=a.firefox, env=env, stdout=logf, stderr=subprocess.STDOUT)
    elapsed = time.time() - t0
    sessions = mon.stop()

    suite = None
    if os.path.exists(raptor_json):
        with open(raptor_json) as f:
            suite = next((s for s in json.load(f)["suites"] if s["name"] == "speedometer3"),
                         None)
    new_dumps = sorted(dumps() - dumps_before)
    rec = {"ok": p.returncode == 0 and suite is not None and not new_dumps,
           "returncode": p.returncode, "elapsed_s": round(elapsed, 1),
           "sessions": sessions}
    if new_dumps:
        rec["crash_dumps"] = new_dumps
    if suite:
        subs = {t["name"]: t.get("replicates") or [t.get("value")] for t in suite["subtests"]}
        rec["metrics"] = {"score": subs.get("score", []),
                          "wall_s": [v / 1000 for v in subs.get("wallclock-for-tracking-only", [])]}
        rec["subtests"] = subs
    return rec


RUNNERS = {"alloc-micro": run_micro, "sp3": run_sp3}


def samples(rec):
    """Yields (metric, value) for one run record. Timing metrics come from the
    timing pass only, resource metrics from the resource pass only."""
    if not rec["ok"]:
        return
    if rec["pass"] == "timing":
        for m, v in rec.get("metrics", {}).items():
            for x in v if isinstance(v, list) else [v]:
                yield m, x
    elif rec.get("sessions"):
        for s in rec["sessions"]:
            if s["processes"] >= MIN_SESSION_PROCESSES:
                yield "cpu_s", s["cpu_s"]
                yield "peak_rss_mb", s["peak_rss_bytes"] / 2**20
                yield "peak_footprint_mb", s["peak_footprint_bytes"] / 2**20


def summarize(recs, bench, builds, path):
    data = collections.defaultdict(lambda: collections.defaultdict(list))
    for r in recs:
        for m, v in samples(r):
            data[r["build"]][m].append(v)
    cols = METRICS[bench]
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["allocator"] + cols)
        for b in builds:
            w.writerow([b] + [f"{statistics.median(data[b][c]):.3f}" if data[b][c] else ""
                              for c in cols])
    width = max(len(b) for b in builds)
    print(f"{'allocator':{width}s} " + " ".join(f"{c:>17s}" for c in cols))
    for b in builds:
        cells = []
        for c in cols:
            xs = data[b][c]
            cells.append(f"{statistics.median(xs):11.2f} (n={len(xs):2d})" if xs else " " * 17)
        print(f"{b:{width}s} " + " ".join(cells))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("benchmark", choices=list(RUNNERS))
    ap.add_argument("--rounds", type=int, help="rounds per pass (alloc-micro 10, sp3 5)")
    ap.add_argument("--builds", help="comma-separated subset of: " + ",".join(BUILDS))
    ap.add_argument("--passes", default="timing,resources")
    ap.add_argument("--out", help="default results/BENCHMARK-<timestamp>")
    ap.add_argument("--firefox", default=FIREFOX, help="checkout holding the objdirs")
    ap.add_argument("--warmup", type=int, default=1,
                    help="unrecorded alloc-micro runs per build before the first round")
    ap.add_argument("--interval", type=float, default=0.1, help="procmon sampling interval")
    ap.add_argument("--steps", type=int, help="alloc-micro steps (default: the page's 900)")
    ap.add_argument("--headless", action="store_true", help="run alloc-micro headless")
    ap.add_argument("--cycles", type=int, default=2, help="sp3 browser cycles per run")
    ap.add_argument("--raptor-arg", action="append", default=[],
                    help="extra argument for ./mach raptor (repeatable)")
    ap.add_argument("--rescore", action="store_true",
                    help="only recompute BENCHMARK.csv from raw.jsonl")
    a = ap.parse_args()

    bench = a.benchmark
    builds = a.builds.split(",") if a.builds else list(BUILDS)
    for b in builds:
        if b not in BUILDS:
            sys.exit(f"unknown build {b}")
    a.out = a.out or os.path.join(HERE, "results", f"{bench}-{time.strftime('%Y%m%d-%H%M')}")
    rounds = a.rounds or DEFAULT_ROUNDS[bench]
    os.makedirs(os.path.join(a.out, "logs"), exist_ok=True)
    raw_path = os.path.join(a.out, "raw.jsonl")
    recs = []
    if os.path.exists(raw_path):
        with open(raw_path) as f:
            recs = [json.loads(l) for l in f if l.strip()]
    recs = [r for r in recs if r["bench"] == bench]

    if not a.rescore:
        for b in builds:
            if not os.access(exe_path(a, b), os.X_OK):
                sys.exit(f"{b}: no build at {exe_path(a, b)}")
        done = {(r["pass"], r["build"], r["round"]) for r in recs}
        run = RUNNERS[bench]
        if bench == "alloc-micro" and not done:
            for i in range(a.warmup):
                for b in builds:
                    log(f"warmup {b}")
                    run(a, b, f"warmup-{b}-{i}", False)
        for pass_ in a.passes.split(","):
            for r in range(rounds):
                k = r % len(builds)
                for b in builds[k:] + builds[:k]:
                    if (pass_, b, r) in done:
                        continue
                    tag = f"{pass_}-{b}-r{r}"
                    rec = {"bench": bench, "pass": pass_, "build": b, "round": r,
                           "time": datetime.datetime.now().isoformat(timespec="seconds")}
                    rec.update(run(a, b, tag, pass_ == "resources"))
                    with open(raw_path, "a") as f:
                        f.write(json.dumps(rec) + "\n")
                    recs.append(rec)
                    log(f"{tag} {'ok' if rec['ok'] else 'FAILED, see logs/' + tag + '.log'}"
                        f" ({rec['elapsed_s']} s)")

    failed = collections.Counter(r["build"] for r in recs if not r["ok"])
    for b, n in failed.items():
        print(f"warning: {b}: {n} failed runs (left out)")
    csv_path = os.path.join(a.out, f"{bench}.csv")
    summarize([r for r in recs if r["build"] in builds], bench, builds, csv_path)
    print(f"\nwrote {csv_path}")


if __name__ == "__main__":
    main()
