/* ----------------------------------------------------------------------------
// guard_orientation.c
//
// Tests for the per-thread-heap guarded allocation direction:
//   mi_theap_guarded_set_direction(theap, MI_GUARD_AFTER | MI_GUARD_BEFORE)
//
// Guarded touches that must raise SIGSEGV/SIGBUS run in a forked child process.
// A crash alone is not accepted: a sibling child performs the same allocation,
// normal access and free and is required to exit cleanly, so a failure can be
// attributed to the deliberate out-of-bounds touch rather than to allocation or
// cleanup logic. All ordinary functional checks run in the parent process.
//
// Limitations (by design):
//  - Direction only takes effect for objects selected by the existing guarded
//    sampler of a MI_GUARDED build; it never changes the sample countdown, the
//    size bounds or the random sequence.
//  - A front guard uses ordinary OS pages only (no pinned/huge-page arena). The
//    user pointer starts immediately after an inaccessible OS page; aligned /
//    aligned_at entries are not required to sit tight against that page but must
//    still locate the underlying block, keep their alignment/usable-size
//    guarantees and free correctly.
//  - If the required protection cannot be established the allocation fails
//    instead of returning writable memory advertised as guarded.
---------------------------------------------------------------------------- */
#include "mimalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>

// minimal C allocation alignment guaranteed by the ordinary entries
#define MI_TEST_MIN_ALIGN 16

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg) do { \
  checks++; \
  if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; } \
} while (0)

// Run `body` in a forked child. Returns the terminating signal (>0), 0 for a
// clean exit with status 0, or -1 for any other/unexpected outcome.
typedef void (mi_child_body_fn)(void);
static int run_in_child(mi_child_body_fn* body) {
  pid_t const c = fork();
  if (c == 0) {
    body();
    _exit(0);
  }
  if (c < 0) return -1;
  int status = 0;
  if (waitpid(c, &status, 0) != c) return -1;
  if (WIFSIGNALED(status)) return WTERMSIG(status);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
  return -1;
}

static bool is_guard_signal(int sig) {
  return (sig == SIGSEGV || sig == SIGBUS);
}

// Every child configures its own theap so each case is self contained.
static mi_theap_t* configure(mi_guard_direction_t direction) {
  mi_theap_t* const t = mi_theap_get_default();
  mi_theap_guarded_set_sample_rate(t, 1, 1);   // every in-bound allocation is guarded
  mi_theap_guarded_set_size_bound(t, 1, (size_t)1 << 20);
  mi_theap_guarded_set_direction(t, direction);
  return t;
}

// ---- child bodies: deliberate guard touches ----
// Volatile pointers are required so an optimizing compiler cannot elide a "dead"
// out-of-bounds store (the touched object is never read again before exit).
static void child_before_write(void) {
  configure(MI_GUARD_BEFORE);
  unsigned char* p = mi_malloc(64);
  volatile unsigned char* vp = p;
  vp[-1] = 0xAB;                        // forward underflow write
}
static void child_before_read(void) {
  configure(MI_GUARD_BEFORE);
  unsigned char* p = mi_malloc(64);
  volatile unsigned char x = p[-1];     // forward underflow read
  (void)x;
}
static void child_before_deep(void) {
  configure(MI_GUARD_BEFORE);
  unsigned char* p = mi_malloc(64);
  volatile unsigned char* vp = p;
  vp[-4096] = 0xCD;                     // start of the inaccessible OS page
}
static void child_after_tail(void) {
  configure(MI_GUARD_AFTER);
  unsigned char* p = mi_malloc(64);
  volatile unsigned char* vp = p;
  vp[mi_usable_size(p)] = 0xEF;         // one byte past the usable region
}
static void child_sampling_off_underflow(void) {
  mi_theap_t* t = configure(MI_GUARD_BEFORE);
  mi_theap_guarded_set_sample_rate(t, 0, 0);  // sampling disabled: no guard at all
  unsigned char* p = mi_malloc(64);
  volatile unsigned char* vp = p;
  vp[-1] = 1;
  vp[63] = 2;
}
// Allocates using the theap direction inherited across fork; used after a rejected
// direction change to prove the previous direction is still in effect.
static void child_inherited_underflow(void) {
  unsigned char* p = mi_malloc(64);
  volatile unsigned char* vp = p;
  vp[-1] = 1;
}
// Same lifecycle as the faulting children but without the bad touch: must exit cleanly.
static void child_clean_lifecycle(void) {
  configure(MI_GUARD_BEFORE);
  unsigned char* p = mi_zalloc(64);
  for (int i = 0; i < 64; i++) p[i] = (unsigned char)i;
  void* a = mi_malloc_aligned(128, 64);
  mi_free(a);
  mi_free(p);
}

// ---- cross-thread free ----
struct free_arg { void* p; };
static void* free_thread(void* arg) {
  mi_free(((struct free_arg*)arg)->p);
  return NULL;
}

// ---- owner thread exits, object is freed later from another thread ----
static void* g_escaped;
static void* exiting_worker(void* arg) {
  (void)arg;
  configure(MI_GUARD_BEFORE);
  unsigned char* p = mi_malloc(64);
  memset(p, 0x42, 64);
  g_escaped = p;
  return NULL;                          // theap pages are abandoned, object stays alive
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);

  // ---------------------------------------------------------------
  // 1. Dedicated guard touches in child processes
  // ---------------------------------------------------------------
  CHECK(is_guard_signal(run_in_child(child_before_write)), "BEFORE: p[-1] write raises SIGSEGV/SIGBUS");
  CHECK(is_guard_signal(run_in_child(child_before_read)),  "BEFORE: p[-1] read raises SIGSEGV/SIGBUS");
  CHECK(is_guard_signal(run_in_child(child_before_deep)),  "BEFORE: touch at the guard page start raises a signal");
  CHECK(is_guard_signal(run_in_child(child_after_tail)),   "AFTER: one byte past usable size raises a signal");
  // a normal lifecycle in a child must not crash: proves the faults above are specific to the bad touch
  CHECK(run_in_child(child_clean_lifecycle) == 0,          "BEFORE: alloc/use/aligned/free exits cleanly (no cleanup crash)");
  // sampling disabled: a front underflow must not fault
  CHECK(run_in_child(child_sampling_off_underflow) == 0,   "sampling disabled: no guard page, underflow does not fault");

  // ---------------------------------------------------------------
  // 2. Parent process: ordinary guarantees of BEFORE objects
  // ---------------------------------------------------------------
  mi_theap_t* const t = configure(MI_GUARD_BEFORE);

  unsigned char* m = mi_malloc(123);
  CHECK(m != NULL, "mi_malloc returns a block");
  CHECK(((uintptr_t)m % MI_TEST_MIN_ALIGN) == 0, "minimal alignment is preserved");
  for (int i = 0; i < 123; i++) m[i] = (unsigned char)i;
  int roundtrip = 1;
  for (int i = 0; i < 123; i++) if (m[i] != (unsigned char)i) { roundtrip = 0; break; }
  CHECK(roundtrip, "all requested bytes are readable and writable");
  size_t usable = mi_usable_size(m);
  CHECK(usable >= 123, "mi_usable_size covers at least the request");
  m[usable - 1] = 0x11;                 // last usable byte is accessible
  mi_free(m);

  // zero initializing entries zero every requested byte
  unsigned char* z = mi_zalloc(177);
  CHECK(z != NULL, "mi_zalloc returns a block");
  int zzero = 1; for (int i = 0; i < 177; i++) if (z[i]) { zzero = 0; break; }
  CHECK(zzero, "mi_zalloc zeroes every requested byte");
  mi_free(z);

  unsigned int* c = mi_calloc(31, sizeof(unsigned int));
  CHECK(c != NULL, "mi_calloc returns a block");
  int czero = 1; for (int i = 0; i < 31; i++) if (c[i]) { czero = 0; break; }
  CHECK(czero, "mi_calloc zeroes every requested byte");
  mi_free(c);

  // ---------------------------------------------------------------
  // 3. realloc / rezalloc
  // ---------------------------------------------------------------
  unsigned char* r = mi_malloc(64);
  CHECK(r != NULL, "realloc source allocation");
  memset(r, 0x5A, 64);
  // in-place realloc (within [usable/2, usable]) keeps the object and its direction
  size_t r_usable = mi_usable_size(r);
  unsigned char* r2 = mi_realloc(r, r_usable / 2 + 16);
  CHECK(r2 != NULL, "in-place realloc succeeds");
  int prefix = 1; for (int i = 0; i < 64; i++) if (r2[i] != 0x5A) { prefix = 0; break; }
  CHECK(prefix, "realloc preserves the common prefix");
  // a relocation follows the receiving theap's current direction
  mi_theap_guarded_set_direction(t, MI_GUARD_AFTER);
  unsigned char* r3 = mi_realloc(r2, 200000);
  CHECK(r3 != NULL, "relocating realloc succeeds");
  int prefix2 = 1; for (int i = 0; i < 64; i++) if (r3[i] != 0x5A) { prefix2 = 0; break; }
  CHECK(prefix2, "relocating realloc preserves min(old usable, new request) bytes");
  // failing realloc keeps the original object and its protection intact
  unsigned char* r4 = mi_realloc(r3, (size_t)-16);
  CHECK(r4 == NULL, "oversized realloc fails with NULL");
  r3[0] = 0x6B;                        // original still usable
  mi_free(r3);

  // rezalloc keeps the previous extension-zeroing rule (including on recycled blocks)
  for (int i = 0; i < 16; i++) {
    unsigned char* q = mi_malloc(96);
    memset(q, 0x7F, 96);
    mi_free(q);
  }
  mi_theap_guarded_set_direction(t, MI_GUARD_BEFORE);
  unsigned char* ez = mi_zalloc(32);
  unsigned char* ez2 = mi_rezalloc(ez, 256);
  CHECK(ez2 != NULL, "mi_rezalloc succeeds");
  int extzero = 1; for (int i = 32; i < 256; i++) if (ez2[i]) { extzero = 0; break; }
  CHECK(extzero, "mi_rezalloc zeroes the expanded area even over recycled storage");
  mi_free(ez2);

  // ---------------------------------------------------------------
  // 4. aligned and aligned_at entries
  // ---------------------------------------------------------------
  void* a = mi_malloc_aligned(300, 128);
  CHECK(a != NULL && ((uintptr_t)a % 128) == 0, "mi_malloc_aligned returns an aligned pointer");
  memset(a, 0x33, 300);
  CHECK(mi_usable_size(a) >= 300, "aligned usable size covers the request");
  mi_free(a);

  void* aa = mi_malloc_aligned_at(300, 128, 16);
  CHECK(aa != NULL && (((uintptr_t)aa + 16) % 128) == 0, "mi_malloc_aligned_at honors the offset alignment");
  memset(aa, 0x44, 300);
  CHECK(mi_usable_size(aa) >= 300, "aligned_at usable size covers the request");
  mi_free(aa);

  // ---------------------------------------------------------------
  // 5. Both directions coexist; freeing uses the object's own record
  // ---------------------------------------------------------------
  mi_theap_guarded_set_direction(t, MI_GUARD_AFTER);
  unsigned char* after_p = mi_malloc(64);
  CHECK(after_p != NULL, "AFTER object allocated");
  mi_theap_guarded_set_direction(t, MI_GUARD_BEFORE);
  unsigned char* before_p = mi_malloc(64);
  CHECK(before_p != NULL, "BEFORE object allocated alongside an AFTER object");
  memset(after_p, 0x11, 64);
  memset(before_p, 0x22, 64);
  // the theap is currently BEFORE; freeing the older AFTER object must still remove its tail page
  mi_free(after_p);
  mi_free(before_p);

  // recycled storage must not retain an old guard page
  unsigned char* reuse = mi_malloc(64);
  CHECK(reuse != NULL, "reuse after freeing mixed-direction objects");
  memset(reuse, 0x88, 64);
  int reuseok = 1; for (int i = 0; i < 64; i++) if (reuse[i] != 0x88) { reuseok = 0; break; }
  CHECK(reuseok, "recycled storage is fully writable (no residual guard)");
  mi_free(reuse);

  // ---------------------------------------------------------------
  // 6. Cross-thread free (joined), then reuse from the current thread
  // ---------------------------------------------------------------
  unsigned char* xp = mi_malloc(64);
  CHECK(xp != NULL, "cross-thread free source allocation");
  memset(xp, 0x99, 64);
  pthread_t th;
  struct free_arg fa = { xp };
  CHECK(pthread_create(&th, NULL, free_thread, &fa) == 0, "spawn freeing thread");
  CHECK(pthread_join(th, NULL) == 0, "join the freeing thread");
  unsigned char* xp2 = mi_malloc(64);
  CHECK(xp2 != NULL, "allocation after a joined cross-thread free");
  memset(xp2, 0xAA, 64);
  int xok = 1; for (int i = 0; i < 64; i++) if (xp2[i] != 0xAA) { xok = 0; break; }
  CHECK(xok, "storage freed by another thread is reusable");
  mi_free(xp2);

  // ---------------------------------------------------------------
  // 7. Free after the owning thread exited
  // ---------------------------------------------------------------
  pthread_t worker;
  CHECK(pthread_create(&worker, NULL, exiting_worker, NULL) == 0, "spawn exiting worker thread");
  CHECK(pthread_join(worker, NULL) == 0, "join the exiting worker thread");
  unsigned char* ep = (unsigned char*)g_escaped;
  CHECK(ep != NULL, "object outlives its creating thread");
  int kept = 1; for (int i = 0; i < 64; i++) if (ep[i] != 0x42) { kept = 0; break; }
  CHECK(kept, "content survives the owning thread exit");
  mi_free(ep);                        // frees the correct page without consulting a dead theap
  unsigned char* ep2 = mi_malloc(64);
  CHECK(ep2 != NULL, "reuse after freeing an object whose owner thread exited");
  memset(ep2, 0xBB, 64);
  mi_free(ep2);

  // ---------------------------------------------------------------
  // 8. Heap destroy batch cleanup with live front-guard objects
  // ---------------------------------------------------------------
  mi_heap_t* h = mi_heap_new();
  CHECK(h != NULL, "create a secondary heap");
  mi_theap_t* ht = mi_heap_theap(h);
  mi_theap_guarded_set_sample_rate(ht, 1, 1);
  mi_theap_guarded_set_size_bound(ht, 1, (size_t)1 << 20);
  mi_theap_guarded_set_direction(ht, MI_GUARD_BEFORE);
  // leave the objects alive so the destroy must unguard every interior front page in bulk
  void* hp1 = mi_heap_malloc(h, 64);
  void* hp2 = mi_heap_malloc(h, 128);
  CHECK(hp1 != NULL && hp2 != NULL, "allocate live front-guard objects in the secondary heap");
  memset(hp1, 0xAA, 64);
  memset(hp2, 0xBB, 128);
  mi_heap_destroy(h);   // exercises _mi_page_unguard_all over the interior front guard pages

  // ---------------------------------------------------------------
  // 9. Invalid enum is reported and leaves the direction unchanged
  // ---------------------------------------------------------------
  mi_theap_guarded_set_direction(t, MI_GUARD_BEFORE);
  mi_theap_guarded_set_direction(t, (mi_guard_direction_t)0x55AA);  // invalid
  // the direction is still BEFORE: a fresh sampled object must keep faulting in front
  CHECK(is_guard_signal(run_in_child(child_inherited_underflow)),
        "invalid direction enum does not change the current direction");

  if (failures == 0) {
    printf("guard_orientation: all %d checks passed\n", checks);
    return 0;
  }
  printf("guard_orientation: %d/%d checks failed\n", failures, checks);
  return 1;
}
