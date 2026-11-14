// upvalue_fuzz.h
//
// Structured fuzzing of open upvalues, stack relocation and cell identity in
// the Wren VM. Everything in this directory is pure C99 plus the standard
// library; the only reference model is the independent interpreter in
// upvalue_model.c. No other scripting engine is ever consulted.
//
// Three translation units share the declarations below:
//   upvalue_plan.c       wire decoding, plan validation, Wren rendering,
//                        deterministic plan generator and wire encoder
//   upvalue_model.c      independent reference model, moving allocator and
//                        the lockstep Wren execution/comparison harness
//   upvalue_fuzz_main.c  LLVMFuzzerTestOneInput entry and a standalone driver
#ifndef upvalue_fuzz_h
#define upvalue_fuzz_h

#include <stddef.h>
#include <stdint.h>

#include "wren.h"

// Bounded sub-language limits. Every limit is explicit so that a malformed
// input can never drive the decoders into unbounded recursion or allocation.
#define UP_MAX_STREAMS 16
#define UP_MAX_OPS 240
#define UP_MAX_DECLS 200
#define UP_MAX_SCOPES 12
#define UP_MAX_NESTING 4
#define UP_MAX_CAPS 8
#define UP_MAX_VALUE 2000
#define UP_MIN_VALUE (-2000)
#define UP_MAX_DEPTH 140
#define UP_GENERATED_PLANS 320

// Event tags used by both the C model and the rendered Wren program.
#define UP_TAG_READ 0
#define UP_TAG_WRITE 1
#define UP_TAG_YIELD 2
#define UP_TAG_CALL 3

// Integer locals are limited to a small range so every observed event fits
// exactly in a Wren double (well inside the 53 bit integer mantissa).
#define UP_VALUE_OFFSET 4096
#define UP_DECL_STRIDE (UP_MAX_DECLS + 1)

// Wire format opcodes. The first byte of every legal input is UP_MAGIC and the
// second is UP_VERSION. After that comes a sequence of opcode/operand groups
// terminated by UP_OP_END. All multi-byte integers are big endian.
#define UP_MAGIC 0x55u /* 'U' for upvalue */
#define UP_VERSION 1u

typedef enum {
  UP_OP_END = 0,
  UP_SCOPE_ENTER = 1,
  UP_SCOPE_EXIT = 2,
  UP_VAR = 3,        // u16 id, s16 initial
  UP_SET = 4,        // u16 id, s16 value
  UP_CLOSURE = 5,    // u16 id, u8 ncap, ncap * (u16 varId, u8 mode, s16 w)
  UP_ALIAS = 6,      // u16 newId, u16 sourceClosureId
  UP_WRAP_BEGIN = 7, // u16 id (intermediate function used for indirect capture)
  UP_WRAP_END = 8,
  UP_CALL = 9,       // u16 closureId
  UP_DROP = 10,      // u16 id, overwrite the script side reference with null
  UP_FIBER_BEGIN = 11, // u16 id
  UP_FIBER_END = 12,
  UP_YIELD = 13,     // s16 value
  UP_DEPTH = 14,     // u8 nested call depth
  UP_GC = 15,
  UP_RESUME = 16,    // u16 fiberId
  UP_SHADOW = 17,    // u16 outerId, s16, u16 innerId, s16, u16 innerClosureId
  UP_SLOTREUSE = 18, // u16 idA, s16, u16 closureA, u16 idB, s16, u16 closureB
  UP_OP_COUNT = 19
} UpOpCode;

// Closure capture modes.
#define UP_MODE_READ 1
#define UP_MODE_WRITE 2
#define UP_MODE_READ_WRITE 3

typedef enum {
  UP_DECL_VAR = 1,
  UP_DECL_CLOSURE = 2,
  UP_DECL_FIBER = 3,
  UP_DECL_WRAP = 4
} UpDeclKind;

typedef struct {
  int varId;
  int mode;
  int writeConst;
} UpCapture;

typedef struct {
  uint8_t code;
  int stream; // owning lexical stream (main, a fiber body or a wrapper body)
  int a;
  int b;
  int c;
  int d;
  int e;
  int f;
} UpOp;

// A stream is a nested lexical body: the main program, a Fiber block or an
// intermediate wrapper function.
typedef struct UpStream {
  int parent;
  int depthBase;       // lexical depth of the stream's own body
  int firstOp;
  int lastOp;          // one past the end
} UpStream;

typedef struct {
  UpDeclKind kind;
  int id;
  int depth;
  int stream;
  int initValue;       // UP_DECL_VAR
  int closureObject;   // UP_DECL_CLOSURE/UP_DECL_ALIAS: owning closure object
  int capCount;
  UpCapture caps[UP_MAX_CAPS];
  int fiberBodyStream; // UP_DECL_FIBER
} UpDecl;

typedef struct {
  UpOp ops[UP_MAX_OPS];
  int opCount;
  UpStream streams[UP_MAX_STREAMS];
  int streamCount;
  UpDecl decls[UP_MAX_DECLS];
  int declCount;
  int maxDepth;
  int hasOpenFiberCapture;
  int hasIndirectTwoLevel;
  int hasClosedAfterScope;
  int hasReadWriteCell;
} UpPlan;

typedef enum {
  UP_DECODE_OK = 0,
  UP_DECODE_TRUNCATED = 1,
  UP_DECODE_BAD_MAGIC = 2,
  UP_DECODE_UNKNOWN_OP = 3,
  UP_DECODE_LIMIT = 4,
  UP_DECODE_BAD_REF = 5,
  UP_DECODE_BAD_STRUCTURE = 6
} UpDecodeStatus;

typedef enum {
  UP_RUN_OK = 0,
  UP_RUN_REJECTED = 1, // input failed validation: not an interpreter defect
  UP_RUN_MISMATCH = 2, // model and Wren disagreed
  UP_RUN_WREN_ERROR = 3,
  UP_RUN_RESOURCE_ERROR = 4
} UpRunStatus;

// Counters shared with the check script. Only the hook-backed fields require
// WREN_FUZZ_TEST_HOOKS to be defined while compiling the VM sources.
typedef struct {
  long comparisons;       // item-wise model/Wren event comparisons
  long closureCalls;
  long fiberResumes;
  long explicitGCs;
  long hookCollections;   // every GC seen by the in-VM test hook
  long hookMovesOpen;     // stack moves while open upvalues existed
  long allocatorMoves;    // non-zero reallocs the moving allocator relocated
  long allocatorAllocs;
  long allocatorFrees;
  long plansRun;
  long plansRejected;
  long replays; // total fresh-VM executions (every plan runs at least twice)
} UpCounters;

// Numeric event codec, identical in C and in the rendered Wren program.
static inline int upEncodeEvent(int declId, int tag, int value) {
  int q = (value + UP_VALUE_OFFSET) * UP_DECL_STRIDE + declId;
  return q * 8 + tag;
}

// ---- upvalue_plan.c -------------------------------------------------------

UpDecodeStatus upDecodePlan(const uint8_t *data, size_t length, UpPlan *plan);

// Renders a validated plan into a benign Wren source program. The returned
// buffer is malloc owned by the caller. Returns NULL on allocation failure.
char *upRenderProgram(const UpPlan *plan);

// Deterministic PRNG and structured generator. Generated plans are always
// valid by construction; the bytes are produced through the same encoder the
// standalone corpus uses.
typedef struct { uint64_t state; } UpRng;
void upRngSeed(UpRng *rng, uint64_t seed);
uint32_t upRngNext(UpRng *rng);
size_t upEncodePlan(const UpPlan *plan, uint8_t *out, size_t outSize);
void upGeneratePlan(UpRng *rng, UpPlan *plan);

// Builds the fixed corpus, including the combined mega-plan. Returns the
// number of plans written; capped at capacity.
int upFixedPlans(UpPlan *out, int capacity);

// ---- upvalue_model.c ------------------------------------------------------

// Moving reallocator for WrenConfiguration.reallocateFn. Every non-zero
// reallocation lands on a different address, copies the live prefix and frees
// the old block; sizes are kept in an allocation header.
void *upMovingReallocate(void *memory, size_t newSize, void *userData);

// One fresh-VM, lockstep execution of a validated plan. A non-negative
// replayId is included in diagnostics so repeated replays are distinguishable.
UpRunStatus upExecutePlan(const UpPlan *plan, int replayId, UpCounters *counters);

// Test-only VM accessors, compiled into the VM only under WREN_FUZZ_TEST_HOOKS.
int wrenFuzzTestStackMoves(WrenVM *vm);
int wrenFuzzTestCollections(WrenVM *vm);

// ---- upvalue_fuzz_main.c --------------------------------------------------

// Describes the first model/Wren disagreement, empty string while in sync.
const char *upLastMismatch(void);

#endif
