#!/usr/bin/env python3
# Produces rpmalloc.c from an upstream rpmalloc/rpmalloc.c given as argument.
#
# The change: on macOS, rpmalloc's per-thread context is kept behind a pthread
# key instead of a _Thread_local. The first access to a _Thread_local on a
# thread makes dyld malloc() its storage, and that malloc re-enters the
# replaced allocator while it is still setting up (see REPORT section 10).
# Upstream stays untouched; the copy is what gets built.
#
# Each replacement must match exactly once, or the script stops.
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
UPSTREAM = pathlib.Path(sys.argv[1])
OUT = HERE / "rpmalloc.c"

REPLACEMENTS = [
    (
        '#include "malloc.c"\n',
        """// Firefox build: size class rounding for malloc_good_size.
extern size_t
rpmalloc_good_size(size_t size) {
	uint32_t size_class = get_size_class(size);
	if (size_class < SIZE_CLASS_COUNT)
		return global_size_class[size_class].block_size;
	size_t page_size = global_config.page_size;
	return (size + page_size - 1) & ~(page_size - 1);
}

#include "malloc.c"
""",
    ),
    (
        "static _Thread_local thread_context_t global_thread TLS_MODEL = {&global_heap_fallback, 0, 0, 0, 0, 0};\n",
        """#if defined(__APPLE__) && defined(RPMALLOC_PTHREAD_TLS)
// Firefox build: the per-thread context lives behind a pthread key, not a
// _Thread_local. On macOS the first access to a _Thread_local on a thread makes
// dyld malloc() its storage, which re-enters the replaced allocator.
static pthread_key_t thread_ctx_key;
static int thread_ctx_key_created;
static thread_context_t global_thread_early = {&global_heap_fallback, 0, 0, 0, 0, 0};
static void rp_thread_ctx_destroy(void* value);
static thread_context_t*
rp_thread_ctx(void) {
	if (UNEXPECTED(!thread_ctx_key_created))
		return &global_thread_early;
	thread_context_t* ctx = (thread_context_t*)pthread_getspecific(thread_ctx_key);
	if (UNEXPECTED(!ctx)) {
		void* mem = mmap(0, sizeof(thread_context_t), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (mem == MAP_FAILED)
			return &global_thread_early;
		ctx = (thread_context_t*)mem;
		*ctx = (thread_context_t){&global_heap_fallback, 0, 0, 0, 0, 0};
		pthread_setspecific(thread_ctx_key, ctx);
	}
	return ctx;
}
// Runs from pthread TSD cleanup. Finalize the thread (flushes its chain and
// releases its heap), then free the context. Keep the key set while finalizing,
// since pthread has cleared it before calling this destructor.
static void
rp_thread_ctx_destroy(void* value) {
	thread_context_t* ctx = (thread_context_t*)value;
	pthread_setspecific(thread_ctx_key, ctx);
	rpmalloc_thread_finalize();
	pthread_setspecific(thread_ctx_key, NULL);
	munmap(ctx, sizeof(thread_context_t));
}
#define global_thread (*rp_thread_ctx())
#else
static _Thread_local thread_context_t global_thread TLS_MODEL = {&global_heap_fallback, 0, 0, 0, 0, 0};
#endif
""",
    ),
    (
        "#if defined(__APPLE__)\n\t\t_tlv_atexit(thread_free_chain_exit, thread);\n#elif PLATFORM_WINDOWS\n",
        """#if defined(__APPLE__) && !defined(RPMALLOC_PTHREAD_TLS)
		_tlv_atexit(thread_free_chain_exit, thread);
#elif defined(__APPLE__)
		// Firefox build: rp_thread_ctx_destroy finalizes the thread and flushes its chain.
#elif PLATFORM_WINDOWS
""",
    ),
    (
        "pthread_key_created = (pthread_key_create(&pthread_key, rpmalloc_thread_destructor) == 0);",
        "pthread_key_created = (pthread_key_create(&pthread_key, RPMALLOC_PTHREAD_KEY_DTOR) == 0);",
    ),
    (
        "\tglobal_main_thread_id = get_thread_id();\n",
        """#if defined(__APPLE__) && defined(RPMALLOC_PTHREAD_TLS)
	if (!global_module_detached && pthread_key_create(&thread_ctx_key, rp_thread_ctx_destroy) == 0) {
		thread_ctx_key_created = 1;
	}
#endif

	global_main_thread_id = get_thread_id();
""",
    ),
    (
        "#include \"rpmalloc.h\"\n",
        """#include "rpmalloc.h"

#if defined(__APPLE__) && defined(RPMALLOC_PTHREAD_TLS)
#define RPMALLOC_PTHREAD_KEY_DTOR NULL
#else
#define RPMALLOC_PTHREAD_KEY_DTOR rpmalloc_thread_destructor
#endif
""",
    ),
]


def main():
    text = UPSTREAM.read_text()
    for old, new in REPLACEMENTS:
        count = text.count(old)
        if count != 1:
            sys.exit(f"anchor matched {count} times, expected 1:\n{old[:120]!r}")
        text = text.replace(old, new)
    OUT.write_text(text)
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
