snmalloc, vendored for `--enable-alt-allocator=snmalloc` (glue in
`memory/build/alt/snmalloc_glue.cpp`).

Upstream: https://github.com/microsoft/snmalloc at
48997b556044756940e9ccc283b8f1f412cfbbd5.

`src/snmalloc/` (minus `override/`, which holds the malloc/new shims) and
`LICENSE` are copied unmodified. snmalloc is header-only; the glue is built
with `NDEBUG` and `SNMALLOC_USE_WAIT_ON_ADDRESS=1` as upstream CMake does by
default, and uses `SNMALLOC_EXTERNAL_THREAD_ALLOC` to keep the per-thread
allocator behind a pthread key.
