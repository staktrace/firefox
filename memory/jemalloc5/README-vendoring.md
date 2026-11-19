Upstream jemalloc, vendored for `--enable-alt-allocator=jemalloc5` (glue in
`memory/build/alt/jemalloc5_glue.cpp` and `jemalloc5_helpers.c`).

Upstream: https://github.com/jemalloc/jemalloc at
f6e39597b5eb47362da90bfcb8f84113df43e5a5 (5.4.0-76).

`src/*.c` (the sources upstream builds on macOS, minus `zone.c`),
`include/jemalloc/internal/` and `COPYING` (as `LICENSE`) are copied
unmodified. The generated headers come from an out-of-tree build on macOS
arm64:

    /path/to/jemalloc/configure --with-jemalloc-prefix=je_ --without-export \
        --disable-zone-allocator --disable-cxx --with-private-namespace=moz_je_ \
        --disable-doc
    make build_lib_static

then copying `include/jemalloc/*.h` and, from `include/jemalloc/internal/`,
`jemalloc_internal_defs.h`, `jemalloc_preamble.h`, `private_namespace.h`,
`private_namespace.gen.h`, `public_namespace.h` and `public_unnamespace.h`.
These encode the target (page size, os_unfair_lock, no TLS), so they are only
valid for macOS arm64.
