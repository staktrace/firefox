WebKit's libpas (the allocator behind bmalloc), vendored for
`--enable-alt-allocator=libpas` (glue in `memory/build/alt/libpas_glue.cpp`).

Upstream: https://github.com/WebKit/WebKit at
6379da48f7222ca1ae93a46494e9a9cc3aae4682.

Copied unmodified:
- `src/libpas/`: every `.c`, `.h` and `.def` from
  `Source/bmalloc/libpas/src/libpas/`.
- `src/bmalloc/`: `BPlatform.h`, `BExport.h` and `BCompiler.h` from
  `Source/bmalloc/bmalloc/`, which libpas includes when built as part of
  bmalloc. None of WebKit's C++ bmalloc layer is used.

Each file carries its own BSD-2-Clause license header.

The build mirrors `Source/bmalloc/CMakeLists.txt`: `PAS_BMALLOC=1`,
`PAS_BMALLOC_HIDDEN=1` and `_GNU_SOURCE`, with the same thirteen `.c` files
compiled as C++. The glue provides what WebKit's bmalloc otherwise does
(`g_config` and the `pas_system_heap_*` hooks).

`pas_lock.h` calls `os_unfair_lock_lock_with_flags`, as WebKit does with the
public SDK, so this build runs on macOS 15 or later only.
