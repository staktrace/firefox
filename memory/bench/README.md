# Allocator benchmarks

Runs a performance benchmark on each of the 9 allocator builds, which are made
from the `mozconfig-*` files at the top of the checkout, and writes one CSV row
per build. macOS only, since `procmon.py` uses libproc.

## Running

Build each objdir first (`MOZCONFIG=mozconfig-<name> ./mach build`). Then, from
anywhere:

    python3 memory/bench/run.py alloc-micro
    python3 memory/bench/run.py sp3
    python3 memory/bench/run.py alloc-micro --builds baseline,mimalloc --rounds 3
    python3 memory/bench/run.py sp3 --rescore --out results/x   # CSV only

- `alloc-micro` loads `alloc-micro/index.html`: a deterministic, allocation-heavy page of 900 steps (about 15 s). It covers JS, DOM, style, layout and graphics. The page uses `layout.frame_rate=0`, a fresh profile and a real window, and quits through Marionette.
- `sp3` is Speedometer 3, run as `./mach raptor -t speedometer3 --binary <build>` with the build's `MOZCONFIG`. Every browser cycle is a fresh browser and profile.

## How it runs

- Rounds: each round runs every build once, and the build order rotates between rounds, so drift such as thermals spreads evenly across builds. The defaults are 10 rounds for alloc-micro and 5 rounds of 2 browser cycles for sp3.
- Passes: the timing pass runs with nothing attached. The resource pass runs again with `procmon.py`, which samples every process inside the build's `Nightly.app` every 100 ms; its timings are ignored.
- Warm-up: before the first alloc-micro round, each build gets one unrecorded run.
- Resuming: re-running with the same `--out` skips the runs already recorded, and a larger `--rounds` extends a finished run.

Prepare the machine before a run: AC power, other apps quit, the window left
visible and the machine untouched, and `caffeinate -dims`.

## Output

`results/<benchmark>-<timestamp>/` (or `--out`) holds:

- `<benchmark>.csv`: one row per build. Each column is the median of one metric over the runs:

  | column | alloc-micro | sp3 |
  |---|---|---|
  | `wall_s` | page-reported time for all steps | raptor `wallclock-for-tracking-only` |
  | `work_s` | time inside the page's operations, not counting waits for the next frame | |
  | `score` | | Speedometer 3 score, higher is better |
  | `cpu_s` | user + system CPU over all of the build's processes, per browser session | same |
  | `peak_rss_mb` | peak RSS summed over those processes | same |
  | `peak_footprint_mb` | peak `phys_footprint` summed over those processes (Activity Monitor's Memory) | same |

  For every column except `score`, lower is better.
- `raw.jsonl`: one line per run, with the pass, build, round, metrics, monitored sessions and, for alloc-micro, the time per operation.
- `logs/`: the browser or raptor output of each run.

A run fails if it gives no result or (sp3) leaves a crash dump. Failed runs are
left out of the CSV and reported as a warning.

## Caveats

- Summed RSS counts a shared page once in every process that maps it, so `peak_footprint_mb` is closer to the real memory cost.
- `cpu_s` covers the whole browser session, startup and shutdown included. In alloc-micro the GPU process dominates it, because frames aren't rate limited.
- system-malloc has no PHC or hardening, so it is a reference point.
