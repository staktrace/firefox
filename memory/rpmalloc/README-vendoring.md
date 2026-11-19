rpmalloc, vendored for `--enable-alt-allocator=rpmalloc` (glue in
`memory/build/alt/rpmalloc_glue.cpp`).

Upstream: https://github.com/mjansson/rpmalloc at
b8455bb84368ea58cc163c62e12b8fa8c4112e8b.

Files: `rpmalloc.h`, `malloc.c` and `LICENSE` are copied unmodified.
`rpmalloc.c` is generated from upstream by `update.py`:

    python3 update.py /path/to/upstream/rpmalloc/rpmalloc.c

which keeps the per-thread context behind a pthread key on macOS (a
`_Thread_local` access makes dyld malloc through our own zone) and adds
`rpmalloc_good_size()`.

`rpnew.h` (C++ operator new/delete overrides) is not vendored: mozalloc
already routes them to malloc/free.
