# Allocator memory-error probes

Scenarios that commit common heap bugs and record how the allocator responded.
They are compiled into mozglue, next to the allocator, by `./mach build` when
`--enable-alloc-probes` is on (the default for browser builds on macOS and
Linux). So the allocator under test is exactly the one the build ships, with
the same configuration and hardening as the build used for performance testing.

`browser/app/nsBrowserApp.cpp` calls `MozAllocProbesMain()` first thing in
`main()`. By then the allocator is what `malloc()` reaches; on macOS its zone
has been registered by mozglue's static initializers. If the first argument is
`--alloc-probe`, the probe runs and the process exits without starting Gecko:

    firefox --alloc-probe list          # prints "<name> <category>" per probe
    firefox --alloc-probe NAME TRIAL    # runs one probe once

On macOS, run `Contents/MacOS/firefox` inside the build's `.app` bundle.

## How a trial works

A trial is one process. The trial number seeds `shape_heap()`, which leaves a
few hundred live and freed objects of mixed sizes, and it also seeds the choice
of victim object. So each trial sees a different heap state. The probe prints
markers to fd 1 with `write()`: `BUG_BEGIN`, `BUG_END`,
`RESULT <outcome> <frac> [detail...]`, `SETTLED`. A 10 s `alarm()` turns a hang
(an allocator looping in a corrupted free list) into a SIGALRM crash. Fatal
signals are caught, reported as `CRASH <signal>` and turned into exit status
128 + signal, so thousands of trials don't each leave an OS crash report.

| Outcome | Meaning |
|---|---|
| detected | died before `BUG_END`: at the violating operation |
| detected_late | died after `BUG_END`, before `RESULT`: a later allocator call noticed |
| exploitable | survived and the primitive worked (a live object was modified or read, a heap pointer leaked, two live allocations overlapped, `malloc` returned an attacker-chosen address) |
| neutralized | survived and the attacker got nothing |
| *_then_crash | `RESULT` printed, then died during the settle phase or at exit |
| error | no stage reached, or timeout: a harness problem |

`frac` is probe-specific: the share of written bytes that landed in live
objects, or the share of bytes read that were another object's data. Detail
tokens say how the primitive worked:

| Token | Meaning |
|---|---|
| `adj=seq` | the corrupted object was the victim's allocation-order neighbour, so an attacker allocating in sequence knows what they hit |
| `adj=other` | a live object was corrupted, but not the allocation-order neighbour |
| `adj=none` | no batch object sat in the slot right after the victim |
| `ptr_leak=N` | N pointer-sized words that point into the heap were read |
| `reclaim=I` | the I-th sprayed allocation reclaimed the freed or poisoned memory |
| `freed_live` | the double free released an object that was live again |
| `inplace`, `realloc0_inplace` | `realloc` kept the block in place, so there was no stale pointer |
| `no_slack` | the usable size equals the request |
| `usable=N` | an overflowing size request succeeded with N usable bytes |

Exit status: 0 when the probe finished, 2 for bad arguments, 3 for an unknown
probe name.

## Probes

Names are `<prefix>.<primitive>.<size>[.<variant>]`. `--alloc-probe list`
prints each name with one of the six categories: `oob_write`, `oob_read`,
`uaf`, `uninit`, `double_free`, `invalid_free`. Sizes cover each allocator's
regimes: 64 and 72 (small, 72 leaves slack), 1K and 4K (medium), 256K
(large), 2M (huge). `sprayN` is how many allocations the attacker makes to
reclaim memory: 1 is a naive first try, 1024 a realistic spray.

- **oob_write** (`oobw.`)
  - `linear.S.L`: overflow of L bytes (1, 8, 64, or the whole slot) past an object.
  - `nul.S`: a single NUL byte past the end, as in an off-by-one string bug.
  - `under.S.8`: 8 bytes before the start.
  - `offset.S.O`: 8 bytes at offset O (mid next slot, 4K, 64K) from the start, skipping what lies in between, as an index bug does.
  - `freelink.S.sprayN`: overflow an attacker pointer into a freed neighbour's first word, where an inline free list keeps its link, then allocate.
  - `xsize.S.T`: overflow from size S objects into interleaved size T objects.
  - `xarena.S`: overflow from an object in a `moz_arena` into the default heap.
  - `intoverflow.{calloc,malloc,realloc,memalign}`: size requests whose arithmetic overflows. The allocator must refuse them.
- **oob_read** (`oobr.`)
  - `linear.S.L` and `under.S.8`: as for writes, reading.
  - `offset.S.O`: as for writes, reading.
  - `ptrs.S`: over-read of neighbours that hold heap pointers.
  - `freed.S`: over-read of freed neighbours, which leaks free-list links.
  - `xsize.S.T`, `xarena.S`: as for writes, reading.
- **uaf** (`uaf.`)
  - `read.S`: stale read. It reports leftover data and leaked heap pointers.
  - `write.S.sprayN`: stale write after the attacker sprays same-size objects.
  - `poison.S.sprayN`: stale write of an attacker pointer into the freed block's free-list link, then allocate.
  - `xsize.S.T`: freed size S memory reclaimed by size T objects.
  - `xarena.S`: freed `moz_arena` memory reclaimed by the default heap.
  - `xthread.S.sprayN`: freed on another thread, reclaimed on the main thread.
  - `realloc.64.sprayN`: the stale pointer left by a `realloc` that moved.
- **uninit** (`uninit.`)
  - `fresh.S`: a fresh block still holds a freed secret, or a free-list pointer.
  - `realloc.S`: the tail added by a `realloc` grow.
  - `slack.S`: the bytes between the request and the usable size.
  - `xsize.S.T`: size S data freed and seen by size T allocations.
  - `memalign.S.A`: `posix_memalign` with alignment A.
  - `calloc.S`: `calloc` returns non-zero memory after dirty frees.
- **double_free** (`dfree.`)
  - `immediate.S.sprayN`: an immediate double free.
  - `intervening.S.sprayN`: one allocation between the frees.
  - `aba.S`: free A, free B, free A, which beats a check against the last free only.
  - `flushed.S`: the second free comes after 2048 frees that flush the thread cache.
  - `xthread.S`: the second free is on another thread.
  - `realloc0.S`: `realloc(p, 0)` followed by `free(p)`.
- **invalid_free** (`ifree.`)
  - `stack`, `global`, `mmap`: pointers the allocator never handed out.
  - `misaligned.64`: `p+1`.
  - `interior.S`: `p+16`.
  - `interior_slot.1024`: an interior pointer aligned like a 64-byte object.
  - `interior_page.256K`: a page-aligned interior pointer.
  - `fake_chunk.64`: a fake chunk with size headers in attacker-controlled heap memory.
  - `wild`: a heap pointer plus 64 MiB.
  - `realloc`: `realloc` on a stack pointer.

For the 2M sizes, sprays above 16 are capped at 64 allocations.

The `xarena` probes need the `moz_arena` API, so system-malloc builds (no
`MOZ_MEMORY`) list 6 fewer probes.

`AllocProbes.c` is compiled at `-O1` with `-fno-builtin`, and without the stack
protector or FORTIFY. That way the compiler can't remove the undefined
behaviour, and only the allocator gets a chance to detect it.

## Scoring the builds

`run.py` runs every probe on each of the 9 builds made from the `mozconfig-*`
files and scores each build on the six categories. Build each objdir first
(`MOZCONFIG=mozconfig-<name> ./mach build`), then:

    python3 memory/probes/run.py                 # 50 trials per probe, all 9 builds
    python3 memory/probes/run.py -n 100 --builds baseline,jemalloc5 --out results/x
    python3 memory/probes/run.py --rescore --out results/x    # scores.csv only

- Builds are hard-coded in `BUILDS` (name → objdir).
- Trials run in parallel (`--jobs`, default one per CPU). 50 trials of every probe on all 9 builds take about 6 minutes.
- `--probes REGEX` selects probes by name.
- Re-running with the same `--out` skips the trials already recorded, so an interrupted run resumes, and a larger `-n` extends one.

`results/<timestamp>/` (or `--out`) holds two files:

- `raw.jsonl`: one line per trial, with `build`, `probe`, `category`, `trial`, `outcome`, `how` (the fatal signal, if any), and `frac`, `detail` and `stderr` when present.
- `scores.csv`: one row per allocator and one column per category. Higher is better. The same table is printed along with each category's maximum, which is its number of probes.

Each trial scores:

| Outcome | Score |
|---|---|
| detected, detected_late, neutralized, neutralized_then_crash | 1 |
| exploitable_then_crash | 0.5 |
| exploitable | 0 |
| error | left out |

A probe's score is the mean over its trials, so a defence that only works some
of the time (sampled guards, randomisation, PHC) earns a fraction. A category's
score is the sum of its probes' scores. A probe that only some builds have
scores 0 where it is missing: system-malloc lacks the `moz_arena` API the
`xarena` probes need, so it gets no credit for arena isolation it doesn't have.

## Limits

- "Exploitable" means the primitive worked within the given spray. It does not mean a real exploit exists.
- Apart from ASLR and the shaped heap, every config is deterministic. Sampled detectors (PHC, a guard on every 2nd extent) therefore show up as a rate across trials, not as per-bug certainty.
- A crash at an unmapped address (for example a write 64 KiB away from a small object) counts as detected, although it is the sparse address space, not a check, that caught it.
- Thread probes use one extra thread. There are no race probes.
