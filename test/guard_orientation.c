/* ----------------------------------------------------------------------------
// Test for per-thread-heap sampled guard page direction (MI_GUARDED).
//
// A front guard page sits directly before the object so underflows (p[-1])
// fault, while the default rear guard catches overflows. Deliberate faulting
// accesses are always performed in a forked child process so the test driver
// itself keeps running; the parent distinguishes an expected protection signal
// (SIGSEGV/SIGBUS) from a failure during setup (a non-zero exit status) or an
// unexpected clean exit.
//
// Limitations: guarded objects use whole OS pages, the user pointer is OS-page
// aligned for front guards, and pinned/huge-page memory cannot host a guard.
// Aligned entry points keep their address/content/usable-size contract but are
// not required to sit directly against the front guard page.
-----------------------------------------------------------------------------*/
#include "mimalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>

// distinct, non-zero exit statuses so a crash can never be confused with the
// single deliberate guarded access that each child performs.
enum {
  EXIT_ALLOC_FAILED   = 21,
  EXIT_CONTENT_FAILED = 22,
};

typedef enum touch_e {
  TOUCH_FRONT_WRITE,   // p[-1] = value
  TOUCH_FRONT_READ,    // read p[-1]
  TOUCH_TAIL_WRITE,    // p[usable] = value (rear guard)
  TOUCH_FRONT_OK,      // p[-1] write expected to succeed (no guard control)
} touch_t;

static size_t page_size;

// Fork a child which configures its own default theap, allocates a guarded
// object of `size`, proves the normal user region works, and then performs one
// deliberate access. The child must not use a parent-created theap: after fork()
// its thread id differs, which a debug build rejects (mi_theap_matches_thread).
// Returns: >0 the terminating signal, 0 clean exit, -1 wait/setup error.
static int touch_in_child(size_t size, touch_t mode, mi_guard_direction_t dir, bool enabled) {
  pid_t child = fork();
  if (child < 0) return -1;
  if (child == 0) {
    mi_theap_t* theap = mi_theap_get_default();
    mi_theap_guarded_set_sample_rate(theap, enabled ? 1 : 0, 1);
    mi_theap_guarded_set_size_bound(theap, 1, 65536);
    mi_theap_guarded_set_direction(theap, dir);
    unsigned char* p = mi_malloc(size);
    if (p == NULL) _exit(EXIT_ALLOC_FAILED);
    // the accessible region must always work before the guarded access.
    for (size_t i = 0; i < size; i++) p[i] = (unsigned char)(i & 0xff);
    for (size_t i = 0; i < size; i++) {
      if (p[i] != (unsigned char)(i & 0xff)) _exit(EXIT_CONTENT_FAILED);
    }
    volatile unsigned char sink = 0;
    switch (mode) {
      case TOUCH_FRONT_WRITE: p[-1] = 7; break;
      case TOUCH_FRONT_READ:  sink = (unsigned char)p[-1]; break;
      case TOUCH_TAIL_WRITE: p[mi_usable_size(p)] = 7; break;
      case TOUCH_FRONT_OK:   p[-1] = 7; break;
    }
    (void)sink;
    _exit(0);  // only reached when the access did not fault
  }
  int status = 0;
  if (waitpid(child, &status, 0) != child) return -1;
  if (WIFSIGNALED(status)) return WTERMSIG(status);
  if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
    fprintf(stderr, "  child setup failed with exit code %d\n", WEXITSTATUS(status));
    return -1;
  }
  return 0;  // clean exit
}

static bool is_fault_signal(int sig) {
  return (sig == SIGSEGV || sig == SIGBUS);
}

// Fork a child that performs only a raw access to an already-allocated,
// fork-inherited pointer (no mimalloc calls, so no theap/thread-id mismatch).
// VM protection (PROT_NONE) is inherited across fork, so this directly tests a
// specific parent object. `offset` is in bytes relative to `p` (e.g. -1, or the
// usable size for a rear-guard touch); `do_read` selects a read vs write.
// Returns: >0 the terminating signal, 0 clean exit, -1 wait error.
static int fault_ptr_in_child(const void* p, ptrdiff_t offset, bool do_read) {
  pid_t child = fork();
  if (child < 0) return -1;
  if (child == 0) {
    volatile unsigned char* q = (volatile unsigned char*)p + offset;
    if (do_read) { (void)(*q); } else { *q = 7; }
    _exit(0);
  }
  int status = 0;
  if (waitpid(child, &status, 0) != child) return -1;
  if (WIFSIGNALED(status)) return WTERMSIG(status);
  return 0;
}

#define CHECK(cond, msg) do { \
  if (!(cond)) { fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); return 1; } \
} while (0)

static mi_theap_t* make_guarded_theap(mi_heap_t* heap, mi_guard_direction_t dir) {
  mi_theap_t* theap = mi_heap_theap(heap);
  mi_theap_guarded_set_sample_rate(theap, 1, 1);  // guard every eligible object, deterministic
  mi_theap_guarded_set_size_bound(theap, 1, 65536);
  mi_theap_guarded_set_direction(theap, dir);
  return theap;
}

// ---------------------------------------------------------------------------
// Phase 1: protection signals in forked children.
// ---------------------------------------------------------------------------
static int phase_signals(void) {
  // front guard: both write and read must fault
  int sig = touch_in_child(64, TOUCH_FRONT_WRITE, MI_GUARD_BEFORE, true);
  CHECK(is_fault_signal(sig), "front guard p[-1] write must signal");
  sig = touch_in_child(200, TOUCH_FRONT_READ, MI_GUARD_BEFORE, true);
  CHECK(is_fault_signal(sig), "front guard p[-1] read must signal");

  // default rear guard still faults on overflow, while p[-1] is reachable
  sig = touch_in_child(64, TOUCH_TAIL_WRITE, MI_GUARD_AFTER, true);
  CHECK(is_fault_signal(sig), "rear guard tail write must signal");
  sig = touch_in_child(64, TOUCH_FRONT_OK, MI_GUARD_AFTER, true);
  CHECK(sig == 0, "rear-guard object must allow p[-1] (no front guard)");

  // a larger object exercises multi-page user regions too
  sig = touch_in_child(8000, TOUCH_FRONT_WRITE, MI_GUARD_BEFORE, true);
  CHECK(is_fault_signal(sig), "large front guard p[-1] write must signal");

  printf("ok: guard direction fault signals (front read/write, rear control)\n");
  return 0;
}

// ---------------------------------------------------------------------------
// Phase 2: in-process functional contract (no faults expected here).
// ---------------------------------------------------------------------------
static int phase_functional(mi_theap_t* t) {
  // mi_malloc: OS-page-adjacent user pointer, requested bytes usable & aligned
  unsigned char* p = mi_theap_malloc(t, 64);
  CHECK(p != NULL, "front guarded malloc");
  CHECK(((uintptr_t)p % page_size) == 0, "front user pointer is OS page aligned");
  CHECK(mi_usable_size(p) >= 64, "usable size covers request");
  CHECK((mi_usable_size(p) % page_size) == 0, "usable size excludes the guard page");
  for (int i = 0; i < 64; i++) p[i] = (unsigned char)i;
  for (int i = 0; i < 64; i++) CHECK(p[i] == (unsigned char)i, "front object read/write");

  // mi_zalloc: every requested byte is zero
  unsigned char* z = mi_theap_zalloc(t, 300);
  CHECK(z != NULL, "front guarded zalloc");
  for (int i = 0; i < 300; i++) CHECK(z[i] == 0, "zalloc zero bytes");
  CHECK(mi_usable_size(z) >= 300, "zalloc usable size");

  // mi_calloc: zero initialized as well
  int* ip = mi_theap_calloc(t, 128, sizeof(int));
  CHECK(ip != NULL, "front guarded calloc");
  for (int i = 0; i < 128; i++) CHECK(ip[i] == 0, "calloc zero bytes");
  mi_free(ip);
  mi_free(z);
  mi_free(p);

  // realloc moving to a larger block (current theap direction is BEFORE):
  // prefix content is preserved and the new object stays front guarded.
  p = mi_theap_malloc(t, 300);
  CHECK(p != NULL, "realloc source alloc");
  memset(p, 0x34, 300);
  size_t old_usable = mi_usable_size(p);
  unsigned char* q = mi_theap_realloc(t, p, old_usable + 4096);
  CHECK(q != NULL, "realloc move succeeds");
  for (int i = 0; i < 300; i++) CHECK(q[i] == 0x34, "realloc preserves prefix");
  CHECK(mi_usable_size(q) >= old_usable + 4096, "realloc grown usable size");

  // realloc failure (overflow size) keeps the original object and its protection
  unsigned char* bad = mi_theap_realloc(t, q, SIZE_MAX);
  CHECK(bad == NULL, "huge realloc fails");
  CHECK(q[0] == 0x34 && q[299] == 0x34, "failed realloc preserves object");
  mi_free(q);

  // rezalloc: force a relocation (new size beyond the current usable size) so the
  // expansion rule applies: preserved prefix stays, the newly grown bytes are zero.
  unsigned char* s = mi_theap_malloc(t, 256);
  CHECK(s != NULL, "rezalloc source alloc");
  memset(s, 0x34, 256);
  const size_t src_usable = mi_usable_size(s);
  const size_t grow = src_usable + 2 * page_size;
  unsigned char* r = mi_theap_rezalloc(t, s, grow);
  CHECK(r != NULL, "rezalloc relocates");
  for (int i = 0; i < 256; i++) CHECK(r[i] == 0x34, "rezalloc preserves prefix");
  // only the extension beyond the old usable length is guaranteed zero
  for (size_t i = src_usable; i < grow; i++) CHECK(r[i] == 0, "rezalloc zeroes extension");
  mi_free(r);

  // aligned / aligned-at entry points keep their contract through front blocks
  for (size_t align = 16; align <= 4096; align *= 2) {
    void* a = mi_theap_malloc_aligned(t, 500, align);
    CHECK(a != NULL, "malloc_aligned alloc");
    CHECK(((uintptr_t)a % align) == 0, "malloc_aligned address");
    CHECK(mi_usable_size(a) >= 500, "malloc_aligned usable size");
    memset(a, 0x5a, 500);
    mi_free(a);

    void* az = mi_theap_zalloc_aligned(t, 500, align);
    CHECK(az != NULL && ((uintptr_t)az % align) == 0, "zalloc_aligned address");
    const unsigned char* bz = (const unsigned char*)az;
    for (int i = 0; i < 500; i++) CHECK(bz[i] == 0, "zalloc_aligned zero");
    mi_free(az);
  }
  void* at = mi_malloc_aligned_at(777, 128, 0);
  CHECK(at != NULL && ((uintptr_t)at % 128) == 0, "malloc_aligned_at address");
  CHECK(mi_usable_size(at) >= 777, "aligned_at usable size");
  memset(at, 0x11, 777);
  mi_free(at);

  printf("ok: functional contract (zero, usable_size, realloc/rezalloc, aligned)\n");
  return 0;
}

// ---------------------------------------------------------------------------
// Phase 3: both orientations coexist; freeing uses each object's own record.
// ---------------------------------------------------------------------------
static int phase_coexistence(mi_theap_t* t) {
  mi_theap_guarded_set_direction(t, MI_GUARD_AFTER);
  unsigned char* a = mi_theap_malloc(t, 128);
  CHECK(a != NULL, "after object a");

  mi_theap_guarded_set_direction(t, MI_GUARD_BEFORE);
  unsigned char* b = mi_theap_malloc(t, 128);
  CHECK(b != NULL, "before object b");
  CHECK(((uintptr_t)b % page_size) == 0, "before object b page aligned");

  mi_theap_guarded_set_direction(t, MI_GUARD_AFTER);
  unsigned char* c = mi_theap_malloc(t, 128);
  CHECK(c != NULL, "after object c");

  // all three stay live and writable while orientations differ
  memset(a, 1, 128); memset(b, 2, 128); memset(c, 3, 128);
  CHECK(a[127] == 1 && b[127] == 2 && c[127] == 3, "coexistence content");

  // each object's own recorded direction governs, regardless of the theap's
  // current setting (which is AFTER here): the front object b faults on p[-1],
  // the rear objects a/c do not, while their tail still faults.
  CHECK(is_fault_signal(fault_ptr_in_child(b, -1, false)), "live front object b guards p[-1]");
  CHECK(fault_ptr_in_child(a, -1, false) == 0, "live rear object a allows p[-1]");
  CHECK(is_fault_signal(fault_ptr_in_child(a, (ptrdiff_t)mi_usable_size(a), false)), "rear object a guards its tail");

  // free after switching back and forth: each must remove its own guard page
  mi_free(a);
  mi_free(b);  // front guard recorded on the block, not inferred from current config
  mi_free(c);

  // reclaimed storage must be reusable, with no stale guard, in both directions.
  // (writability in this process proves the old guard was removed; a separate
  // forked child confirms each direction still establishes a fresh guard.)
  mi_theap_guarded_set_direction(t, MI_GUARD_BEFORE);
  unsigned char* d = mi_theap_malloc(t, 128);
  CHECK(d != NULL, "reused storage front alloc");
  memset(d, 4, 128);
  CHECK(d[127] == 4, "reused front storage writable");
  CHECK(is_fault_signal(fault_ptr_in_child(d, -1, false)), "reused front block guards p[-1]");
  mi_free(d);

  mi_theap_guarded_set_direction(t, MI_GUARD_AFTER);
  unsigned char* e = mi_theap_malloc(t, 128);
  CHECK(e != NULL, "reused storage rear alloc");
  memset(e, 5, 128);
  CHECK(is_fault_signal(fault_ptr_in_child(e, (ptrdiff_t)mi_usable_size(e), false)), "reused rear block guards the tail");
  mi_free(e);

  printf("ok: orientation coexistence, per-object unguard, storage reuse\n");
  return 0;
}

// ---------------------------------------------------------------------------
// Phase 4: cross-thread and post-thread-exit frees arranged with join.
// ---------------------------------------------------------------------------
typedef struct shared_s {
  mi_heap_t* heap;
  void* ptr;
} shared_t;

// Each thread must allocate through its own theap for the shared heap.
static void* worker_alloc(void* arg) {
  shared_t* s = (shared_t*)arg;
  mi_theap_t* wt = mi_heap_theap(s->heap);
  mi_theap_guarded_set_sample_rate(wt, 1, 1);
  mi_theap_guarded_set_size_bound(wt, 1, 65536);
  mi_theap_guarded_set_direction(wt, MI_GUARD_BEFORE);
  s->ptr = mi_theap_malloc(wt, 256);
  if (s->ptr != NULL) memset(s->ptr, 0x7e, 256);
  return NULL;
}

static void* worker_free(void* arg) {
  mi_free(arg);  // free an object allocated by a different thread
  return NULL;
}

static int phase_threads(void) {
  mi_heap_t* heap = mi_heap_new();
  mi_theap_t* t = make_guarded_theap(heap, MI_GUARD_BEFORE);

  // (1) allocating thread exits (join), then the object is freed by another thread
  shared_t s1 = { heap, NULL };
  pthread_t th1;
  CHECK(pthread_create(&th1, NULL, worker_alloc, &s1) == 0, "thread create");
  CHECK(pthread_join(th1, NULL) == 0, "thread join (owner exit)");
  CHECK(s1.ptr != NULL, "thread allocated front object");
  CHECK(((unsigned char*)s1.ptr)[255] == 0x7e, "object survived owner thread exit");
  CHECK(is_fault_signal(fault_ptr_in_child(s1.ptr, -1, false)), "worker front object guards p[-1]");
  mi_free(s1.ptr);  // freed after the owning thread has exited; removes its own front guard

  // (2) a different thread frees the object; join before reusing storage
  shared_t s2 = { heap, NULL };
  pthread_t tha, thb;
  CHECK(pthread_create(&tha, NULL, worker_alloc, &s2) == 0, "alloc thread create");
  CHECK(pthread_join(tha, NULL) == 0, "alloc thread join");
  CHECK(s2.ptr != NULL, "cross-thread front object");
  CHECK(pthread_create(&thb, NULL, worker_free, s2.ptr) == 0, "free thread create");
  CHECK(pthread_join(thb, NULL) == 0, "free thread join");

  // storage reclaimed through the cross-thread path must be reusable and guarded
  unsigned char* reuse = mi_theap_malloc(t, 256);
  CHECK(reuse != NULL, "alloc after cross-thread free");
  memset(reuse, 0x6d, 256);
  CHECK(reuse[255] == 0x6d, "reclaimed storage writable (no stale guard)");
  CHECK(is_fault_signal(fault_ptr_in_child(reuse, -1, false)), "reclaimed cross-thread block guards p[-1]");
  mi_free(reuse);

  mi_heap_delete(heap);
  printf("ok: cross-thread free, owner-exit free, post-join reuse\n");
  return 0;
}

// ---------------------------------------------------------------------------
// Phase 5: heap destroy batch cleanup must unguard live front objects.
// ---------------------------------------------------------------------------
static int phase_heap_destroy(void) {
  mi_heap_t* heap = mi_heap_new();
  mi_theap_t* t = make_guarded_theap(heap, MI_GUARD_BEFORE);
  (void)t;  // direction is configured on the heap's thread-local theap used by mi_heap_malloc
  unsigned char* live[8];
  for (int i = 0; i < 8; i++) {
    live[i] = mi_heap_malloc(heap, 64 + 64 * i);
    CHECK(live[i] != NULL, "destroy-scene alloc");
    memset(live[i], i, 64 + 64 * i);
  }
  // destroy while front-guarded objects are still live: batch unguard + free
  mi_heap_destroy(heap);
  printf("ok: heap destroy batch cleanup of live front guards\n");
  return 0;
}

// ---------------------------------------------------------------------------
// Phase 6: invalid enum handling and sampling-disabled control.
// ---------------------------------------------------------------------------
static int einval_count;
static void error_handler(int err, void* arg) {
  (void)arg;
  if (err == EINVAL) einval_count++;
}

static int phase_controls(mi_theap_t* t) {
  // invalid enum: reported via the error channel, direction unchanged
  einval_count = 0;
  mi_register_error(error_handler, NULL);
  mi_theap_guarded_set_direction(t, MI_GUARD_BEFORE);
  mi_theap_guarded_set_direction(t, (mi_guard_direction_t)999);
  CHECK(einval_count == 1, "invalid direction reported once via EINVAL");
  unsigned char* guarded = mi_theap_malloc(t, 64);
  CHECK(guarded != NULL, "alloc after invalid enum");
  CHECK(is_fault_signal(fault_ptr_in_child(guarded, -1, false)), "direction unchanged after invalid enum");
  mi_free(guarded);
  mi_register_error(NULL, NULL);

  // sampling disabled: no guard page, p[-1] remains reachable (fork-inherited mapping)
  mi_theap_guarded_set_sample_rate(t, 0, 0);
  unsigned char* plain = mi_theap_malloc(t, 64);
  CHECK(plain != NULL, "plain alloc with sampling disabled");
  plain[0] = 1;
  CHECK(fault_ptr_in_child(plain, -1, false) == 0, "sampling disabled leaves p[-1] unguarded");
  mi_free(plain);

  printf("ok: invalid enum rejected (EINVAL, kept direction), sampling disabled control\n");
  return 0;
}

int main(void) {
  long ps = sysconf(_SC_PAGESIZE);  // VM protection granularity, matches the mimalloc OS page
  page_size = (ps > 0 ? (size_t)ps : 4096);

  // dedicated default-direction theap for the in-process phases
  mi_heap_t* main_heap = mi_heap_new();
  mi_theap_t* t = make_guarded_theap(main_heap, MI_GUARD_BEFORE);

  if (phase_signals() != 0) return 1;
  if (phase_functional(t) != 0) return 1;
  if (phase_coexistence(t) != 0) return 1;
  if (phase_threads() != 0) return 1;
  if (phase_heap_destroy() != 0) return 1;
  if (phase_controls(t) != 0) return 1;

  mi_heap_delete(main_heap);
  printf("ALL guard orientation checks passed\n");
  return 0;
}
