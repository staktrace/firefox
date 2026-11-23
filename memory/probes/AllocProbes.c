/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Memory-error probes, run against whatever allocator malloc() reaches in
 * this build. One process runs one probe once:
 *
 *   firefox --alloc-probe NAME TRIAL
 *
 * Output protocol (stdout): BUG_BEGIN, BUG_END,
 * RESULT <outcome> <frac> [detail...], SETTLED. See README.md.
 *
 * Everything here is deliberately undefined behaviour. moz.build compiles this
 * file with flags that keep the compiler from removing it. */

#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__APPLE__)
#  include <malloc/malloc.h>
#  define usable(p) malloc_size(p)
#else
#  include <malloc.h>
#  define usable(p) malloc_usable_size(p)
#endif

#include "mozilla/AllocProbes.h"
#ifdef MOZ_MEMORY
#  include "mozmemory.h"
#endif

/* ---- helpers ---- */

/* Stage markers go straight to fd 1 so they survive a crash. */
static void say(const char* s) { (void)!write(1, s, strlen(s)); }
#define BUG_BEGIN() say("BUG_BEGIN\n")
#define BUG_END() say("BUG_END\n")

static inline void* hide(void* p) {
  __asm__ volatile("" : "+r"(p)::"memory");
  return p;
}
static void vfill(void* p, int c, size_t n) {
  volatile unsigned char* q = (volatile unsigned char*)hide(p);
  for (size_t i = 0; i < n; i++) q[i] = (unsigned char)c;
}
static size_t vcount(const void* p, int c, size_t n) {
  const volatile unsigned char* q =
      (const volatile unsigned char*)hide((void*)p);
  size_t k = 0;
  for (size_t i = 0; i < n; i++) k += (q[i] == (unsigned char)c);
  return k;
}
static size_t vnonzero(const void* p, size_t n) { return n - vcount(p, 0, n); }
static void vstore_ptr(void* at, const void* value) {
  *(const void* volatile*)hide(at) = value;
}
static uintptr_t vload_word(const void* at) {
  const volatile unsigned char* q =
      (const volatile unsigned char*)hide((void*)at);
  unsigned char b[sizeof(uintptr_t)];
  uintptr_t w;
  for (size_t i = 0; i < sizeof b; i++) b[i] = q[i];
  memcpy(&w, b, sizeof w);
  return w;
}

static uint64_t s_state;
static uint64_t rnd(void) {
  s_state ^= s_state << 13;
  s_state ^= s_state >> 7;
  s_state ^= s_state << 17;
  return s_state;
}

/* Address range of everything the probes allocated, to recognise leaked heap
 * pointers. */
static uintptr_t s_heap_lo = UINTPTR_MAX, s_heap_hi;
static void* note(void* p, size_t n) {
  uintptr_t a = (uintptr_t)p;
  if (p) {
    if (a < s_heap_lo) s_heap_lo = a;
    if (a + n > s_heap_hi) s_heap_hi = a + n;
  }
  return p;
}
static void* A(size_t n) { return note(malloc(n), n); }

static int is_heap_ptr(uintptr_t w) {
  const uintptr_t win = (uintptr_t)256 << 20;
  uintptr_t lo = s_heap_lo > win ? s_heap_lo - win : 0;
  return w && !(w & 7) && w >= lo && w < s_heap_hi + win;
}
static size_t count_ptrs(const void* p, size_t n) {
  uintptr_t a = ((uintptr_t)p + 7) & ~(uintptr_t)7;
  uintptr_t end = (uintptr_t)p + n;
  size_t k = 0;
  for (; a + sizeof(uintptr_t) <= end; a += sizeof(uintptr_t)) {
    k += is_heap_ptr(vload_word((const void*)a));
  }
  return k;
}

/* Arrays of object pointers come from mmap so they don't disturb the heap. */
static void** objs(size_t n) {
  void* p = mmap(NULL, n * sizeof(void*), PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON, -1, 0);
  if (p == MAP_FAILED) abort();
  return (void**)p;
}

static int overlaps(const void* p, size_t n, const void* lo, size_t len) {
  const char* a = p;
  const char* b = lo;
  return p && a < b + len && b < a + n;
}

static int has_dupe(void** p, size_t n) {
  uintptr_t* s = (uintptr_t*)objs(n);
  size_t m = 0;
  for (size_t i = 0; i < n; i++) {
    if (p[i]) s[m++] = (uintptr_t)p[i];
  }
  for (size_t gap = m / 2; gap; gap /= 2) {
    for (size_t i = gap; i < m; i++) {
      uintptr_t t = s[i];
      size_t j = i;
      for (; j >= gap && s[j - gap] > t; j -= gap) s[j] = s[j - gap];
      s[j] = t;
    }
  }
  for (size_t i = 1; i < m; i++) {
    if (s[i] == s[i - 1]) return 1;
  }
  return 0;
}

/* Heaps a probe can allocate from: the default heap and a moz_arena. */
enum { HEAP_DEFAULT, HEAP_ARENA };
#ifdef MOZ_MEMORY
static arena_id_t s_arenas[2];
static arena_id_t arena(int h) {
  if (!s_arenas[h]) s_arenas[h] = moz_create_arena();
  return s_arenas[h];
}
#endif
static void* halloc(int h, size_t n) {
#ifdef MOZ_MEMORY
  if (h != HEAP_DEFAULT) return note(moz_arena_malloc(arena(h), n), n);
#endif
  (void)h;
  return A(n);
}
#ifdef MOZ_MEMORY
static void hfree(int h, void* p) {
  if (h != HEAP_DEFAULT) {
    moz_arena_free(arena(h), p);
    return;
  }
  free(p);
}
#endif

/* ---- probe description and result ---- */

#define LEN_SLOT ((size_t)-1) /* the usable size of the victim */
#define KiB ((size_t)1024)
#define MiB (KiB * 1024)

typedef struct {
  int exploitable;
  double frac; /* probe-specific measure, e.g. share of bytes leaked */
  char detail[96];
} result_t;

typedef struct probe probe_t;
typedef void (*probe_fn)(const probe_t*, result_t*);
struct probe {
  char name[48];
  const char* category;
  probe_fn fn;
  size_t size;  /* request size of the vulnerable object */
  size_t size2; /* request size of the other object, if any */
  size_t len;   /* bytes overflowed or read, offset, or alignment */
  size_t spray; /* allocations the attacker makes to reclaim memory */
  int var;      /* probe-specific variant */
  int heap;     /* heap of the vulnerable object */
};

static void add_detail(result_t* r, const char* fmt, ...) {
  size_t used = strlen(r->detail);
  if (used + 2 >= sizeof r->detail) return;
  if (used) r->detail[used++] = ' ';
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(r->detail + used, sizeof r->detail - used, fmt, ap);
  va_end(ap);
}

/* Put the heap in an arbitrary, trial-dependent state: a few hundred live and
 * freed objects of mixed sizes, so results do not hinge on a pristine heap. */
static void shape_heap(void) {
  static const size_t sz[] = {16,  24,  48,  64,   96,   128,
                              200, 256, 512, 1000, 2000, 4000};
  void* keep[400];
  int n = 50 + (int)(rnd() % 350);
  for (int i = 0; i < n; i++) {
    keep[i] = A(sz[rnd() % (sizeof sz / sizeof *sz)]);
    vfill(keep[i], 0x11, 16);
  }
  for (int i = 0; i < n; i++) {
    if (rnd() & 1) free(keep[i]);
  }
}

#define CANARY 0xAA
#define CANARY2 0xCC
#define SECRET 0x5A
#define EVIL 0xBB

static char s_target[4096] __attribute__((aligned(64)));
#define TARGET ((void*)(s_target + 1024))

/* A batch of live objects filled with a canary, so a neighbour's contents are
 * recognisable. With a second size, objects of the two kinds are allocated
 * alternately and b holds the second kind. */
typedef struct {
  void** o;
  size_t n;
  int heap;
} batch_t;

static size_t batch_count(size_t size) {
  return size >= MiB ? 8 : size >= 64 * KiB ? 32 : 256;
}

static void batch_fill(batch_t* b, int canary) {
  for (size_t i = 0; i < b->n; i++) {
    if (b->o[i]) vfill(b->o[i], canary, usable(b->o[i]));
  }
}

static void batch2(batch_t* a, int ha, size_t sa, batch_t* b, int hb, size_t sb,
                   size_t cnt) {
  a->o = objs(cnt);
  a->heap = ha;
  a->n = cnt;
  if (b) {
    b->o = objs(cnt);
    b->heap = hb;
    b->n = cnt;
  }
  for (size_t i = 0; i < cnt; i++) {
    a->o[i] = halloc(ha, sa);
    if (b) b->o[i] = halloc(hb, sb);
  }
  batch_fill(a, CANARY);
  if (b) batch_fill(b, CANARY2);
}

static void batch(batch_t* a, size_t size, size_t cnt) {
  batch2(a, HEAP_DEFAULT, size, NULL, 0, 0, cnt);
}

static size_t pick(size_t n) { return 1 + (size_t)(rnd() % (n - 2)); }

static size_t find(const batch_t* b, const void* p) {
  for (size_t i = 0; i < b->n; i++) {
    if (b->o[i] == p) return i;
  }
  return (size_t)-1;
}

/* Bytes of live objects in b (other than skip) that lost their canary. Sets
 * *seq when the allocation-order neighbour v+dir is among them: an attacker
 * who allocates in sequence knows which object they hit. */
static size_t damage(const batch_t* b, size_t skip, int canary, int dir,
                     int* seq) {
  size_t bad = 0;
  *seq = 0;
  for (size_t i = 0; i < b->n; i++) {
    if (i == skip || !b->o[i]) continue;
    size_t u = usable(b->o[i]);
    size_t d = u - vcount(b->o[i], canary, u);
    bad += d;
    if (d && skip != (size_t)-1 && i == skip + dir) *seq = 1;
  }
  return bad;
}

static void judge_damage(const batch_t* b, size_t v, int canary, int dir,
                         size_t len, result_t* r) {
  int seq;
  size_t bad = damage(b, v, canary, dir, &seq);
  r->exploitable = bad > 0;
  r->frac = len ? (double)bad / len : 0;
  if (bad) add_detail(r, seq ? "adj=seq" : "adj=other");
}

static void** spray(size_t size, size_t n, int canary) {
  void** s = objs(n);
  for (size_t i = 0; i < n; i++) {
    s[i] = A(size);
    if (canary >= 0) vfill(s[i], canary, size);
  }
  return s;
}

/* Index of the first of n allocations of size that overlaps [lo, lo+len). */
static size_t reissue(const void* lo, size_t len, size_t size, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (overlaps(A(size), size, lo, len)) return i;
  }
  return (size_t)-1;
}

/* ---- out-of-bounds write ---- */

enum { W_OVER, W_UNDER, W_NUL };

static void oobw_linear(const probe_t* p, result_t* r) {
  batch_t b;
  batch(&b, p->size, batch_count(p->size));
  size_t v = pick(b.n);
  unsigned char* base = b.o[v];
  size_t len = p->len == LEN_SLOT ? usable(base) : p->len;
  BUG_BEGIN();
  if (p->var == W_UNDER) {
    vfill(base - len, EVIL, len);
  } else {
    vfill(base + p->size, p->var == W_NUL ? 0 : EVIL, len);
  }
  BUG_END();
  judge_damage(&b, v, CANARY, p->var == W_UNDER ? -1 : 1, len, r);
}

/* 8 bytes at a chosen offset from the start of the object, past whatever lies
 * in between: an index-based write. */
static size_t nonlinear_offset(const probe_t* p, const void* base) {
  size_t u = usable(base);
  return p->len == LEN_SLOT ? u + u / 2 : p->len;
}
static size_t nonlinear_count(const probe_t* p) {
  size_t reach = p->len == LEN_SLOT ? 2 : p->len / p->size + 2;
  size_t n = 2 * reach + 16;
  return n > batch_count(p->size) ? n : batch_count(p->size);
}

static void oobw_nonlinear(const probe_t* p, result_t* r) {
  batch_t b;
  batch(&b, p->size, nonlinear_count(p));
  size_t v = pick(b.n);
  unsigned char* base = b.o[v];
  BUG_BEGIN();
  vfill(base + nonlinear_offset(p, base), EVIL, 8);
  BUG_END();
  judge_damage(&b, v, CANARY, 1, 8, r);
}

/* Free everything around the victim, then overflow into the next slot's first
 * word, where an inline free list keeps its link. */
static void oobw_freelink(const probe_t* p, result_t* r) {
  batch_t b;
  batch(&b, p->size, batch_count(p->size));
  size_t v = pick(b.n);
  unsigned char* base = b.o[v];
  size_t next = find(&b, base + usable(base));
  for (size_t i = 0; i < b.n; i++) {
    if (i != v && i != next) free(b.o[i]);
  }
  if (next != (size_t)-1) {
    free(b.o[next]);
  } else {
    add_detail(r, "adj=none");
  }
  BUG_BEGIN();
  vstore_ptr(base + usable(base), TARGET);
  BUG_END();
  for (size_t i = 0; i < p->spray; i++) {
    if (A(p->size) == TARGET) {
      r->exploitable = 1;
      add_detail(r, "reclaim=%zu", i);
      break;
    }
  }
}

/* Overflow from one kind of object into the other: a different size class, or
 * the same size in a different heap. */
static void oobw_cross(const probe_t* p, result_t* r) {
  batch_t a, b;
  batch2(&a, p->heap, p->size, &b, HEAP_DEFAULT, p->size2,
         batch_count(p->size));
  size_t v = pick(a.n);
  unsigned char* base = a.o[v];
  size_t len = p->len == LEN_SLOT ? usable(base) : p->len;
  BUG_BEGIN();
  vfill(base + p->size, EVIL, len);
  BUG_END();
  int seq;
  size_t bad = damage(&b, (size_t)-1, CANARY2, 0, &seq);
  r->exploitable = bad > 0;
  r->frac = (double)bad / len;
}

/* Size computations that overflow: the allocator must refuse them. */
enum { INT_CALLOC, INT_MALLOC, INT_REALLOC, INT_MEMALIGN };

static void oobw_intoverflow(const probe_t* p, result_t* r) {
  void* q = NULL;
  void* old = p->var == INT_REALLOC ? A(64) : NULL;
  BUG_BEGIN();
  switch (p->var) {
    case INT_CALLOC:
      q = calloc(SIZE_MAX / 16 + 2, 16);
      break;
    case INT_MALLOC:
      q = malloc(SIZE_MAX - 4096);
      break;
    case INT_REALLOC:
      q = realloc(old, SIZE_MAX - 4096);
      break;
    case INT_MEMALIGN:
      if (posix_memalign(&q, 4096, SIZE_MAX - 100)) q = NULL;
      break;
  }
  BUG_END();
  r->exploitable = q != NULL;
  if (q) add_detail(r, "usable=%zu", (size_t)usable(q));
}

/* ---- out-of-bounds read ---- */

enum { R_OVER, R_UNDER, R_PTRS, R_FREED };

static void oobr_linear(const probe_t* p, result_t* r) {
  batch_t b;
  batch(&b, p->size, batch_count(p->size));
  size_t v = pick(b.n);
  unsigned char* base = b.o[v];
  size_t len = p->len == LEN_SLOT ? usable(base) : p->len;
  if (p->var == R_PTRS) {
    for (size_t i = 0; i < b.n; i++) {
      if (i == v) continue;
      size_t u = usable(b.o[i]);
      for (size_t w = 0; w + sizeof(void*) <= u; w += sizeof(void*)) {
        vstore_ptr((char*)b.o[i] + w, b.o[i]);
      }
    }
  } else if (p->var == R_FREED) {
    for (size_t i = 0; i < b.n; i++) {
      if (i != v) free(b.o[i]);
    }
  }
  unsigned char* at = p->var == R_UNDER ? base - len : base + p->size;
  BUG_BEGIN();
  size_t leaked = 0, ptrs = 0;
  if (p->var == R_PTRS || p->var == R_FREED) {
    ptrs = count_ptrs(at, len);
  } else {
    leaked = vcount(at, CANARY, len);
  }
  BUG_END();
  if (ptrs) {
    r->exploitable = 1;
    r->frac = (double)ptrs * sizeof(void*) / len;
    add_detail(r, "ptr_leak=%zu", ptrs);
  } else {
    r->exploitable = leaked > 0;
    r->frac = (double)leaked / len;
  }
}

static void oobr_nonlinear(const probe_t* p, result_t* r) {
  batch_t b;
  batch(&b, p->size, nonlinear_count(p));
  size_t v = pick(b.n);
  unsigned char* base = b.o[v];
  BUG_BEGIN();
  size_t leaked = vcount(base + nonlinear_offset(p, base), CANARY, 8);
  BUG_END();
  r->exploitable = leaked > 0;
  r->frac = leaked / 8.0;
}

static void oobr_cross(const probe_t* p, result_t* r) {
  batch_t a, b;
  batch2(&a, p->heap, p->size, &b, HEAP_DEFAULT, p->size2,
         batch_count(p->size));
  size_t v = pick(a.n);
  unsigned char* base = a.o[v];
  size_t len = p->len == LEN_SLOT ? usable(base) : p->len;
  BUG_BEGIN();
  size_t leaked = vcount(base + p->size, CANARY2, len);
  BUG_END();
  r->exploitable = leaked > 0;
  r->frac = (double)leaked / len;
}

/* ---- use after free ---- */

/* Frees a few more objects of the size, so free-list links in freed memory
 * point at other free objects. */
static void free_some(size_t size) {
  void* t[8];
  for (int i = 0; i < 8; i++) t[i] = A(size);
  for (int i = 0; i < 8; i++) free(t[i]);
}

static void uaf_read(const probe_t* p, result_t* r) {
  free_some(p->size);
  unsigned char* a = A(p->size);
  vfill(a, SECRET, p->size);
  free(a);
  BUG_BEGIN();
  size_t k = vcount(a, SECRET, p->size);
  size_t ptrs = count_ptrs(a, p->size < 64 ? p->size : 64);
  BUG_END();
  r->frac = (double)k / p->size;
  r->exploitable = k || ptrs;
  if (ptrs) add_detail(r, "ptr_leak=%zu", ptrs);
}

/* Write through the dangling pointer after the attacker has sprayed objects
 * of size2 to reclaim the memory. */
static void uaf_write(const probe_t* p, result_t* r, unsigned char* a) {
  void** s = objs(p->spray);
  for (size_t i = 0; i < p->spray; i++) {
    s[i] = A(p->size2);
    vfill(s[i], CANARY, p->size2);
  }
  BUG_BEGIN();
  vfill(a, EVIL, p->var ? p->size : 8);
  BUG_END();
  for (size_t i = 0; i < p->spray; i++) {
    if (vcount(s[i], EVIL, p->size2)) {
      r->exploitable = 1;
      add_detail(r, "reclaim=%zu", i);
      break;
    }
  }
}

static void uaf_write_reuse(const probe_t* p, result_t* r) {
  unsigned char* a = A(p->size);
  vfill(a, SECRET, p->size);
  free(a);
  uaf_write(p, r, a);
}

#ifdef MOZ_MEMORY
static void uaf_cross_arena(const probe_t* p, result_t* r) {
  unsigned char* a = halloc(p->heap, p->size);
  vfill(a, SECRET, p->size);
  hfree(p->heap, a);
  uaf_write(p, r, a);
}
#endif

static void* thread_free(void* a) {
  free(a);
  return NULL;
}

static void uaf_xthread(const probe_t* p, result_t* r) {
  unsigned char* a = A(p->size);
  vfill(a, SECRET, p->size);
  pthread_t t;
  pthread_create(&t, NULL, thread_free, a);
  pthread_join(t, NULL);
  uaf_write(p, r, a);
}

static void uaf_realloc(const probe_t* p, result_t* r) {
  unsigned char* a = A(p->size);
  vfill(a, SECRET, p->size);
  unsigned char* q = note(realloc(a, 4096), 4096);
  if (q == a) {
    add_detail(r, "inplace");
    BUG_BEGIN();
    BUG_END();
    return;
  }
  uaf_write(p, r, a);
}

static void uaf_freelist_poison(const probe_t* p, result_t* r) {
  void* a = A(p->size);
  free(a);
  BUG_BEGIN();
  vstore_ptr(a, TARGET);
  BUG_END();
  for (size_t i = 0; i < p->spray; i++) {
    if (A(p->size) == TARGET) {
      r->exploitable = 1;
      add_detail(r, "reclaim=%zu", i);
      break;
    }
  }
}

/* ---- uninitialised memory ---- */

static void dirty(size_t size, int n) {
  void** t = objs(n);
  for (int i = 0; i < n; i++) {
    t[i] = A(size);
    vfill(t[i], SECRET, usable(t[i]));
  }
  for (int i = 0; i < n; i++) free(t[i]);
}

static void judge_secret(const void* at, size_t n, size_t ptr_bytes,
                         result_t* r) {
  BUG_BEGIN();
  size_t k = vcount(at, SECRET, n);
  size_t ptrs = ptr_bytes ? count_ptrs(at, ptr_bytes) : 0;
  BUG_END();
  r->frac = n ? (double)k / n : 0;
  r->exploitable = k || ptrs;
  if (ptrs) add_detail(r, "ptr_leak=%zu", ptrs);
}

static void uninit_fresh(const probe_t* p, result_t* r) {
  dirty(p->size, 8);
  unsigned char* b = A(p->size);
  judge_secret(b, p->size, p->size < 16 ? p->size : 16, r);
}

static void uninit_realloc(const probe_t* p, result_t* r) {
  dirty(2 * p->size, 8);
  unsigned char* a = A(p->size);
  vfill(a, 0x11, p->size);
  unsigned char* q = note(realloc(a, 2 * p->size), 2 * p->size);
  judge_secret(q + p->size, p->size, 0, r);
}

static void uninit_slack(const probe_t* p, result_t* r) {
  dirty(p->size, 8);
  unsigned char* b = A(p->size);
  size_t u = usable(b);
  if (u == p->size) add_detail(r, "no_slack");
  judge_secret(b + p->size, u - p->size, 0, r);
}

static void uninit_cross(const probe_t* p, result_t* r) {
  dirty(p->size2, 8);
  size_t n = p->size2 / p->size;
  void** s = spray(p->size, n, -1);
  BUG_BEGIN();
  size_t k = 0;
  for (size_t i = 0; i < n; i++) k += vcount(s[i], SECRET, p->size);
  BUG_END();
  r->frac = (double)k / (n * p->size);
  r->exploitable = k > 0;
}

static void uninit_memalign(const probe_t* p, result_t* r) {
  dirty(p->size, 8);
  void* q = NULL;
  if (posix_memalign(&q, p->len, p->size)) q = NULL;
  if (!q) return;
  note(q, p->size);
  judge_secret(q, p->size, 16, r);
}

static void uninit_calloc(const probe_t* p, result_t* r) {
  dirty(p->size, 8);
  unsigned char* c = note(calloc(1, p->size), p->size);
  BUG_BEGIN();
  size_t k = vnonzero(c, p->size);
  BUG_END();
  r->frac = (double)k / p->size;
  r->exploitable = k > 0;
}

/* ---- double free ---- */

/* Two live allocations now share memory if any pointer repeats. */
static void judge_alias(void** extra, size_t nextra, size_t size, size_t n,
                        result_t* r) {
  void** s = objs(n + nextra);
  for (size_t i = 0; i < n; i++) s[i] = A(size);
  for (size_t i = 0; i < nextra; i++) s[n + i] = extra[i];
  r->exploitable = has_dupe(s, n + nextra);
}

enum { DF_IMMEDIATE, DF_INTERVENING, DF_ABA, DF_FLUSHED };

static void dfree_simple(const probe_t* p, result_t* r) {
  void* a = A(p->size);
  void* live = NULL;
  void* b = NULL;
  free(a);
  if (p->var == DF_INTERVENING) {
    live = A(p->size);
    vfill(live, CANARY, p->size);
    if (live == a) add_detail(r, "freed_live");
  } else if (p->var == DF_ABA) {
    b = A(p->size);
    free(b);
  } else if (p->var == DF_FLUSHED) {
    void** s = spray(p->size, 2048, -1);
    for (size_t i = 0; i < 2048; i++) free(s[i]);
  }
  BUG_BEGIN();
  free(a);
  BUG_END();
  judge_alias(live ? &live : NULL, live ? 1 : 0, p->size,
              p->spray < 2 ? 2 : p->spray, r);
}

typedef struct {
  void* a;
  size_t size, n;
  void** out;
} df_job_t;

static void* thread_dfree(void* arg) {
  df_job_t* j = arg;
  BUG_BEGIN();
  free(j->a);
  BUG_END();
  for (size_t i = 0; i < j->n; i++) j->out[i] = malloc(j->size);
  return NULL;
}

static void dfree_xthread(const probe_t* p, result_t* r) {
  df_job_t j = {A(p->size), p->size, p->spray, objs(p->spray)};
  free(j.a);
  pthread_t t;
  pthread_create(&t, NULL, thread_dfree, &j);
  pthread_join(t, NULL);
  judge_alias(j.out, j.n, p->size, p->spray, r);
}

static void dfree_realloc0(const probe_t* p, result_t* r) {
  void* a = A(p->size);
  void* q = realloc(a, 0);
  if (q == a) {
    add_detail(r, "realloc0_inplace");
    BUG_BEGIN();
    BUG_END();
    return;
  }
  BUG_BEGIN();
  free(a);
  BUG_END();
  judge_alias(q ? &q : NULL, q ? 1 : 0, p->size, p->spray, r);
}

/* ---- invalid free ---- */

static char s_global[1024] __attribute__((aligned(64)));

static void judge_reissue(const void* lo, size_t len, size_t size,
                          result_t* r) {
  size_t i = reissue(lo, len, size, 64);
  r->exploitable = i != (size_t)-1;
  if (r->exploitable) add_detail(r, "reclaim=%zu", i);
}

enum {
  IF_STACK,
  IF_GLOBAL,
  IF_MISALIGNED,
  IF_INTERIOR,
  IF_FAKE_CHUNK,
  IF_WILD,
  IF_MMAP,
  IF_REALLOC
};

static void ifree(const probe_t* p, result_t* r) {
  char stack[512] __attribute__((aligned(64)));
  unsigned char* lo = NULL;
  size_t len = 0;
  void* fake = NULL;
  switch (p->var) {
    case IF_STACK:
      lo = (unsigned char*)hide(stack);
      len = sizeof stack;
      fake = lo + 64;
      break;
    case IF_GLOBAL:
      lo = (unsigned char*)hide(s_global);
      len = sizeof s_global;
      fake = lo + 64;
      break;
    case IF_MISALIGNED:
    case IF_INTERIOR:
      lo = A(p->size);
      len = p->size;
      vfill(lo, CANARY, len);
      fake = lo + (p->var == IF_MISALIGNED ? 1 : p->len);
      break;
    case IF_FAKE_CHUNK:
      /* A size header and a following chunk header, as an inline-metadata
       * allocator would expect them, in memory the attacker controls. */
      lo = A(4096);
      len = 4096;
      vfill(lo, 0, len);
      vstore_ptr(lo + 56, (void*)(uintptr_t)(p->size2 | 1));
      vstore_ptr(lo + 56 + p->size2, (void*)(uintptr_t)(p->size2 | 1));
      fake = lo + 64;
      break;
    case IF_WILD:
      lo = (unsigned char*)((((uintptr_t)A(64)) + 64 * MiB) & ~(uintptr_t)15);
      len = 64;
      fake = lo;
      break;
    case IF_MMAP:
      lo = mmap(NULL, 16 * KiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
                -1, 0);
      if (lo == MAP_FAILED) abort();
      len = 16 * KiB;
      fake = lo;
      break;
    case IF_REALLOC:
      lo = (unsigned char*)hide(stack);
      len = sizeof stack;
      fake = lo + 64;
      break;
  }
  BUG_BEGIN();
  if (p->var == IF_REALLOC) {
    void* q = realloc(fake, 128);
    BUG_END();
    if (q == fake) {
      r->exploitable = 1;
      add_detail(r, "inplace");
      return;
    }
  } else {
    free(fake);
    BUG_END();
  }
  judge_reissue(lo, len, p->size2 ? p->size2 : p->size, r);
}

/* ---- probe table ---- */

#define MAX_PROBES 256
static probe_t s_probes[MAX_PROBES];
static size_t s_nprobes;

static const char C_OOBW[] = "oob_write";
static const char C_OOBR[] = "oob_read";
static const char C_UAF[] = "uaf";
static const char C_UNINIT[] = "uninit";
static const char C_DFREE[] = "double_free";
static const char C_IFREE[] = "invalid_free";

static probe_t* add(const char* category, probe_fn fn, const char* fmt, ...) {
  if (s_nprobes == MAX_PROBES) abort();
  probe_t* p = &s_probes[s_nprobes++];
  memset(p, 0, sizeof *p);
  p->category = category;
  p->fn = fn;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(p->name, sizeof p->name, fmt, ap);
  va_end(ap);
  return p;
}

static const char* size_name(size_t s) {
  static char buf[8][16];
  static int i;
  char* b = buf[i++ % 8];
  if (s >= MiB && !(s % MiB)) {
    snprintf(b, 16, "%zuM", s / MiB);
  } else if (s >= KiB && !(s % KiB)) {
    snprintf(b, 16, "%zuK", s / KiB);
  } else {
    snprintf(b, 16, "%zu", s);
  }
  return b;
}
static const char* len_name(size_t l) {
  return l == LEN_SLOT ? "slot" : size_name(l);
}

static void build_probes(void) {
  static const size_t sizes[] = {64, 72, 1024, 4096, 256 * KiB, 2 * MiB};
  static const size_t sprays[] = {1, 16, 1024};
  static const size_t small[] = {64, 1024};
  static const size_t nonlinear_sizes[] = {64, 1024, 4096};
  static const size_t offsets[] = {LEN_SLOT, 4 * KiB, 64 * KiB};
  static const size_t pairs[][2] = {{64, 128}, {1024, 512}, {4096, 1024}};
#define EACH(i, arr) for (size_t i = 0; i < sizeof(arr) / sizeof(*(arr)); i++)
  probe_t* p;

  /* out-of-bounds write */
  EACH(i, sizes) {
    static const size_t lens[] = {1, 8, 64, LEN_SLOT};
    EACH(j, lens) {
      p = add(C_OOBW, oobw_linear, "oobw.linear.%s.%s", size_name(sizes[i]),
              len_name(lens[j]));
      p->size = sizes[i];
      p->len = lens[j];
    }
    p = add(C_OOBW, oobw_linear, "oobw.nul.%s", size_name(sizes[i]));
    p->size = sizes[i];
    p->len = 1;
    p->var = W_NUL;
    p = add(C_OOBW, oobw_linear, "oobw.under.%s.8", size_name(sizes[i]));
    p->size = sizes[i];
    p->len = 8;
    p->var = W_UNDER;
  }
  EACH(i, nonlinear_sizes) {
    EACH(j, offsets) {
      p = add(C_OOBW, oobw_nonlinear, "oobw.offset.%s.%s",
              size_name(nonlinear_sizes[i]), len_name(offsets[j]));
      p->size = nonlinear_sizes[i];
      p->len = offsets[j];
    }
  }
  EACH(i, small) {
    EACH(j, sprays) {
      p = add(C_OOBW, oobw_freelink, "oobw.freelink.%s.spray%zu",
              size_name(small[i]), sprays[j]);
      p->size = small[i];
      p->spray = sprays[j];
    }
  }
  EACH(i, pairs) {
    p = add(C_OOBW, oobw_cross, "oobw.xsize.%s.%s", size_name(pairs[i][0]),
            size_name(pairs[i][1]));
    p->size = pairs[i][0];
    p->size2 = pairs[i][1];
    p->len = LEN_SLOT;
  }
#ifdef MOZ_MEMORY
  EACH(i, small) {
    p = add(C_OOBW, oobw_cross, "oobw.xarena.%s", size_name(small[i]));
    p->size = p->size2 = small[i];
    p->len = LEN_SLOT;
    p->heap = HEAP_ARENA;
  }
#endif
  {
    static const char* names[] = {"calloc", "malloc", "realloc", "memalign"};
    EACH(i, names) {
      p = add(C_OOBW, oobw_intoverflow, "oobw.intoverflow.%s", names[i]);
      p->var = (int)i;
    }
  }

  /* out-of-bounds read */
  EACH(i, sizes) {
    static const size_t lens[] = {8, 64, LEN_SLOT};
    EACH(j, lens) {
      p = add(C_OOBR, oobr_linear, "oobr.linear.%s.%s", size_name(sizes[i]),
              len_name(lens[j]));
      p->size = sizes[i];
      p->len = lens[j];
    }
    p = add(C_OOBR, oobr_linear, "oobr.under.%s.8", size_name(sizes[i]));
    p->size = sizes[i];
    p->len = 8;
    p->var = R_UNDER;
  }
  EACH(i, small) {
    p = add(C_OOBR, oobr_linear, "oobr.ptrs.%s", size_name(small[i]));
    p->size = small[i];
    p->len = 64;
    p->var = R_PTRS;
    p = add(C_OOBR, oobr_linear, "oobr.freed.%s", size_name(small[i]));
    p->size = small[i];
    p->len = LEN_SLOT;
    p->var = R_FREED;
  }
  EACH(i, nonlinear_sizes) {
    EACH(j, offsets) {
      p = add(C_OOBR, oobr_nonlinear, "oobr.offset.%s.%s",
              size_name(nonlinear_sizes[i]), len_name(offsets[j]));
      p->size = nonlinear_sizes[i];
      p->len = offsets[j];
    }
  }
  EACH(i, pairs) {
    p = add(C_OOBR, oobr_cross, "oobr.xsize.%s.%s", size_name(pairs[i][0]),
            size_name(pairs[i][1]));
    p->size = pairs[i][0];
    p->size2 = pairs[i][1];
    p->len = LEN_SLOT;
  }
#ifdef MOZ_MEMORY
  EACH(i, small) {
    p = add(C_OOBR, oobr_cross, "oobr.xarena.%s", size_name(small[i]));
    p->size = p->size2 = small[i];
    p->len = LEN_SLOT;
    p->heap = HEAP_ARENA;
  }
#endif

  /* use after free */
  EACH(i, sizes) {
    p = add(C_UAF, uaf_read, "uaf.read.%s", size_name(sizes[i]));
    p->size = sizes[i];
  }
  {
    static const size_t reuse_sizes[] = {64, 1024, 256 * KiB, 2 * MiB};
    EACH(i, reuse_sizes) {
      EACH(j, sprays) {
        p = add(C_UAF, uaf_write_reuse, "uaf.write.%s.spray%zu",
                size_name(reuse_sizes[i]), sprays[j]);
        p->size = p->size2 = reuse_sizes[i];
        p->spray = reuse_sizes[i] >= MiB && sprays[j] > 16 ? 64 : sprays[j];
      }
    }
  }
  EACH(i, small) {
    EACH(j, sprays) {
      p = add(C_UAF, uaf_freelist_poison, "uaf.poison.%s.spray%zu",
              size_name(small[i]), sprays[j]);
      p->size = small[i];
      p->spray = sprays[j];
    }
  }
  {
    static const size_t xs[][3] = {{64, 128, 1024},
                                   {1024, 512, 1024},
                                   {1024, 2048, 1024},
                                   {256 * KiB, 64, 8192},
                                   {256 * KiB, 4096, 1024}};
    EACH(i, xs) {
      p = add(C_UAF, uaf_write_reuse, "uaf.xsize.%s.%s", size_name(xs[i][0]),
              size_name(xs[i][1]));
      p->size = xs[i][0];
      p->size2 = xs[i][1];
      p->spray = xs[i][2];
      p->var = 1;
    }
  }
#ifdef MOZ_MEMORY
  EACH(i, small) {
    p = add(C_UAF, uaf_cross_arena, "uaf.xarena.%s", size_name(small[i]));
    p->size = p->size2 = small[i];
    p->spray = 1024;
    p->var = 1;
    p->heap = HEAP_ARENA;
  }
#endif
  EACH(i, small) {
    static const size_t ts[] = {1, 1024};
    EACH(j, ts) {
      p = add(C_UAF, uaf_xthread, "uaf.xthread.%s.spray%zu",
              size_name(small[i]), ts[j]);
      p->size = p->size2 = small[i];
      p->spray = ts[j];
    }
  }
  EACH(j, sprays) {
    p = add(C_UAF, uaf_realloc, "uaf.realloc.64.spray%zu", sprays[j]);
    p->size = p->size2 = 64;
    p->spray = sprays[j];
  }

  /* uninitialised memory */
  EACH(i, sizes) {
    p = add(C_UNINIT, uninit_fresh, "uninit.fresh.%s", size_name(sizes[i]));
    p->size = sizes[i];
  }
  {
    static const size_t rs[] = {64, 1024, 4096};
    EACH(i, rs) {
      p = add(C_UNINIT, uninit_realloc, "uninit.realloc.%s", size_name(rs[i]));
      p->size = rs[i];
    }
  }
  {
    static const size_t ss[] = {72, 1000};
    EACH(i, ss) {
      p = add(C_UNINIT, uninit_slack, "uninit.slack.%s", size_name(ss[i]));
      p->size = ss[i];
    }
  }
  {
    static const size_t xs[][2] = {{256, 1024}, {64, 4096}};
    EACH(i, xs) {
      p = add(C_UNINIT, uninit_cross, "uninit.xsize.%s.%s", size_name(xs[i][1]),
              size_name(xs[i][0]));
      p->size = xs[i][0];
      p->size2 = xs[i][1];
    }
  }
  {
    static const size_t ms[][2] = {{64, 64}, {4096, 4096}};
    EACH(i, ms) {
      p = add(C_UNINIT, uninit_memalign, "uninit.memalign.%s.%s",
              size_name(ms[i][0]), size_name(ms[i][1]));
      p->size = ms[i][0];
      p->len = ms[i][1];
    }
  }
  {
    static const size_t cs[] = {64, 1024, 256 * KiB};
    EACH(i, cs) {
      p = add(C_UNINIT, uninit_calloc, "uninit.calloc.%s", size_name(cs[i]));
      p->size = cs[i];
    }
  }

  /* double free */
  {
    static const size_t ds[] = {64, 1024, 256 * KiB, 2 * MiB};
    EACH(i, ds) {
      EACH(j, sprays) {
        p = add(C_DFREE, dfree_simple, "dfree.immediate.%s.spray%zu",
                size_name(ds[i]), sprays[j]);
        p->size = ds[i];
        p->spray = ds[i] >= MiB && sprays[j] > 16 ? 64 : sprays[j];
        p->var = DF_IMMEDIATE;
      }
    }
  }
  EACH(i, small) {
    EACH(j, sprays) {
      p = add(C_DFREE, dfree_simple, "dfree.intervening.%s.spray%zu",
              size_name(small[i]), sprays[j]);
      p->size = small[i];
      p->spray = sprays[j];
      p->var = DF_INTERVENING;
    }
    p = add(C_DFREE, dfree_simple, "dfree.aba.%s", size_name(small[i]));
    p->size = small[i];
    p->spray = 16;
    p->var = DF_ABA;
    p = add(C_DFREE, dfree_simple, "dfree.flushed.%s", size_name(small[i]));
    p->size = small[i];
    p->spray = 16;
    p->var = DF_FLUSHED;
    p = add(C_DFREE, dfree_xthread, "dfree.xthread.%s", size_name(small[i]));
    p->size = small[i];
    p->spray = 16;
    p = add(C_DFREE, dfree_realloc0, "dfree.realloc0.%s", size_name(small[i]));
    p->size = small[i];
    p->spray = 16;
  }

  /* invalid free */
  p = add(C_IFREE, ifree, "ifree.stack");
  p->size = 64;
  p->var = IF_STACK;
  p = add(C_IFREE, ifree, "ifree.global");
  p->size = 64;
  p->var = IF_GLOBAL;
  p = add(C_IFREE, ifree, "ifree.misaligned.64");
  p->size = 64;
  p->var = IF_MISALIGNED;
  EACH(i, small) {
    p = add(C_IFREE, ifree, "ifree.interior.%s", size_name(small[i]));
    p->size = small[i];
    p->len = 16;
    p->var = IF_INTERIOR;
  }
  /* Interior pointers aligned like a smaller object would be. */
  p = add(C_IFREE, ifree, "ifree.interior_slot.1024");
  p->size = 1024;
  p->size2 = 64;
  p->len = 64;
  p->var = IF_INTERIOR;
  p = add(C_IFREE, ifree, "ifree.interior_page.256K");
  p->size = 256 * KiB;
  p->size2 = 16 * KiB;
  p->len = 16 * KiB;
  p->var = IF_INTERIOR;
  p = add(C_IFREE, ifree, "ifree.fake_chunk.64");
  p->size = 64;
  p->size2 = 80;
  p->var = IF_FAKE_CHUNK;
  p = add(C_IFREE, ifree, "ifree.wild");
  p->size = 64;
  p->var = IF_WILD;
  p = add(C_IFREE, ifree, "ifree.mmap");
  p->size = 64;
  p->size2 = 4096;
  p->var = IF_MMAP;
  p = add(C_IFREE, ifree, "ifree.realloc");
  p->size = 128;
  p->var = IF_REALLOC;
#undef EACH
}

/* ---- driver ---- */

/* Report fatal signals on stdout and exit, rather than dying from them, so a
 * run of thousands of trials doesn't leave a crash report for each. Only
 * async-signal-safe calls. */
static void on_fatal_signal(int sig) {
  char msg[] = "CRASH 00\n";
  msg[6] = (char)('0' + sig / 10 % 10);
  msg[7] = (char)('0' + sig % 10);
  (void)!write(1, msg, sizeof msg - 1);
  _exit(128 + sig);
}

static void catch_fatal_signals(void) {
  static const int sigs[] = {SIGSEGV, SIGBUS, SIGABRT, SIGTRAP,
                             SIGILL,  SIGFPE, SIGALRM};
  stack_t ss;
  ss.ss_size = 64 * KiB;
  ss.ss_flags = 0;
  ss.ss_sp = mmap(NULL, ss.ss_size, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANON, -1, 0);
  if (ss.ss_sp != MAP_FAILED) sigaltstack(&ss, NULL);
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_fatal_signal;
  sa.sa_flags = SA_ONSTACK | SA_RESETHAND;
  sigemptyset(&sa.sa_mask);
  for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++) {
    sigaction(sigs[i], &sa, NULL);
  }
}

/* Exercise the allocator so deferred checks can fire. */
static void settle(void) {
  void* p[64];
  for (int r = 0; r < 4; r++) {
    for (int i = 0; i < 64; i++) p[i] = malloc(16 + (i % 8) * 40);
    for (int i = 0; i < 64; i++) free(p[i]);
  }
}

static int is_probe_flag(const char* arg) {
  if (*arg++ != '-') return 0;
  if (*arg == '-') arg++;
  return !strcmp(arg, "alloc-probe");
}

static int run_probe(const char* name, const char* trial) {
  s_state = 0x9E3779B97F4A7C15ull ^
            (uint64_t)strtoull(trial, NULL, 10) * 0xD1B54A32D192ED03ull;
  for (int i = 0; i < 8; i++) rnd();
  for (size_t i = 0; i < s_nprobes; i++) {
    const probe_t* p = &s_probes[i];
    if (strcmp(p->name, name)) continue;
    /* An allocator stuck in a corrupted free list counts as a crash. */
    catch_fatal_signals();
    alarm(10);
    shape_heap();
    result_t r;
    memset(&r, 0, sizeof r);
    p->fn(p, &r);
    char buf[160];
    snprintf(buf, sizeof buf, "RESULT %s %.3f%s%s\n",
             r.exploitable ? "exploitable" : "neutralized", r.frac,
             r.detail[0] ? " " : "", r.detail);
    say(buf);
    settle();
    say("SETTLED\n");
    return 0;
  }
  fprintf(stderr, "alloc-probe: unknown probe %s\n", name);
  return 3;
}

MFBT_API int MozAllocProbesMain(int aArgc, char** aArgv) {
  if (aArgc < 2 || !is_probe_flag(aArgv[1])) return -1;
  build_probes();
  if (aArgc == 3 && !strcmp(aArgv[2], "list")) {
    for (size_t i = 0; i < s_nprobes; i++) {
      printf("%s %s\n", s_probes[i].name, s_probes[i].category);
    }
    fflush(stdout);
    return 0;
  }
  if (aArgc != 4) {
    fprintf(stderr,
            "usage: %s --alloc-probe list\n"
            "       %s --alloc-probe NAME TRIAL\n",
            aArgv[0], aArgv[0]);
    return 2;
  }
  return run_probe(aArgv[2], aArgv[3]);
}
