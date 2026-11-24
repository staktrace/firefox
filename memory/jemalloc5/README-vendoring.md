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
        --disable-doc --enable-opt-safety-checks --enable-uaf-detection \
        --with-malloc-conf=san_guard_small:1,san_guard_large:1,lg_san_uaf_align:14,junk:alloc
    make build_lib_static

then copying `include/jemalloc/*.h` and, from `include/jemalloc/internal/`,
`jemalloc_internal_defs.h`, `jemalloc_preamble.h`, `private_namespace.h`,
`private_namespace.gen.h`, `public_namespace.h` and `public_unnamespace.h`.
These encode the target (page size, os_unfair_lock, no TLS), so they are only
valid for macOS arm64.

The last three options turn on all of jemalloc's release-mode hardening at its
strongest setting: its safety checks, a guard page around every slab and large
extent, junk-filling of every freed region whose address is 16 KiB aligned
(the smallest alignment allowed, one page), and junk-filling of every new
allocation.
