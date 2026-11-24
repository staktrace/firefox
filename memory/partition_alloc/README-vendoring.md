Chromium's PartitionAlloc, vendored for
`--enable-alt-allocator=partition_alloc` (glue in
`memory/build/alt/partition_alloc_glue.cpp`).

Upstream: https://chromium.googlesource.com/chromium/src at
4d211296bdb2ca9f76ec081efd70c4b8e3855083
(`base/allocator/partition_allocator/`).

`src/partition_alloc/` is copied unmodified, minus tests, perftests, the
`pointers/` (raw_ptr) and `shim/` directories and GN files. `LICENSE` is
Chromium's top-level LICENSE, which the sources refer to.

`gen/partition_alloc/buildflags.h` and the source lists in
`memory/build/moz.build` come from PartitionAlloc's standalone GN build
(`gn/BUILDCONFIG.gn`), generated on macOS arm64 with:

    gn gen out --args='is_debug=false is_official_build=true
        dcheck_always_on=false use_allocator_shim=true
        use_partition_alloc_as_malloc=true enable_backup_ref_ptr_support=false'

after removing the Linux-only assert in `gn/BUILDCONFIG.gn`, stubbing
`//testing/gtest` and dropping the ARC config line from the shim target (all
in a scratch copy). `buildflags.h` is the output of the
`//src/partition_alloc:buildflags` action (`write_buildflag_header.py`); the
sources are those of the `allocator_base` and `allocator_core` targets. The
shim itself is not built: Firefox's Zone.c and the glue take its place.

This is Chromium's configuration for PartitionAlloc as malloc, except
BackupRefPtr, which only protects Chromium's raw_ptr<T>. For maximum
hardening, `buildflags.h` is then edited by hand: `USE_PARTITION_COOKIE` is 1,
so every slot carries a cookie checked on free, and
`ENABLE_MOVE_METADATA_OUT_OF_GIGACAGE_TRIAL` is 0, so slot span metadata is
always kept outside the GigaCage instead of in a random share of processes.
