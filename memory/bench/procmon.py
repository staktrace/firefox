# Samples every process whose executable lives inside one Firefox .app bundle
# and records total CPU time and summed resident memory, using libproc
# (proc_pid_rusage), so it costs a few syscalls per process per sample.
#
# Usage: python3 procmon.py --app /path/to/Nightly.app --out result.json
#                           [--interval 0.1]
# Runs until SIGINT/SIGTERM, then writes the JSON summary.
#
# Processes are grouped into browser sessions by their root process (the
# bundle's main firefox executable); a run of a cold-start benchmark that
# restarts the browser several times yields several sessions.
#
# Caveats: a process's CPU time after its last sample is lost (at most one
# interval's worth), and a process that lives less than one interval may be
# missed. RSS counts shared pages in every process that maps them; the
# phys_footprint total (what Activity Monitor calls Memory) does not.
import argparse
import ctypes
import json
import os
import signal
import struct
import time

libc = ctypes.CDLL("/usr/lib/libSystem.B.dylib", use_errno=True)
libc.proc_listallpids.argtypes = [ctypes.c_void_p, ctypes.c_int]
libc.proc_pidpath.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_uint32]
libc.proc_pid_rusage.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
libc.proc_pidinfo.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint64,
                              ctypes.c_void_p, ctypes.c_int]

RUSAGE_INFO_V2 = 2
PROC_PIDTBSDINFO = 3
PROC_BSDINFO_SIZE = 136


class Timebase(ctypes.Structure):
    _fields_ = [("numer", ctypes.c_uint32), ("denom", ctypes.c_uint32)]


tb = Timebase()
libc.mach_timebase_info(ctypes.byref(tb))
# rusage_info CPU times are in mach absolute time units.
TICK_NS = tb.numer / tb.denom


def all_pids():
    n = libc.proc_listallpids(None, 0)
    buf = (ctypes.c_int * (n + 64))()
    n = libc.proc_listallpids(buf, ctypes.sizeof(buf))
    return buf[:n]


def pid_path(pid):
    buf = ctypes.create_string_buffer(4096)
    n = libc.proc_pidpath(pid, buf, 4096)
    return buf.value.decode(errors="replace") if n > 0 else None


def pid_ppid(pid):
    buf = ctypes.create_string_buffer(PROC_BSDINFO_SIZE)
    n = libc.proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, buf, PROC_BSDINFO_SIZE)
    if n < PROC_BSDINFO_SIZE:
        return None
    return struct.unpack_from("I", buf.raw, 16)[0]


def pid_rusage(pid):
    buf = ctypes.create_string_buffer(512)
    if libc.proc_pid_rusage(pid, RUSAGE_INFO_V2, buf) != 0:
        return None
    user, system = struct.unpack_from("QQ", buf.raw, 16)
    resident, footprint, start = struct.unpack_from("QQQ", buf.raw, 64)
    return (user + system) * TICK_NS / 1e9, resident, footprint, start


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--interval", type=float, default=0.1)
    args = ap.parse_args()

    app = os.path.realpath(args.app).rstrip("/") + "/"
    main_exe = app + "Contents/MacOS/firefox"
    stop = False

    def on_signal(*_):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)

    procs = {}  # (pid, start) -> info
    # pid -> (start time, when checked) of a process whose path did not
    # match. exec keeps the pid and start time, so a shell that is about to
    # exec firefox must be looked at again: entries expire after a second.
    not_ours = {}
    samples = []
    t0 = time.time()
    while not stop:
        now = time.time() - t0
        live = {}
        for pid in all_pids():
            ru = pid_rusage(pid)
            if not ru:
                continue
            cpu, rss, fp, start = ru
            key = (pid, start)
            info = procs.get(key)
            if info is None:
                seen = not_ours.get(pid)
                if seen and seen[0] == start and now - seen[1] < 1.0:
                    continue
                path = pid_path(pid)
                if not path or not path.startswith(app):
                    not_ours[pid] = (start, now)
                    continue
                info = procs[key] = {
                    "pid": pid, "ppid": pid_ppid(pid), "path": path[len(app):],
                    "first": now, "cpu": 0.0, "peak_rss": 0, "peak_fp": 0,
                }
            info["cpu"] = cpu
            info["last"] = now
            info["peak_rss"] = max(info["peak_rss"], rss)
            info["peak_fp"] = max(info["peak_fp"], fp)
            live[pid] = (rss, fp)
        total_cpu = sum(p["cpu"] for p in procs.values())
        samples.append([round(now, 3), sum(r for r, _ in live.values()),
                        sum(f for _, f in live.values()), round(total_cpu, 3),
                        live])
        elapsed = time.time() - t0 - now
        time.sleep(max(0.0, args.interval - elapsed))

    # Group processes into sessions by their main-executable ancestor.
    by_pid = {p["pid"]: p for p in procs.values()}
    roots = {}
    for p in procs.values():
        r = p
        seen = set()
        while r["ppid"] in by_pid and r["ppid"] not in seen and \
                app + r["path"] != main_exe:
            seen.add(r["pid"])
            r = by_pid[r["ppid"]]
        roots.setdefault(r["pid"], []).append(p)

    # Helpers whose parent is not a firefox process of this build (launched
    # through launchd, say) join the main-executable session they overlap
    # most.
    main_roots = [pid for pid in roots if app + by_pid[pid]["path"] == main_exe]
    for pid in [pid for pid in roots if pid not in main_roots]:
        def overlap(m):
            ms = roots[m]
            lo = max(min(p["first"] for p in roots[pid]), min(p["first"] for p in ms))
            hi = min(max(p["last"] for p in roots[pid]), max(p["last"] for p in ms))
            return hi - lo
        best = max(main_roots, key=overlap, default=None)
        if best is not None and overlap(best) > 0:
            roots[best].extend(roots.pop(pid))

    sessions = []
    for root, members in sorted(roots.items(), key=lambda kv: min(p["first"] for p in kv[1])):
        pids = {p["pid"] for p in members}
        peak_rss = peak_fp = 0
        for s in samples:
            vals = [v for pid, v in s[4].items() if pid in pids]
            peak_rss = max(peak_rss, sum(r for r, _ in vals))
            peak_fp = max(peak_fp, sum(f for _, f in vals))
        sessions.append({
            "root_pid": root,
            "start": min(p["first"] for p in members),
            "end": max(p["last"] for p in members),
            "cpu_s": round(sum(p["cpu"] for p in members), 3),
            "peak_rss_bytes": peak_rss,
            "peak_footprint_bytes": peak_fp,
            "processes": len(members),
        })

    out = {
        "app": app,
        "interval": args.interval,
        "total": {
            "cpu_s": round(sum(p["cpu"] for p in procs.values()), 3),
            "peak_rss_bytes": max((s[1] for s in samples), default=0),
            "peak_footprint_bytes": max((s[2] for s in samples), default=0),
            "processes": len(procs),
        },
        "sessions": sessions,
        "processes": sorted(procs.values(), key=lambda p: p["first"]),
        "timeline": [s[:4] for s in samples],
    }
    with open(args.out, "w") as f:
        json.dump(out, f)


if __name__ == "__main__":
    main()
