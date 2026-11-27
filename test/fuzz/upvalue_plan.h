// upvalue_plan.h
//
// Shared declarations for the structured open-upvalue fuzzing chain.
//
// The fuzzer works in three stages:
//
//   1. A raw byte string is decoded into a finite, fully validated Plan
//      (upvaluePlanDecode()). Truncated input and out of range declaration
//      references are rejected here, never dereferenced.
//
//   2. An independent C reference model (upvalue_model.c) interprets the plan
//      using its own malloc-backed cell environment and produces a stream of
//      expected observations. It never reads Wren bytecode, upvalue
//      structures, handles or real return values.
//
//   3. A Wren renderer turns the same plan into a benign Wren program, which
//      is executed through the real public embedding chain (wrenInterpret,
//      wrenMakeCallHandle, wrenCall). Every closure read / write and every
//      Fiber hand-back is observed through the C API and compared against the
//      model stream.
//
// Only this small sub-language is handled: nested lexical scopes, integer
// locals, closures that read or modify a captured variable, closure aliases,
// closure calls and returns, and bounded Fiber.call/yield suspend/resume.
// Exceptions, transfer, imports and host resources are intentionally excluded.

#ifndef WREN_FUZZ_UPVALUE_PLAN_H
#define WREN_FUZZ_UPVALUE_PLAN_H

#include <stddef.h>
#include <stdint.h>

#define UP_MAX_DECLS 24       // distinct declarations in one plan
#define UP_MAX_CELLS 16       // distinct captured declarations (cells)
#define UP_MAX_FNS 20         // closure declarations in one plan
#define UP_MAX_NEST 5         // lexical block nesting depth
#define UP_MAX_FIBERS 3       // fibers created by one plan
#define UP_MAX_OPS 80         // operations in one plan
#define UP_MAX_DEPTH 26       // generated deep-call depth
#define UP_MAX_YIELDS 3       // yield points per fiber body
#define UP_MAX_INDIRECT 2     // extra intermediate functions for capture
#define UP_MAX_NAME 24        // rendered local name buffer length
#define UP_MAGIC 0xA5
#define UP_VERSION 0x01

// Kinds of declaration. A declaration is an identity, not a name: shadowing
// and stack-slot reuse always produce distinct declaration ids.
typedef enum {
  UP_DECL_VAR = 1,    // integer local variable
  UP_DECL_FN  = 2,    // closure whose body reads/writes one captured cell
  UP_DECL_FIBER = 3   // Fiber template with an inline, fixed-shape body
} UpDeclKind;

// What a closure body does with the single cell it captures.
typedef enum {
  UP_FN_READ = 1,     // body: return cell
  UP_FN_WRITE = 2     // body: cell = writeValue; return cell
} UpFnKind;

typedef struct {
  UpDeclKind kind;
  int id;                     // declaration index, assigned in plan order
  int blockDepth;             // lexical depth at which it is declared

  // VAR fields.
  int initial;                // initial integer value
  int nameTag;                // 0: unique rendered name v<id>; 1: literal "s",
                              //    used to exercise same-name shadowing

  // FN fields.
  UpFnKind fnKind;            // body shape
  int capturedDecl;           // declaration identity of the captured cell
  int writeValue;             // FN_WRITE: value assigned to the cell
  int indirect;               // intermediate functions between the declaring
                              // function and the leaf (0 = direct capture)

  // FIBER fields. The fiber owns a VAR cell plus reader/writer FN decls that
  // are ordinary wire records referenced by id. They are emitted inside the
  // Fiber.new body and may never appear as a top level DECL operation.
  int locDecl;                // VAR: the fiber-local cell
  int readerFn;               // FN (READ) capturing locDecl
  int writerFn;               // FN (WRITE) capturing locDecl, or -1
  int yields;                 // Fiber.yield points before the final return
} UpDecl;

// Operation codes. DECL/BLOCK ops are structural; the rest are steps whose
// observable effect the model predicts and the Wren run must reproduce.
typedef enum {
  UP_OP_SCOPE_ENTER  = 1,
  UP_OP_SCOPE_EXIT   = 2,
  UP_OP_DECL         = 3,    // a: declaration index
  UP_OP_ALIAS        = 4,    // a: target VAR decl, b: source FN decl
  UP_OP_CALL         = 5,    // a: function declaration; b: 1 = via host handle
  UP_OP_FIBER_NEW    = 6,    // a: FIBER declaration; fiber slot = ordinal
  UP_OP_FIBER_RESUME = 7,    // a: fiber slot; b: 1 = via host handle
  UP_OP_GC           = 8,    // explicit wrenCollectGarbage at a boundary
  UP_OP_DEEPEN       = 9     // bounded deep Wren call on the current fiber
} UpOpCode;

typedef struct {
  UpOpCode code;
  int a;
  int b;
} UpOp;

typedef struct {
  UpDecl decls[UP_MAX_DECLS];
  int numDecls;
  int numVars;
  int numFns;
  int numFiberDecls;
  UpOp ops[UP_MAX_OPS];
  int numOps;
  int maxDepth;               // lexical depth the plan actually reaches
  int deepDepth;              // depth used by DEEPEN and fiber bodies
  int numFibers;              // fiber slots referenced by FIBER_NEW
  int encodedTopDecls;        // explicit declaration records on the wire; the
                              // remaining decls are fiber-private and derived
  int cellDecls[UP_MAX_CELLS];
  int numCells;
} UpPlan;

// Observations the model emits and the Wren run must match. Every value is an
// integer (booleans are normalized to 0/1 by the host).
typedef enum {
  UP_OBS_CALL_RETURN = 1,    // closure call handed back an integer
  UP_OBS_FIBER_VALUE = 2,    // Fiber.call handed back a yield/final value
  UP_OBS_IDENTITY    = 3     // Object.same alias check, 1 when identical
} UpObsKind;

typedef struct {
  UpObsKind kind;
  int opIndex;        // index of the plan operation that produced it
  int decl;           // declaration id involved (fn / reader fn)
  int value;          // expected integer value
} UpObservation;

#define UP_MAX_OBS 200

typedef struct {
  UpObservation items[UP_MAX_OBS];
  int count;
} UpExpectations;

// Result of decoding. Anything other than UP_DECODE_OK means the input must be
// skipped; a rejected input is a property of the fuzzer corpus, not an
// interpreter defect, and must never be reported as one.
typedef enum {
  UP_DECODE_OK = 0,
  UP_DECODE_EMPTY,
  UP_DECODE_TRUNCATED,
  UP_DECODE_BAD_HEADER,
  UP_DECODE_BAD_OP,
  UP_DECODE_BAD_REF,
  UP_DECODE_BAD_SCOPE,
  UP_DECODE_BAD_NAME,
  UP_DECODE_LIMIT
} UpDecodeResult;

// Decode and fully validate [data]. On success writes a plan to *plan and
// returns UP_DECODE_OK. On failure returns a rejection code and leaves *plan
// untouched. Never reads past [size].
UpDecodeResult upvaluePlanDecode(const uint8_t* data, size_t size,
                                 UpPlan* plan);

// Inverse of the decoder, used by the fixed corpus and the deterministic
// generator so that every plan travels through the same decode/validate path.
// Returns encoded length, or 0 if the plan does not fit [cap].
size_t upvaluePlanEncode(const UpPlan* plan, uint8_t* out, size_t cap);

// Human readable name of a decode result, for diagnostics.
const char* upvaluePlanDecodeError(UpDecodeResult result);

// Reference model: interpret [plan] with independent, malloc-backed state and
// write the expected observation stream to [out]. Returns 1 on success, 0 if
// the plan violates a model invariant (treated like a decode rejection).
int upvalueModelRun(const UpPlan* plan, UpExpectations* out);

// Render [plan] as a benign Wren source program. Writes NUL-terminated source
// into [buffer] (capacity [cap]) and returns its length, or -1 if too small.
int upvalueRenderWren(const UpPlan* plan, char* buffer, size_t cap);

// One executed run of a plan against a real Wren VM. The counters let the
// driver prove that the interesting chain (stack relocation while open
// upvalues exist, and real collections) was actually exercised, not merely
// repeated many times.
typedef struct {
  int comparisons;        // per-observation value/identity comparisons made
  int mismatches;         // comparisons that disagreed with the model
  int allocatorMoves;     // nonzero reallocs the moving allocator relocated
  int stackRelocations;   // fiber Value-stack moves observed by the test hook
  int openUpvalueMoves;   // stack moves while at least one upvalue was open
  int explicitGc;         // host-triggered wrenCollectGarbage calls
  int gcCount;            // all collections (explicit + threshold triggered)
  int firstBadOp;         // op index of first mismatch, -1 if none
  int firstBadDecl;       // declaration id involved in the first mismatch
  int expectedValue;
  int observedValue;
  char detail[192];
} UpRunReport;

// Run [plan] in a freshly configured Wren VM. The VM is created and destroyed
// inside this call, so replay is simply calling it again with the same plan.
// Returns 1 when every model observation matched, 0 otherwise. Fills [report]
// either way. [label] is attached to diagnostics.
int upvalueRunWren(const UpPlan* plan, const UpExpectations* expected,
                   const char* label, UpRunReport* report);

// Shared entry used by both drivers: decode, model, then execute once. Returns
// 0 when the input was rejected or matched the model, 1 on a real mismatch.
int upvalueFuzzRunInput(const uint8_t* data, size_t size, const char* label,
                        UpRunReport* report);

#endif // WREN_FUZZ_UPVALUE_PLAN_H
