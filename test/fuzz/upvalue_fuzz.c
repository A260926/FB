// upvalue_fuzz.c
//
// Independent driver for the structured open-upvalue fuzzing chain.
//
// It contains four pieces, deliberately kept in one translation unit so the
// driver is self contained:
//
//   1. A moving test allocator installed through WrenConfiguration.reallocateFn.
//      Every non-zero reallocation is served from a freshly allocated,
//      different address: the valid old contents are copied across and the old
//      block is released. Correctness therefore cannot rely on realloc()
//      happening to keep or move a block.
//
//   2. The public-API execution chain. A decoded Plan is rendered to Wren, run
//      through wrenInterpret + wrenMakeCallHandle + wrenCall, and every model
//      observation is compared against the value that actually came back.
//      Fibers and closures the host must keep are held with WrenHandle roots;
//      borrowed slots and strings are never retained across an allocation.
//
//   3. A deterministic fixed corpus and plan generator. Both feed the same
//      decoder/executor as the libFuzzer entry point.
//
//   4. LLVMFuzzerTestOneInput (when built against libFuzzer) and a main() that
//      runs without libFuzzer using fixed seeds.
//
// Build either way with -DWREN_FUZZ_TESTING; that macro only enables read-only
// relocation/collection counters in the VM and never changes production
// semantics.

#include "upvalue_plan.h"

#include "wren.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Read-only instrumentation exported by the VM under the test macro. The
// counters are plain integers the driver resets before each run; they never
// feed back into interpreter behavior.
#ifdef WREN_FUZZ_TESTING
extern int wren_fuzz_stack_moves;
extern int wren_fuzz_open_upvalue_moves;
extern int wren_fuzz_gc_count;
#else
static int wren_fuzz_stack_moves = 0;
static int wren_fuzz_open_upvalue_moves = 0;
static int wren_fuzz_gc_count = 0;
#endif

#define FUZZ_MODULE "upfuzz"

// ---------------------------------------------------------------------------
// Moving test allocator
// ---------------------------------------------------------------------------

// Every handed-out block carries an inline header and trailing canary bytes.
// The header is padded to a full cache line so the pointer returned to Wren is
// always naturally aligned, and metadata is reached by subtraction instead of
// a separately allocated (and separately corruptible) node list.
#define HDR_SIZE 32
#define REDZONE 16
#define BLOCK_MAGIC 0x5752454Eu   // "WREN"
static const unsigned char CANARY = 0xAB;

typedef struct
{
  size_t size;
  uint32_t magic;
} BlockHeader;

typedef struct
{
  int liveBlocks;
  int peakBlocks;
  int totalAllocs;
  int totalReallocs;
  int totalFrees;
  int moves;          // non-zero reallocs served at a new address
  int canaryFaults;   // overrun, bad header, double/foreign free
} TestHeap;

static void paintCanary(void* user, size_t size)
{
  memset((unsigned char*)user + size, CANARY, REDZONE);
}

static BlockHeader* headerOf(void* user)
{
  return (BlockHeader*)((unsigned char*)user - HDR_SIZE);
}

static int blockIntact(BlockHeader* h)
{
  if (h->magic != BLOCK_MAGIC) return 0;
  const unsigned char* tail =
      (const unsigned char*)h + HDR_SIZE + h->size;
  for (int i = 0; i < REDZONE; i++)
  {
    if (tail[i] != CANARY) return 0;
  }
  return 1;
}

// Wren reallocation contract implemented without ever reusing the incoming
// pointer for a non-zero request. The fresh block is taken while the old one
// is still allocated, which guarantees a distinct address, the valid old
// contents are copied across, and the old block is only released afterwards.
static void* testReallocate(void* memory, size_t newSize, void* userData)
{
  TestHeap* heap = (TestHeap*)userData;

  if (newSize == 0)
  {
    if (memory == NULL) return NULL;
    BlockHeader* h = headerOf(memory);
    if (!blockIntact(h))
    {
      // Double free, foreign pointer, or a detected overrun.
      heap->canaryFaults++;
      return NULL;
    }
    h->magic = 0;   // poison so a second free is caught
    free(h);
    heap->liveBlocks--;
    heap->totalFrees++;
    return NULL;
  }

  if (memory == NULL)
  {
    void* block = malloc(HDR_SIZE + newSize + REDZONE);
    if (block == NULL) return NULL;
    BlockHeader* h = (BlockHeader*)block;
    h->size = newSize;
    h->magic = BLOCK_MAGIC;
    void* user = (unsigned char*)block + HDR_SIZE;
    paintCanary(user, newSize);
    heap->liveBlocks++;
    heap->peakBlocks = heap->peakBlocks > heap->liveBlocks
                         ? heap->peakBlocks : heap->liveBlocks;
    heap->totalAllocs++;
    return user;
  }

  // Grow or shrink: always relocate to a fresh, distinct address.
  BlockHeader* old = headerOf(memory);
  if (!blockIntact(old))
  {
    heap->canaryFaults++;
    return NULL;
  }

  void* newBlock = malloc(HDR_SIZE + newSize + REDZONE);
  if (newBlock == NULL) return NULL;
  BlockHeader* nh = (BlockHeader*)newBlock;
  nh->size = newSize;
  nh->magic = BLOCK_MAGIC;
  void* fresh = (unsigned char*)newBlock + HDR_SIZE;

  size_t copySize = old->size < newSize ? old->size : newSize;
  memcpy(fresh, memory, copySize);
  paintCanary(fresh, newSize);

  old->magic = 0;   // the old block is dead; poison its header, then release
  free(old);
  heap->moves++;
  heap->totalReallocs++;
  return fresh;
}

// ---------------------------------------------------------------------------
// Error capture
// ---------------------------------------------------------------------------

typedef struct
{
  int count;
  char message[256];
} ErrorCapture;

// Single host context installed as the VM user data. The moving allocator and
// the error callback both reach their own slice through it, so wrenSetUserData
// is never called after the VM exists (which would silently re-target the
// allocator's bookkeeping pointer).
typedef struct
{
  TestHeap heap;
  ErrorCapture errors;
} FuzzContext;

static void errorCallback(WrenVM* vm, WrenErrorType type, const char* module,
                          int line, const char* message)
{
  FuzzContext* ctx = (FuzzContext*)wrenGetUserData(vm);
  ErrorCapture* cap = &ctx->errors;
  cap->count++;
  // Copy immediately: message memory is owned by the VM and must not be
  // retained across later allocations.
  if (cap->count <= 4)
  {
    size_t n = strlen(cap->message);
    snprintf(cap->message + n, sizeof(cap->message) - n,
             "[%s] %s:%d: %s | ",
             type == WREN_ERROR_COMPILE ? "compile" :
             type == WREN_ERROR_RUNTIME ? "runtime" : "trace",
             module ? module : "?", line, message ? message : "(null)");
  }
}

// ---------------------------------------------------------------------------
// Yield event schedule derived from a plan
// ---------------------------------------------------------------------------

typedef enum { YIELD_HOST_CALL = 1, YIELD_HOST_FIBER = 2, YIELD_GC = 3 } YieldKind;

typedef struct
{
  YieldKind kind;
  int opIndex;
  int phase;      // 0 = parking yield, 1 = post-action yield
  int a;
  int b;
} YieldEvent;

static int buildYieldSchedule(const UpPlan* plan, YieldEvent* events,
                              int maxEvents)
{
  int n = 0;
  for (int i = 0; i < plan->numOps; i++)
  {
    const UpOp* op = &plan->ops[i];
    if (op->code == UP_OP_CALL && op->b)
    {
      if (n + 2 > maxEvents) return -1;
      events[n++] = (YieldEvent){YIELD_HOST_CALL, i, 0, op->a, op->b};
      events[n++] = (YieldEvent){YIELD_HOST_CALL, i, 1, op->a, op->b};
    }
    else if (op->code == UP_OP_FIBER_RESUME && op->b)
    {
      if (n + 2 > maxEvents) return -1;
      events[n++] = (YieldEvent){YIELD_HOST_FIBER, i, 0, op->a, op->b};
      events[n++] = (YieldEvent){YIELD_HOST_FIBER, i, 1, op->a, op->b};
    }
    else if (op->code == UP_OP_GC)
    {
      if (n + 1 > maxEvents) return -1;
      events[n++] = (YieldEvent){YIELD_GC, i, 0, op->a, op->b};
    }
  }
  return n;
}

// ---------------------------------------------------------------------------
// Execution against a fresh VM
// ---------------------------------------------------------------------------

static void reportMismatch(UpRunReport* r, const UpExpectations* exp,
                           int logIndex, int observed, int isBool, int opIndex,
                           const char* fmt, ...)
{
  if (r->mismatches == 0)
  {
    const UpObservation* o = logIndex < exp->count ? &exp->items[logIndex] : NULL;
    r->firstBadOp = opIndex;
    r->firstBadDecl = o ? o->decl : -1;
    r->expectedValue = o ? o->value : -1;
    r->observedValue = observed;
    va_list args;
    va_start(args, fmt);
    vsnprintf(r->detail, sizeof(r->detail), fmt, args);
    va_end(args);
    if (isBool)
      snprintf(r->detail + strlen(r->detail),
               sizeof(r->detail) - strlen(r->detail),
               " (observed bool %d)", observed);
  }
  r->mismatches++;
}

int upvalueRunWren(const UpPlan* plan, const UpExpectations* expected,
                   const char* label, UpRunReport* report)
{
  memset(report, 0, sizeof(*report));
  report->firstBadOp = -1;
  report->firstBadDecl = -1;

  char source[24 * 1024];
  int sourceLen = upvalueRenderWren(plan, source, sizeof(source));
  if (sourceLen < 0)
  {
    snprintf(report->detail, sizeof(report->detail),
             "rendered program exceeded the source buffer");
    return 0;
  }
  if (getenv("FUZZ_DEBUG"))
  {
    FILE* df = fopen(getenv("FUZZ_DEBUG_SRC") ? getenv("FUZZ_DEBUG_SRC")
                                              : "fuzz-debug.wren", "w");
    if (df != NULL) { fputs(source, df); fclose(df); }
  }

  FuzzContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  TestHeap* heap = &ctx.heap;
  ErrorCapture* errors = &ctx.errors;

  WrenConfiguration config;
  wrenInitConfiguration(&config);
  config.reallocateFn = testReallocate;
  config.userData = &ctx;
  config.errorFn = errorCallback;
  // Small thresholds so allocation-driven collections happen during the run,
  // in addition to the explicit collections at call boundaries.
  config.initialHeapSize = 256;
  config.minHeapSize = 128;
  config.heapGrowthPercent = 50;

  wren_fuzz_stack_moves = 0;
  wren_fuzz_open_upvalue_moves = 0;
  wren_fuzz_gc_count = 0;

  WrenVM* vm = wrenNewVM(&config);

  int ok = 1;

  if (wrenInterpret(vm, FUZZ_MODULE, source) != WREN_RESULT_SUCCESS)
  {
    if (getenv("FUZZ_DEBUG"))
    {
      FILE* df = fopen(getenv("FUZZ_DEBUG_SRC") ? getenv("FUZZ_DEBUG_SRC")
                                                : "fuzz-debug.wren", "w");
      if (df != NULL) { fputs(source, df); fclose(df); }
    }
    snprintf(report->detail, sizeof(report->detail),
             "[%s] interpret failed (errors=%d): %s", label, errors->count,
             errors->count ? errors->message : "no error callback");
    ok = 0;
  }

  WrenHandle* driverHandle = NULL;
  WrenHandle* callHandle = NULL;
  YieldEvent events[UP_MAX_OPS * 2];
  int numEvents = 0;

  if (ok)
  {
    numEvents = buildYieldSchedule(plan, events, UP_MAX_OPS * 2);
    if (numEvents < 0)
    {
      snprintf(report->detail, sizeof(report->detail),
               "[%s] yield schedule overflow", label);
      ok = 0;
    }
  }

  if (ok)
  {
    wrenEnsureSlots(vm, 4);
    wrenGetVariable(vm, FUZZ_MODULE, "Driver", 0);
    driverHandle = wrenGetSlotHandle(vm, 0);
    callHandle = wrenMakeCallHandle(vm, "call()");
  }

  // Drive the script fiber to each scheduled yield, act, then resume. Every
  // observed value is appended to the script-side log in execution order; the
  // model expectation stream is diffed against that one ordered log at the end.
  for (int ei = 0; ei < numEvents && ok; ei++)
  {
    const YieldEvent* ev = &events[ei];

    // Every resume invokes call() on the Driver in slot 0. Earlier host
    // actions (e.g. the log insert) leave other values in the slots, so the
    // receiver must be re-seated from its handle before each wrenCall.
    wrenEnsureSlots(vm, 4);
    wrenSetSlotHandle(vm, 0, driverHandle);
    if (wrenCall(vm, callHandle) != WREN_RESULT_SUCCESS)
    {
      snprintf(report->detail, sizeof(report->detail),
               "[%s] driver resume failed at op %d: %s", label,
               ev->opIndex, errors->count ? errors->message : "?");
      ok = 0;
      break;
    }

    if (ev->kind == YIELD_GC)
    {
      // Explicit collection at a call boundary. Handles are the only roots
      // the host relies on while the script fiber is suspended.
      wrenCollectGarbage(vm);
      report->explicitGc++;
      continue;
    }

    if (ev->phase != 0)
    {
      // Second handshake yield: the action is done; collect before resume so
      // the parked handle is proven to keep the open capture alive.
      wrenCollectGarbage(vm);
      report->explicitGc++;
      continue;
    }

    if (ev->kind == YIELD_HOST_CALL)
    {
      // Fetch the parked closure through a fresh slot, root it with a handle,
      // invoke it through the public call handle, and append the result to the
      // shared log in execution order. The model stream is diffed once at the
      // end, so host and in-script observations stay in one ordered sequence.
      wrenEnsureSlots(vm, 3);
      wrenGetVariable(vm, FUZZ_MODULE, "parkedFn", 0);
      if (wrenGetSlotType(vm, 0) == WREN_TYPE_NULL)
      {
        snprintf(report->detail, sizeof(report->detail),
                 "[%s] op %d: parkedFn was null at the host boundary",
                 label, ev->opIndex);
        ok = 0;
        break;
      }
      WrenHandle* fnHandle = wrenGetSlotHandle(vm, 0);
      wrenSetSlotHandle(vm, 0, fnHandle);
      if (wrenCall(vm, callHandle) != WREN_RESULT_SUCCESS)
      {
        snprintf(report->detail, sizeof(report->detail),
                 "[%s] host closure call failed at op %d: %s", label,
                 ev->opIndex, errors->count ? errors->message : "?");
        wrenReleaseHandle(vm, fnHandle);
        ok = 0;
        break;
      }
      int observed = (int)wrenGetSlotDouble(vm, 0);

      wrenEnsureSlots(vm, 3);
      wrenGetVariable(vm, FUZZ_MODULE, "log", 0);
      wrenSetSlotDouble(vm, 1, (double)observed);
      wrenInsertInList(vm, 0, -1, 1);
      wrenReleaseHandle(vm, fnHandle);
    }
    else // YIELD_HOST_FIBER
    {
      wrenEnsureSlots(vm, 3);
      wrenGetVariable(vm, FUZZ_MODULE, "parkedFiber", 0);
      if (wrenGetSlotType(vm, 0) == WREN_TYPE_NULL)
      {
        snprintf(report->detail, sizeof(report->detail),
                 "[%s] op %d: parkedFiber was null at the host boundary",
                 label, ev->opIndex);
        ok = 0;
        break;
      }
      WrenHandle* fiberHandle = wrenGetSlotHandle(vm, 0);
      wrenSetSlotHandle(vm, 0, fiberHandle);
      if (wrenCall(vm, callHandle) != WREN_RESULT_SUCCESS)
      {
        snprintf(report->detail, sizeof(report->detail),
                 "[%s] host fiber resume failed at op %d: %s", label,
                 ev->opIndex, errors->count ? errors->message : "?");
        wrenReleaseHandle(vm, fiberHandle);
        ok = 0;
        break;
      }

      WrenType t = wrenGetSlotType(vm, 0);
      if (t != WREN_TYPE_NUM)
      {
        snprintf(report->detail, sizeof(report->detail),
                 "[%s] op %d: fiber handed back a non-integer type %d",
                 label, ev->opIndex, (int)t);
        wrenReleaseHandle(vm, fiberHandle);
        ok = 0;
        break;
      }
      int observed = (int)wrenGetSlotDouble(vm, 0);

      wrenEnsureSlots(vm, 3);
      wrenGetVariable(vm, FUZZ_MODULE, "log", 0);
      wrenSetSlotDouble(vm, 1, (double)observed);
      wrenInsertInList(vm, 0, -1, 1);
      wrenReleaseHandle(vm, fiberHandle);
    }
  }

  // One final resume runs the driver to completion after its last yield.
  if (ok)
  {
    wrenEnsureSlots(vm, 4);
    wrenSetSlotHandle(vm, 0, driverHandle);
  }
  if (ok && wrenCall(vm, callHandle) != WREN_RESULT_SUCCESS)
  {
    snprintf(report->detail, sizeof(report->detail),
             "[%s] driver completion failed: %s", label,
             errors->count ? errors->message : "?");
    ok = 0;
  }

  // Diff the complete observation log against the model stream, in order.
  if (ok)
  {
    wrenEnsureSlots(vm, 3);
    wrenGetVariable(vm, FUZZ_MODULE, "log", 0);
    int logged = wrenGetListCount(vm, 0);
    if (logged != expected->count)
    {
      snprintf(report->detail, sizeof(report->detail),
               "[%s] observation count: model %d, Wren log %d", label,
               expected->count, logged);
      report->firstBadOp = -1;
      report->mismatches++;
      ok = 0;
    }

    for (int i = 0; ok && i < logged && i < expected->count; i++)
    {
      wrenGetListElement(vm, 0, i, 1);
      WrenType t = wrenGetSlotType(vm, 1);
      const UpObservation* mo = &expected->items[i];
      int observed;
      int isBool = 0;
      if (t == WREN_TYPE_BOOL)
      {
        observed = wrenGetSlotBool(vm, 1) ? 1 : 0;
        isBool = 1;
      }
      else if (t == WREN_TYPE_NUM)
      {
        observed = (int)wrenGetSlotDouble(vm, 1);
      }
      else
      {
        reportMismatch(report, expected, i, -1, 0, mo->opIndex,
                       "[%s] log %d: unexpected Wren type %d", label, i,
                       (int)t);
        ok = 0;
        break;
      }
      report->comparisons++;
      if (observed != mo->value)
      {
        reportMismatch(report, expected, i, observed, isBool, mo->opIndex,
                       "[%s] log %d op %d decl %d: model %d, Wren %d",
                       label, i, mo->opIndex, mo->decl, mo->value, observed);
        ok = 0;
        break;
      }
    }
  }

  if (driverHandle != NULL) wrenReleaseHandle(vm, driverHandle);
  if (callHandle != NULL) wrenReleaseHandle(vm, callHandle);

  report->allocatorMoves = heap->moves;
  report->stackRelocations = wren_fuzz_stack_moves;
  report->openUpvalueMoves = wren_fuzz_open_upvalue_moves;
  report->gcCount = wren_fuzz_gc_count;

  if (heap->canaryFaults)
  {
    snprintf(report->detail, sizeof(report->detail),
             "[%s] moving allocator detected %d canary/contract fault(s)",
             label, heap->canaryFaults);
    ok = 0;
  }

  wrenFreeVM(vm);

  // Every Wren allocation must have been returned by shutdown.
  if (heap->liveBlocks != 0)
  {
    if (ok)
    {
      snprintf(report->detail, sizeof(report->detail),
               "[%s] test allocator leak: %d live blocks after wrenFreeVM",
               label, heap->liveBlocks);
    }
    ok = 0;
  }

  return ok;
}

// Decode, model, then execute in a new VM. A rejected input returns 0 without
// being treated as an interpreter defect.
int upvalueFuzzRunInput(const uint8_t* data, size_t size, const char* label,
                        UpRunReport* report)
{
  // A rejected input must leave a defined, all-zero report so a caller can
  // never mistake its own stale counters for an interpreter result.
  memset(report, 0, sizeof(*report));
  report->firstBadOp = -1;
  report->firstBadDecl = -1;

  UpPlan plan;
  UpDecodeResult dr = upvaluePlanDecode(data, size, &plan);
  if (dr != UP_DECODE_OK)
  {
    if (getenv("FUZZ_DEBUG"))
      fprintf(stderr, "[debug %s] decode rejected: %s (size %zu)\n", label,
              upvaluePlanDecodeError(dr), size);
    return 0;
  }

  UpExpectations expected;
  if (!upvalueModelRun(&plan, &expected)) return 0;

  if (!upvalueRunWren(&plan, &expected, label, report)) return 1;
  return 0;
}

// ---------------------------------------------------------------------------
// Plan builder shared by the fixed corpus and the generator
// ---------------------------------------------------------------------------

typedef struct
{
  UpPlan plan;
} Builder;

static void builderInit(Builder* b, int deepDepth)
{
  memset(b, 0, sizeof(*b));
  for (int i = 0; i < UP_MAX_DECLS; i++)
  {
    b->plan.decls[i].readerFn = -1;
    b->plan.decls[i].writerFn = -1;
    b->plan.decls[i].locDecl = -1;
    b->plan.decls[i].capturedDecl = -1;
  }
  b->plan.deepDepth = deepDepth;
}

static int addVar(Builder* b, int depth, int initial, int nameTag)
{
  int id = b->plan.numDecls++;
  UpDecl* d = &b->plan.decls[id];
  d->kind = UP_DECL_VAR;
  d->id = id;
  d->blockDepth = depth;
  d->initial = initial;
  d->nameTag = nameTag;
  b->plan.numVars++;
  return id;
}

static int addFn(Builder* b, int depth, UpFnKind kind, int captured,
                 int indirect, int writeValue)
{
  int id = b->plan.numDecls++;
  UpDecl* d = &b->plan.decls[id];
  d->kind = UP_DECL_FN;
  d->id = id;
  d->blockDepth = depth;
  d->fnKind = kind;
  d->capturedDecl = captured;
  d->indirect = indirect;
  d->writeValue = writeValue;
  b->plan.numFns++;
  return id;
}

// A Fiber owns an ordinary VAR cell plus a reader FN and, optionally, a
// writer FN. They are normal declaration records referenced by id so the
// model, renderer and decoder share one identity namespace.
static int addFiber(Builder* b, int depth, int yields, int hasWriter,
                    int writeValue)
{
  int loc = addVar(b, depth, 100 + b->plan.numDecls, 0);
  int reader = addFn(b, depth, UP_FN_READ, loc, 0, 0);
  int writer = -1;
  if (hasWriter) writer = addFn(b, depth, UP_FN_WRITE, loc, 0, writeValue);

  int id = b->plan.numDecls++;
  UpDecl* d = &b->plan.decls[id];
  d->kind = UP_DECL_FIBER;
  d->id = id;
  d->blockDepth = depth;
  d->yields = yields;
  d->writeValue = writeValue;
  d->locDecl = loc;
  d->readerFn = reader;
  d->writerFn = writer;
  b->plan.numFiberDecls++;
  return id;
}

static void addOp(Builder* b, UpOpCode code, int a, int bb)
{
  if (b->plan.numOps >= UP_MAX_OPS) return;
  UpOp* op = &b->plan.ops[b->plan.numOps++];
  op->code = code;
  op->a = a;
  op->b = bb;
}

// Encode the partial builder plan and run it through the real decoder so the
// corpus and the fuzzer share one validation/execution path.
static UpDecodeResult builderFinish(Builder* b, uint8_t* bytes, size_t cap,
                                    size_t* outLen, UpPlan* decoded)
{
  b->plan.encodedTopDecls = b->plan.numDecls;
  size_t n = upvaluePlanEncode(&b->plan, bytes, cap);
  if (n == 0) return UP_DECODE_LIMIT;
  *outLen = n;
  UpDecodeResult dr = upvaluePlanDecode(bytes, n, decoded);
  if (dr != UP_DECODE_OK && getenv("FUZZ_DEBUG"))
    fprintf(stderr, "[debug builder] decode rejected: %s (bytes %zu)\n",
            upvaluePlanDecodeError(dr), n);
  return dr;
}

// ---------------------------------------------------------------------------
// Fixed corpus
// ---------------------------------------------------------------------------

// The combined plan required by the specification: read and write closures on
// one shared cell, a suspended fiber with an open capture, two levels of
// indirect capture, and a capture that is closed after its scope exits.
static size_t buildCombinedPlan(uint8_t* bytes, size_t cap)
{
  Builder b;
  builderInit(&b, 22);

  int cell = addVar(&b, 0, 11, 0);
  int reader = addFn(&b, 0, UP_FN_READ, cell, 0, 0);
  int writer = addFn(&b, 0, UP_FN_WRITE, cell, 0, 99);
  int indirectReader = addFn(&b, 0, UP_FN_READ, cell, 2, 0);
  int holder = addVar(&b, 0, 0, 0);
  int fiber = addFiber(&b, 0, 2, 1, 77);

  addOp(&b, UP_OP_DECL, cell, 0);
  addOp(&b, UP_OP_DECL, reader, 0);
  addOp(&b, UP_OP_DECL, writer, 0);
  addOp(&b, UP_OP_DECL, indirectReader, 0);
  addOp(&b, UP_OP_DECL, holder, 0);
  addOp(&b, UP_OP_CALL, reader, 1);       // host-held closure, open capture
  addOp(&b, UP_OP_GC, 0, 0);
  addOp(&b, UP_OP_CALL, writer, 0);       // shared-cell write
  addOp(&b, UP_OP_CALL, reader, 0);       // alias visibility check
  addOp(&b, UP_OP_CALL, indirectReader, 0);
  addOp(&b, UP_OP_FIBER_NEW, fiber, 0);
  addOp(&b, UP_OP_FIBER_RESUME, 0, 1);    // suspended open capture, host-held
  addOp(&b, UP_OP_GC, 0, 0);
  addOp(&b, UP_OP_DEEPEN, 0, 0);          // grow the driver stack while open
  addOp(&b, UP_OP_FIBER_RESUME, 0, 0);    // final resume after internal deep call

  // A nested block closes over a fresh cell and hands the closure outward via
  // an alias; after the block exits the capture must remain valid (closed).
  addOp(&b, UP_OP_SCOPE_ENTER, 0, 0);
  int inner = addVar(&b, 1, 31, 0);
  int innerFn = addFn(&b, 1, UP_FN_READ, inner, 0, 0);
  addOp(&b, UP_OP_DECL, inner, 0);
  addOp(&b, UP_OP_DECL, innerFn, 0);
  addOp(&b, UP_OP_ALIAS, holder, innerFn);
  addOp(&b, UP_OP_SCOPE_EXIT, 0, 0);      // inner cell closes here
  addOp(&b, UP_OP_GC, 0, 0);
  addOp(&b, UP_OP_CALL, holder, 0);       // holder now holds the closed closure

  UpPlan decoded;
  size_t n = 0;
  if (builderFinish(&b, bytes, cap, &n, &decoded) != UP_DECODE_OK) return 0;
  return n;
}

// Same-name shadowing plus same-slot reuse: two declarations spelled "s" in
// sibling blocks reuse a stack slot, and a nested one shadows. All three cells
// must stay independent.
static size_t buildShadowPlan(uint8_t* bytes, size_t cap)
{
  Builder b;
  builderInit(&b, 12);

  int holder = addVar(&b, 0, 0, 0);
  addOp(&b, UP_OP_DECL, holder, 0);

  int s1 = addVar(&b, 1, 41, 1);
  int f1 = addFn(&b, 1, UP_FN_READ, s1, 0, 0);
  addOp(&b, UP_OP_SCOPE_ENTER, 0, 0);
  addOp(&b, UP_OP_DECL, s1, 0);
  addOp(&b, UP_OP_DECL, f1, 0);

  int s2 = addVar(&b, 2, 42, 1);          // nested same-name shadow
  int f2 = addFn(&b, 2, UP_FN_READ, s2, 1, 0);
  addOp(&b, UP_OP_SCOPE_ENTER, 0, 0);
  addOp(&b, UP_OP_DECL, s2, 0);
  addOp(&b, UP_OP_DECL, f2, 0);
  addOp(&b, UP_OP_CALL, f2, 0);
  addOp(&b, UP_OP_SCOPE_EXIT, 0, 0);

  addOp(&b, UP_OP_ALIAS, holder, f1);
  addOp(&b, UP_OP_SCOPE_EXIT, 0, 0);
  addOp(&b, UP_OP_CALL, holder, 0);       // closed capture of the first "s"

  // Sibling block: the slot of the first "s" is reused by the second.
  int s3 = addVar(&b, 1, 43, 1);
  int f3 = addFn(&b, 1, UP_FN_WRITE, s3, 0, 55);
  addOp(&b, UP_OP_SCOPE_ENTER, 0, 0);
  addOp(&b, UP_OP_DECL, s3, 0);
  addOp(&b, UP_OP_DECL, f3, 0);
  addOp(&b, UP_OP_CALL, f3, 1);
  addOp(&b, UP_OP_SCOPE_EXIT, 0, 0);
  addOp(&b, UP_OP_GC, 0, 0);

  UpPlan decoded;
  size_t n = 0;
  if (builderFinish(&b, bytes, cap, &n, &decoded) != UP_DECODE_OK) return 0;
  return n;
}

// A fiber whose open capture is relocated repeatedly while suspended.
static size_t buildFiberMovePlan(uint8_t* bytes, size_t cap)
{
  Builder b;
  builderInit(&b, 26);

  int fiber = addFiber(&b, 0, 3, 1, 88);
  addOp(&b, UP_OP_FIBER_NEW, fiber, 0);
  addOp(&b, UP_OP_FIBER_RESUME, 0, 1);
  addOp(&b, UP_OP_GC, 0, 0);
  addOp(&b, UP_OP_FIBER_RESUME, 0, 1);    // resume after the in-fiber deep call
  addOp(&b, UP_OP_GC, 0, 0);
  addOp(&b, UP_OP_FIBER_RESUME, 0, 0);
  addOp(&b, UP_OP_FIBER_RESUME, 0, 0);    // final value after the writer
  addOp(&b, UP_OP_GC, 0, 0);

  UpPlan decoded;
  size_t n = 0;
  if (builderFinish(&b, bytes, cap, &n, &decoded) != UP_DECODE_OK) return 0;
  return n;
}

// Deterministic detector for the same-slot capture-reuse fix. Two closures
// (a reader and a writer) capture one block-local cell and are both handed
// outward before the block closes. After the close the writer runs, then the
// reader: with one shared cell the reader sees the new value, while a VM that
// minted a second open upvalue keeps the reader on an independent closed copy.
static size_t buildReusePlan(uint8_t* bytes, size_t cap)
{
  Builder b;
  builderInit(&b, 10);

  int holderG = addVar(&b, 0, 0, 0);
  int holderW = addVar(&b, 0, 0, 0);
  addOp(&b, UP_OP_DECL, holderG, 0);
  addOp(&b, UP_OP_DECL, holderW, 0);

  addOp(&b, UP_OP_SCOPE_ENTER, 0, 0);
  int a = addVar(&b, 1, 5, 0);
  int g = addFn(&b, 1, UP_FN_READ, a, 0, 0);
  int w = addFn(&b, 1, UP_FN_WRITE, a, 0, 30);
  addOp(&b, UP_OP_DECL, a, 0);
  addOp(&b, UP_OP_DECL, g, 0);
  addOp(&b, UP_OP_DECL, w, 0);
  addOp(&b, UP_OP_ALIAS, holderG, g);
  addOp(&b, UP_OP_ALIAS, holderW, w);
  addOp(&b, UP_OP_SCOPE_EXIT, 0, 0);       // the cell closes here
  addOp(&b, UP_OP_GC, 0, 0);
  addOp(&b, UP_OP_CALL, holderW, 0);       // write through the closed capture
  addOp(&b, UP_OP_CALL, holderG, 0);       // shared cell: must read 30

  UpPlan decoded;
  size_t n = 0;
  if (builderFinish(&b, bytes, cap, &n, &decoded) != UP_DECODE_OK) return 0;
  return n;
}

#define FIXED_PLAN_COUNT 4

static size_t (*const fixedBuilders[FIXED_PLAN_COUNT])(uint8_t*, size_t) = {
  buildCombinedPlan,
  buildShadowPlan,
  buildFiberMovePlan,
  buildReusePlan
};

static const char* fixedNames[FIXED_PLAN_COUNT] = {
  "combined",
  "shadow-reuse",
  "fiber-move",
  "capture-reuse"
};

// ---------------------------------------------------------------------------
// Deterministic generator
// ---------------------------------------------------------------------------

static uint32_t rngState;

static uint32_t rngNext(void)
{
  // Numerical Recipes LCG; deterministic and dependency free.
  rngState = rngState * 1664525u + 1013904223u;
  return rngState;
}

static int rngRange(int lo, int hi)
{
  if (hi <= lo) return lo;
  return lo + (int)(rngNext() % (uint32_t)(hi - lo + 1));
}

static size_t generatePlan(uint32_t seed, uint8_t* bytes, size_t cap)
{
  rngState = seed * 2654435761u + 1u;
  Builder b;
  builderInit(&b, rngRange(8, UP_MAX_DEPTH));

  int depth = 0;
  int liveVars[UP_MAX_DECLS];
  int numLiveVars = 0;
  int liveFns[UP_MAX_DECLS];
  int numLiveFns = 0;
  int fiberSlots = 0;
  int fiberYields[UP_MAX_FIBERS];
  int fiberResumes[UP_MAX_FIBERS];

  int steps = rngRange(10, 28);
  for (int step = 0; step < steps; step++)
  {
    int choice = rngRange(0, 11);

    if ((choice == 0 || choice == 1) && depth < UP_MAX_NEST &&
        b.plan.numDecls < UP_MAX_DECLS - 6)
    {
      addOp(&b, UP_OP_SCOPE_ENTER, 0, 0);
      depth++;
      int varsHere = rngRange(1, 3);
      for (int k = 0; k < varsHere && b.plan.numDecls < UP_MAX_DECLS - 6; k++)
      {
        int tag = (rngNext() & 3) == 0 ? 1 : 0;
        int v = addVar(&b, depth, rngRange(0, 900), tag);
        addOp(&b, UP_OP_DECL, v, 0);
        liveVars[numLiveVars++] = v;
      }
    }
    else if (choice == 2 && depth > 0)
    {
      addOp(&b, UP_OP_SCOPE_EXIT, 0, 0);
      // Drop declarations that belonged to the closing block.
      int nv = 0, nf = 0;
      for (int k = 0; k < numLiveVars; k++)
      {
        if (b.plan.decls[liveVars[k]].blockDepth < depth)
          liveVars[nv++] = liveVars[k];
      }
      for (int k = 0; k < numLiveFns; k++)
      {
        if (b.plan.decls[liveFns[k]].blockDepth < depth)
          liveFns[nf++] = liveFns[k];
      }
      numLiveVars = nv;
      numLiveFns = nf;
      depth--;
    }
    else if (choice >= 3 && choice <= 5 && numLiveVars > 0 &&
             b.plan.numDecls < UP_MAX_DECLS - 4)
    {
      int cell = liveVars[rngRange(0, numLiveVars - 1)];
      UpFnKind kind = (rngNext() & 1) ? UP_FN_READ : UP_FN_WRITE;
      int indirect = rngRange(0, UP_MAX_INDIRECT);
      int fn = addFn(&b, depth, kind, cell, indirect, rngRange(1, 950));
      addOp(&b, UP_OP_DECL, fn, 0);
      liveFns[numLiveFns++] = fn;
    }
    else if (choice == 6 && numLiveFns > 0)
    {
      int fn = liveFns[rngRange(0, numLiveFns - 1)];
      addOp(&b, UP_OP_CALL, fn, rngRange(0, 1));
    }
    else if (choice == 7 && numLiveFns > 1)
    {
      // The alias target must be an integer local that no closure captures,
      // matching the decoder's typed-cell rule. Try a few candidates.
      int target = -1;
      for (int tries = 0; tries < 6; tries++)
      {
        int cand = liveVars[rngRange(0, numLiveVars - 1)];
        int captured = 0;
        for (int k = 0; k < b.plan.numDecls; k++)
        {
          if (b.plan.decls[k].kind == UP_DECL_FN &&
              b.plan.decls[k].capturedDecl == cand)
          {
            captured = 1;
            break;
          }
        }
        if (!captured) { target = cand; break; }
      }
      if (target >= 0)
      {
        int source = liveFns[rngRange(0, numLiveFns - 1)];
        addOp(&b, UP_OP_ALIAS, target, source);
      }
    }
    else if (choice == 8 && fiberSlots < UP_MAX_FIBERS && depth == 0 &&
             b.plan.numDecls < UP_MAX_DECLS - 4 && b.plan.numOps < UP_MAX_OPS - 6)
    {
      // Fibers are declared at the driver top scope so the Fiber local stays
      // in scope across nested block entry/exit; resumes may occur anywhere.
      int yields = rngRange(1, UP_MAX_YIELDS);
      int fiber = addFiber(&b, 0, yields, rngNext() & 1, rngRange(1, 950));
      addOp(&b, UP_OP_FIBER_NEW, fiber, 0);
      fiberYields[fiberSlots] = yields;
      fiberResumes[fiberSlots] = 0;
      addOp(&b, UP_OP_FIBER_RESUME, fiberSlots, rngRange(0, 1));
      fiberResumes[fiberSlots]++;
      fiberSlots++;
    }
    else if (choice == 9 && fiberSlots > 0)
    {
      int slot = rngRange(0, fiberSlots - 1);
      if (fiberResumes[slot] <= fiberYields[slot])
      {
        addOp(&b, UP_OP_FIBER_RESUME, slot, rngRange(0, 1));
        fiberResumes[slot]++;
      }
    }
    else if (choice == 10)
    {
      addOp(&b, (rngNext() & 1) ? UP_OP_GC : UP_OP_DEEPEN, 0, 0);
    }
  }

  while (depth > 0)
  {
    addOp(&b, UP_OP_SCOPE_EXIT, 0, 0);
    depth--;
  }

  UpPlan decoded;
  size_t n = 0;
  if (builderFinish(&b, bytes, cap, &n, &decoded) != UP_DECODE_OK) return 0;
  return n;
}

// ---------------------------------------------------------------------------
// Entries
// ---------------------------------------------------------------------------

static int runBytes(const uint8_t* data, size_t size, const char* label,
                    int replay, UpRunReport* totals)
{
  UpRunReport report;
  int failed = upvalueFuzzRunInput(data, size, label, &report);
  if (failed)
  {
    fprintf(stderr, "MISMATCH %s: %s\n", label, report.detail);
    return 1;
  }

  // Rejected inputs carry an all-zero report; only count executed plans.
  if (report.comparisons > 0 || report.gcCount > 0 || report.allocatorMoves > 0)
  {
    totals->comparisons += report.comparisons;
    totals->mismatches += report.mismatches;
    totals->allocatorMoves += report.allocatorMoves;
    totals->stackRelocations += report.stackRelocations;
    totals->openUpvalueMoves += report.openUpvalueMoves;
    totals->explicitGc += report.explicitGc;
    totals->gcCount += report.gcCount;
  }

  if (replay)
  {
    UpRunReport again;
    int failed2 = upvalueFuzzRunInput(data, size, label, &again);
    if (failed2)
    {
      fprintf(stderr, "MISMATCH %s (replay): %s\n", label, again.detail);
      return 1;
    }
    if (again.comparisons != report.comparisons ||
        again.stackRelocations != report.stackRelocations ||
        again.openUpvalueMoves != report.openUpvalueMoves ||
        again.gcCount != report.gcCount ||
        again.allocatorMoves != report.allocatorMoves)
    {
      fprintf(stderr,
              "REPLAY INSTABILITY %s: first cmp=%d moves=%d openMoves=%d "
              "gc=%d allocMoves=%d, replay cmp=%d moves=%d openMoves=%d "
              "gc=%d allocMoves=%d\n", label,
              report.comparisons, report.stackRelocations,
              report.openUpvalueMoves, report.gcCount, report.allocatorMoves,
              again.comparisons, again.stackRelocations,
              again.openUpvalueMoves, again.gcCount, again.allocatorMoves);
      return 1;
    }
  }
  return 0;
}

#ifdef LLVM_FUZZER

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  UpRunReport totals;
  memset(&totals, 0, sizeof(totals));
  (void)runBytes(data, size, "libfuzzer", 0, &totals);
  return 0;
}

#else

#define GEN_PLAN_COUNT 300

static int printFixedSource(int index)
{
  if (index < 0 || index >= FIXED_PLAN_COUNT) return 2;
  uint8_t bytes[4096];
  size_t n = fixedBuilders[index](bytes, sizeof(bytes));
  if (n == 0) return 2;
  UpPlan plan;
  if (upvaluePlanDecode(bytes, n, &plan) != UP_DECODE_OK) return 2;
  char source[24 * 1024];
  if (upvalueRenderWren(&plan, source, sizeof(source)) < 0) return 2;
  fputs(source, stdout);
  return 0;
}

int main(int argc, char** argv)
{
  if (argc == 3 && strcmp(argv[1], "--print-source") == 0)
  {
    return printFixedSource(atoi(argv[2]));
  }

  if (argc >= 2 && strcmp(argv[1], "--") != 0)
  {
    // Reproduce one or more raw corpus files through the shared entry point.
    UpRunReport totals;
    memset(&totals, 0, sizeof(totals));
    for (int i = 1; i < argc; i++)
    {
      FILE* f = fopen(argv[i], "rb");
      if (f == NULL)
      {
        fprintf(stderr, "cannot open %s\n", argv[i]);
        return 2;
      }
      uint8_t bytes[4096];
      size_t n = fread(bytes, 1, sizeof(bytes), f);
      fclose(f);
      if (runBytes(bytes, n, argv[i], 0, &totals)) return 1;
    }
    printf("corpus replay: comparisons=%d stackMoves=%d openUpvalueMoves=%d "
           "gc=%d allocatorMoves=%d\n",
           totals.comparisons, totals.stackRelocations,
           totals.openUpvalueMoves, totals.gcCount, totals.allocatorMoves);
    return 0;
  }

  UpRunReport totals;
  memset(&totals, 0, sizeof(totals));
  int executed = 0;

  // Fixed corpus: each plan is replayed twice in a fresh VM.
  for (int i = 0; i < FIXED_PLAN_COUNT; i++)
  {
    uint8_t bytes[4096];
    size_t n = fixedBuilders[i](bytes, sizeof(bytes));
    if (n == 0)
    {
      fprintf(stderr, "fixed plan %s failed to build/decode\n", fixedNames[i]);
      return 2;
    }
    if (runBytes(bytes, n, fixedNames[i], 1, &totals)) return 1;
    executed++;
  }

  // Generated corpus: distinct encoded plans, each decoded and replayed twice.
  // Distinctness is checked on the actual wire bytes, not the seed, so two
  // seeds collapsing to one plan cannot inflate the count.
  uint8_t seen[GEN_PLAN_COUNT][4096];
  size_t seenLen[GEN_PLAN_COUNT];
  int generated = 0;
  uint32_t seed = 1;
  for (; generated < GEN_PLAN_COUNT && seed <= 100000u; seed++)
  {
    uint8_t bytes[4096];
    size_t n = generatePlan(seed, bytes, sizeof(bytes));
    if (n == 0) continue;

    int duplicate = 0;
    for (int k = 0; k < generated; k++)
    {
      if (seenLen[k] == n && memcmp(seen[k], bytes, n) == 0)
      {
        duplicate = 1;
        break;
      }
    }
    if (duplicate) continue;

    char label[40];
    snprintf(label, sizeof(label), "gen-%u", seed);
    if (runBytes(bytes, n, label, 1, &totals)) return 1;
    memcpy(seen[generated], bytes, n);
    seenLen[generated] = n;
    generated++;
    executed++;
  }

  printf("plans=%d generated=%d comparisons=%d mismatches=%d\n",
         executed, generated, totals.comparisons, totals.mismatches);
  printf("allocatorMoves=%d stackRelocations=%d openUpvalueMoves=%d "
         "explicitGc=%d gcCount=%d\n",
         totals.allocatorMoves, totals.stackRelocations,
         totals.openUpvalueMoves, totals.explicitGc, totals.gcCount);

  if (generated < GEN_PLAN_COUNT)
  {
    fprintf(stderr, "only generated %d distinct plans\n", generated);
    return 1;
  }
  if (totals.openUpvalueMoves == 0)
  {
    fprintf(stderr, "no stack relocation with open upvalues was observed\n");
    return 1;
  }
  if (totals.gcCount == 0)
  {
    fprintf(stderr, "no garbage collection was observed\n");
    return 1;
  }
  return 0;
}

#endif
