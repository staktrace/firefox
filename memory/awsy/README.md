# Allocator AWSY runs

Runs AWSY (`./mach awsy-test`, the tp6 page set) on each of the 9 allocator
builds made from the `mozconfig-*` files at the top of the checkout, and
writes one CSV row per build. The numbers come from Firefox's own memory
reporters, so unlike `memory/bench`, mozjemalloc's MADV_FREE'd pages are purged
before resident memory is measured.

## Running

Build each objdir first (`MOZCONFIG=mozconfig-<name> ./mach build`). Then, from
anywhere:

    python3 memory/awsy/run.py                    # 3 rounds, all 9 builds
    python3 memory/awsy/run.py --quick --rounds 1 --builds baseline,mimalloc
    python3 memory/awsy/run.py --rescore --out results/x   # CSV only

- Rounds: each round runs every build once, with the build order rotated between rounds.
- A full AWSY run opens all 32 tp6 pages with 15 s between tabs and takes roughly 10 minutes per build. `--quick` opens 3 pages with short waits.
- The tp6 recordings (about 1 GB) are downloaded on the first run into the web root, `obj-host-opt/_tests/awsy/html` by default (`--web-root`), and shared by all builds.
- `--checkpoint` picks the memory report to read. The default is `TabsOpenForceGC`, AWSY's "After tabs open [+30s, forced GC]".
- `--awsy-arg` passes an extra argument to `./mach awsy-test`, for example `--awsy-arg=--iterations=3`.
- Re-running with the same `--out` skips the runs already recorded, and a larger `--rounds` extends a finished run.

`awsy-test` exits with an error on system-malloc, which has no `MOZ_MEMORY` and
so doesn't report `heap-allocated`, because its own perfherder summary needs it
to compute heap-unclassified. That summary isn't used here: a run
counts as good when the checkpoint's report was written and no process crashed.

## Output

`results/awsy-<timestamp>/` (or `--out`) holds:

- `awsy.csv`: one row per build. Each column is the median over the runs, from the checkpoint's report (the last AWSY iteration's, if there are several):

  | column | meaning |
  |---|---|
  | `resident_mb` | AWSY's "Resident Memory": RSS of the main process plus USS of every child process |
  | `rss_mb` | `resident` summed over all processes; counts shared pages once per process |
  | `uss_mb` | `resident-unique` summed over all processes |
  | `footprint_mb` | `resident-phys-footprint` summed over all processes (Activity Monitor's Memory); the GPU process's share includes graphics memory |
  | `processes` | number of processes in the report |

- `raw.jsonl`: one line per run, with the build, round, exit status, metrics and the report used.
- `reports/<build>-r<round>/`: every memory report AWSY wrote for that run. They load in `about:memory`.
- `logs/`: the `mach awsy-test` output of each run.
