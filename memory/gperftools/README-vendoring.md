gperftools tcmalloc, vendored for `--enable-alt-allocator=tcmalloc` (glue in
`memory/build/alt/tcmalloc_glue.cpp`).

Upstream: https://github.com/gperftools/gperftools at
aea7ad396a320992f5de7de85dc0bb03ff648fc6 (gperftools-2.19.2-6).

`src/` (minus `tests/` and `windows/`) and `COPYING` (as `LICENSE`) are copied
unmodified. `config/config.h` is the CMake-generated header from a Release
configure on macOS arm64. The build compiles the `tcmalloc_minimal` source set
with `NDEBUG`, `NO_TCMALLOC_SAMPLES` and `TCMALLOC_SKIP_OVERRIDE`, so it
defines no malloc/new entry points and registers no zone of its own.
