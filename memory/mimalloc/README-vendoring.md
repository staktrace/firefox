mimalloc, vendored for `--enable-alt-allocator=mimalloc` (glue in
`memory/build/alt/mimalloc_glue.cpp`).

Upstream: https://github.com/microsoft/mimalloc at
dcd6f09b1d878e06d62949dabea3417eec98c89a (v3.6.1).

`include/`, `src/` and `LICENSE` are copied unmodified. The build compiles
`src/static.c` only, with the defines upstream CMake uses for a release
build (`NDEBUG`, `MI_DEBUG=0`, `MI_STATS=1`, `MI_PROFILE=1`) and without
`MI_MALLOC_OVERRIDE` / `MI_OSX_ZONE`, so it defines no malloc entry points
and registers no zone of its own.
