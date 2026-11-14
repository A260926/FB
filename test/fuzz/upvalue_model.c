// upvalue_model.c
//
// Two independent pieces share this file:
//
//   1. A moving reallocator plugged into WrenConfiguration.reallocateFn. Every
//      non-zero reallocation is placed at a fresh address, the live prefix is
//      copied and the old block is freed explicitly, so stack relocation never
//      succeeds merely because realloc() happened to keep the address.
//
//   2. The reference model: an independent interpreter for the bounded plan
//      sub-language. Variables are modelled by declaration identity, each
//      captured declaration is one mutable cell shared by every closure that
//      captures it, and a Fiber's suspended position and local environment are
//      held in ordinary C structures. It never reads Wren bytecode, upvalues or
//      return values to decide an expectation; the only thing compared is the
//      event stream the two produce.
#include "upvalue_fuzz.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Moving allocator
// ---------------------------------------------------------------------------
// Each block carries its usable size in a header immediately before the
// returned pointer. The header is over-aligned so the payload is suitably
// aligned for every Wren object.
typedef struct {
  size_t size;
} UpAllocHeader;

static void *upAllocInner(size_t size) {
  size_t total = sizeof(UpAllocHeader) + size;
  UpAllocHeader *h = (UpAllocHeader *)malloc(total);
  if (h == NULL) return NULL;
  h->size = size;
  return (void *)(h + 1);
}

void *upMovingReallocate(void *memory, size_t newSize, void *userData) {
  UpCounters *counters = (UpCounters *)userData;

  if (newSize == 0) {
    if (memory != NULL) {
      UpAllocHeader *h = ((UpAllocHeader *)memory) - 1;
      free(h);
      if (counters) counters->allocatorFrees++;
    }
    return NULL;
  }

  void *fresh = upAllocInner(newSize);
  if (fresh == NULL) return NULL; // Wren treats allocation failure as fatal

  if (memory != NULL) {
    UpAllocHeader *old = ((UpAllocHeader *)memory) - 1;
    size_t copy = old->size < newSize ? old->size : newSize;
    memcpy(fresh, memory, copy);
    free(old);
    if (counters) {
      counters->allocatorMoves++;
    }
  } else {
    if (counters) counters->allocatorAllocs++;
  }
  return fresh;
}

// ---------------------------------------------------------------------------
// Reference model
// ---------------------------------------------------------------------------
typedef struct ModelCell ModelCell;
struct ModelCell {
  int declId;
  int value;
  ModelCell *retained; // non-NULL marker once a closure retains the cell
};

typedef struct ModelEnv ModelEnv;
struct ModelEnv {
  ModelEnv *parent;
  // Parallel arrays of the variables introduced in this lexical frame.
  int ids[UP_MAX_DECLS];
  ModelCell *cells[UP_MAX_DECLS];
  int count;
};

typedef enum { MOBJ_CLOSURE, MOBJ_WRAP, MOBJ_FIBER } MObjKind;

typedef struct MObj MObj;
struct MObj {
  MObjKind kind;
  int id;
  int alive;
  // CLOSURE: resolved capture cells plus the declaration spec.
  ModelCell *capCells[UP_MAX_CAPS];
  int closureDeclIndex;
  // WRAP/FIBER: body stream and defining environment.
  int bodyStream;
  ModelEnv *definingEnv;
  // FIBER: its own base frame, parented to the defining environment and kept
  // across suspensions so open captures retain their cells between resumes.
  ModelEnv *frameEnv;
  // FIBER: suspended state.
  int pc;
  int done;
  MObj *next;
};

typedef struct {
  const UpPlan *plan;
  ModelEnv *rootEnv;
  // Every lexical frame created during interpretation is retained here for the
  // life of the Model. A frame outlived by its block can still be the defining
  // environment of a Fiber or wrapper that runs later, so frames are never
  // freed at scope exit; only modelFree releases them.
  ModelEnv *allEnvs[UP_MAX_OPS + UP_MAX_STREAMS + 8];
  int envCount;
  MObj *objects;
  long *expected;
  size_t expectedCount;
  size_t expectedCap;
  char mismatch[512];
} Model;

static void modelFail(Model *m, const char *fmt, ...) {
  if (m->mismatch[0] != '\0') return; // keep the first disagreement
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(m->mismatch, sizeof(m->mismatch), fmt, ap);
  va_end(ap);
}

static void modelEmit(Model *m, int declId, int tag, int value) {
  if (m->expectedCount == m->expectedCap) {
    size_t cap = m->expectedCap ? m->expectedCap * 2 : 128;
    long *p = (long *)realloc(m->expected, cap * sizeof(long));
    if (p == NULL) { modelFail(m, "model out of memory"); return; }
    m->expected = p;
    m->expectedCap = cap;
  }
  m->expected[m->expectedCount++] = upEncodeEvent(declId, tag, value);
}

static ModelEnv *envPush(Model *m, ModelEnv *parent) {
  ModelEnv *e = (ModelEnv *)calloc(1, sizeof(ModelEnv));
  e->parent = parent;
  if (m != NULL && m->envCount < (int)(sizeof(m->allEnvs) /
                                       sizeof(m->allEnvs[0])))
    m->allEnvs[m->envCount++] = e;
  return e;
}

// Scope exit only unwinds the active environment pointer. The frame itself is
// retained until modelFree: a Fiber or wrapper created in that block may still
// treat it as its defining environment after the block has closed.
static void envExitScope(ModelEnv *e) { (void)e; }

// Resolves a declaration id to its cell through the lexical environment chain.
static ModelCell *envResolve(ModelEnv *env, int declId) {
  for (ModelEnv *e = env; e != NULL; e = e->parent) {
    for (int i = 0; i < e->count; i++)
      if (e->ids[i] == declId) return e->cells[i];
  }
  return NULL;
}

static ModelCell *envDefine(Model *m, ModelEnv *env, int declId, int value) {
  ModelCell *cell = (ModelCell *)calloc(1, sizeof(ModelCell));
  cell->declId = declId;
  cell->value = value;
  if (env->count >= UP_MAX_DECLS) {
    modelFail(m, "env frame overflow");
    free(cell);
    return NULL;
  }
  env->ids[env->count] = declId;
  env->cells[env->count] = cell;
  env->count++;
  return cell;
}

static MObj *modelNewObj(Model *m, MObjKind kind, int id) {
  MObj *o = (MObj *)calloc(1, sizeof(MObj));
  o->kind = kind;
  o->id = id;
  o->alive = 1;
  o->next = m->objects;
  m->objects = o;
  return o;
}

static MObj *modelFind(Model *m, int id) {
  for (MObj *o = m->objects; o != NULL; o = o->next)
    if (o->id == id && o->alive) return o;
  return NULL;
}

static const UpDecl *planDecl(const UpPlan *plan, int id) {
  for (int i = 0; i < plan->declCount; i++)
    if (plan->decls[i].id == id) return &plan->decls[i];
  return NULL;
}

// Runs the fixed closure contract: read each captured cell, optionally write
// the constant and read again. This is the same sequence rendered into Wren.
static void modelCallClosure(Model *m, const UpDecl *decl, MObj *closure) {
  for (int k = 0; k < decl->capCount; k++) {
    ModelCell *cell = closure->capCells[k];
    if (cell == NULL) {
      modelFail(m, "closure %d call with unresolved cell for decl %d",
                decl->id, decl->caps[k].varId);
      return;
    }
    modelEmit(m, cell->declId, UP_TAG_READ, cell->value);
    if (decl->caps[k].mode & UP_MODE_WRITE) {
      cell->value = decl->caps[k].writeConst;
      modelEmit(m, cell->declId, UP_TAG_WRITE, cell->value);
      modelEmit(m, cell->declId, UP_TAG_READ, cell->value);
    }
  }
}

// Invokes a closure object through its concrete declaration's capture spec.
// Alias objects carry the concrete decl index they ultimately point to, which
// is what determines the read/write event contract.
static void modelInvoke(Model *m, MObj *c) {
  const UpDecl *d = &m->plan->decls[c->closureDeclIndex];
  modelCallClosure(m, d, c);
}

static MObj *makeClosureObj(Model *m, const UpDecl *decl, ModelEnv *env) {
  MObj *o = modelNewObj(m, MOBJ_CLOSURE, decl->id);
  o->closureDeclIndex = (int)(decl - m->plan->decls);
  for (int k = 0; k < decl->capCount; k++) {
    ModelCell *cell = envResolve(env, decl->caps[k].varId);
    if (cell == NULL) {
      modelFail(m, "closure %d cannot see decl %d",
                decl->id, decl->caps[k].varId);
    }
    if (cell != NULL) cell->retained = cell; // closure keeps the cell alive
    o->capCells[k] = cell;
  }
  return o;
}

// Forward declaration: fibers and wrappers invoke nested streams.
static void runFiber(Model *m, MObj *fiber);

// Invokes a wrapper function once and returns the closure named by its
// matching WRAP_END op.
static MObj *invokeWrap(Model *m, MObj *wrap) {
  const UpPlan *plan = m->plan;
  ModelEnv *env = envPush(m, wrap->definingEnv);

  MObj *result = NULL;
  int resultId = -1;
  for (int i = 0; i < plan->opCount; i++) {
    const UpOp *o = &plan->ops[i];
    if (o->code == UP_WRAP_END && o->a == wrap->id) {
      resultId = o->b;
      break;
    }
  }

  for (int i = 0; i < plan->opCount && m->mismatch[0] == '\0'; i++) {
    const UpOp *o = &plan->ops[i];
    if (o->stream != wrap->bodyStream) continue;
    switch (o->code) {
      case UP_VAR: {
        const UpDecl *d = planDecl(plan, o->a);
        envDefine(m, env, o->a, d->initValue);
        break;
      }
      case UP_SET: {
        ModelCell *cell = envResolve(env, o->a);
        if (cell == NULL) { modelFail(m, "set of unknown var %d", o->a); break; }
        cell->value = o->b;
        break;
      }
      case UP_CLOSURE: {
        const UpDecl *d = planDecl(plan, o->a);
        makeClosureObj(m, d, env);
        break;
      }
      case UP_CALL: {
        MObj *c = modelFind(m, o->a);
        if (c == NULL) { modelFail(m, "bad wrap call %d", o->a); break; }
        modelInvoke(m, c);
        break;
      }
      default:
        modelFail(m, "unsupported op %d in wrapper stream", o->code);
        break;
    }
  }

  if (resultId >= 0 && m->mismatch[0] == '\0')
    result = modelFind(m, resultId);

  // The wrapper frame stays owned by the Model; the returned closure retains
  // any cells it captured.
  return result;
}

// Copies the capture set of one closure object into a new alias object.
static MObj *aliasOf(Model *m, int newId, const MObj *src) {
  MObj *alias = modelNewObj(m, MOBJ_CLOSURE, newId);
  alias->closureDeclIndex = src->closureDeclIndex;
  for (int k = 0; k < UP_MAX_CAPS; k++)
    alias->capCells[k] = src->capCells[k];
  return alias;
}

// Resumes a fiber until its next YIELD or completion.
static void runFiber(Model *m, MObj *fiber) {
  const UpPlan *plan = m->plan;
  const UpDecl *fdecl = planDecl(plan, fiber->id);
  int stream = fdecl->fiberBodyStream;

  if (fiber->done) return;
  // The fiber gets its own base frame parented to the environment that
  // defined it, so an outer capture resolves up the parent chain while the
  // fiber's own locals live in a frame that survives each suspension.
  if (fiber->frameEnv == NULL)
    fiber->frameEnv = envPush(m, fiber->definingEnv);
  ModelEnv *env = fiber->frameEnv;

  for (; fiber->pc < plan->opCount && m->mismatch[0] == '\0'; fiber->pc++) {
    const UpOp *o = &plan->ops[fiber->pc];
    if (o->stream != stream) continue;

    switch (o->code) {
      case UP_VAR: {
        const UpDecl *d = planDecl(plan, o->a);
        envDefine(m, env, o->a, d->initValue);
        break;
      }
      case UP_SET: {
        ModelCell *cell = envResolve(env, o->a);
        if (cell == NULL) { modelFail(m, "set of unknown var %d", o->a); break; }
        cell->value = o->b;
        break;
      }
      case UP_CLOSURE: {
        const UpDecl *d = planDecl(plan, o->a);
        makeClosureObj(m, d, env);
        break;
      }
      case UP_ALIAS: {
        const UpDecl *srcDecl = planDecl(plan, o->b);
        MObj *src = modelFind(m, o->b);
        if (src == NULL) { modelFail(m, "alias of missing %d", o->b); break; }
        if (srcDecl->kind == UP_DECL_WRAP) {
          MObj *returned = invokeWrap(m, src);
          if (returned == NULL) { modelFail(m, "wrap %d returned nothing", o->b); break; }
          aliasOf(m, o->a, returned);
        } else {
          aliasOf(m, o->a, src); // second name, same cells: identity preserved
        }
        break;
      }
      case UP_CALL: {
        MObj *c = modelFind(m, o->a);
        if (c == NULL) { modelFail(m, "bad call %d", o->a); break; }
        modelInvoke(m, c);
        break;
      }
      case UP_YIELD:
        modelEmit(m, fiber->id, UP_TAG_YIELD, o->a);
        fiber->pc++; // resume after this op
        return;     // suspended: position and environment stay in the object
      case UP_DEPTH:
        break; // mirrors the rendered recursion, which emits no events
      case UP_WRAP_BEGIN: {
        const UpDecl *wd = planDecl(plan, o->a);
        MObj *wrap = modelNewObj(m, MOBJ_WRAP, o->a);
        wrap->bodyStream = wd->fiberBodyStream;
        wrap->definingEnv = env;
        break;
      }
      case UP_WRAP_END:
        break; // wrapper body is executed by invokeWrap at the alias op
      case UP_SCOPE_ENTER:
        env = envPush(m, env);
        break;
      case UP_SCOPE_EXIT: {
        ModelEnv *popped = env;
        env = env->parent;
        envExitScope(popped);
        break;
      }
      default:
        modelFail(m, "unsupported op %d in fiber stream", o->code);
        break;
    }
  }
  // Balanced scopes mean the current frame is the fiber's base frame again.
  // The frame remains Model-owned (a closure may still reference its cells);
  // modelFree releases it.
  if (env != fiber->frameEnv)
    modelFail(m, "fiber %d ended with unbalanced scopes", fiber->id);
  fiber->done = 1;
}

// Runs the main stream to completion, producing the expected event list.
static void modelRun(Model *m) {
  const UpPlan *plan = m->plan;
  ModelEnv *env = envPush(m, NULL);
  m->rootEnv = env;

  for (int i = 0; i < plan->opCount && m->mismatch[0] == '\0'; i++) {
    const UpOp *o = &plan->ops[i];
    if (o->stream != 0) continue;

    switch (o->code) {
      case UP_SCOPE_ENTER:
        env = envPush(m, env);
        break;
      case UP_SCOPE_EXIT: {
        ModelEnv *popped = env;
        env = env->parent;
        envExitScope(popped);
        break;
      }
      case UP_VAR: {
        const UpDecl *d = planDecl(plan, o->a);
        envDefine(m, env, o->a, d->initValue);
        break;
      }
      case UP_SET: {
        ModelCell *cell = envResolve(env, o->a);
        if (cell == NULL) { modelFail(m, "set of unknown var %d", o->a); break; }
        cell->value = o->b;
        break;
      }
      case UP_CLOSURE: {
        const UpDecl *d = planDecl(plan, o->a);
        makeClosureObj(m, d, env);
        break;
      }
      case UP_ALIAS: {
        const UpDecl *srcDecl = planDecl(plan, o->b);
        MObj *src = modelFind(m, o->b);
        if (src == NULL) { modelFail(m, "alias of missing %d", o->b); break; }
        if (srcDecl->kind == UP_DECL_WRAP) {
          MObj *returned = invokeWrap(m, src);
          if (returned == NULL) { modelFail(m, "wrap %d returned nothing", o->b); break; }
          aliasOf(m, o->a, returned);
        } else {
          aliasOf(m, o->a, src);
        }
        break;
      }
      case UP_WRAP_BEGIN: {
        const UpDecl *d = planDecl(plan, o->a);
        MObj *wrap = modelNewObj(m, MOBJ_WRAP, o->a);
        wrap->bodyStream = d->fiberBodyStream;
        wrap->definingEnv = env;
        break;
      }
      case UP_FIBER_BEGIN: {
        const UpDecl *d = planDecl(plan, o->a);
        MObj *fiber = modelNewObj(m, MOBJ_FIBER, o->a);
        fiber->bodyStream = d->fiberBodyStream;
        fiber->definingEnv = env; // lexical parent; a call frame is pushed on run
        fiber->pc = 0;
        break;
      }
      case UP_RESUME: {
        MObj *fiber = modelFind(m, o->a);
        if (fiber == NULL) { modelFail(m, "resume of missing fiber %d", o->a); break; }
        runFiber(m, fiber);
        break;
      }
      case UP_CALL: {
        MObj *c = modelFind(m, o->a);
        if (c == NULL) { modelFail(m, "bad call %d", o->a); break; }
        modelInvoke(m, c);
        break;
      }
      case UP_DROP:
        // Script references disappear; host roots keep the object alive.
        break;
      case UP_GC:
      case UP_DEPTH:
      case UP_FIBER_END:
      case UP_WRAP_END:
        break;
      case UP_SHADOW: {
        // Mirror the rendered structure exactly.
        envDefine(m, env, o->a, o->b);
        ModelEnv *inner = envPush(m, env);
        envDefine(m, inner, o->c, o->d);
        const UpDecl *d = planDecl(plan, o->e);
        MObj *clo = makeClosureObj(m, d, inner);
        modelInvoke(m, clo);
        env = inner->parent;
        envExitScope(inner);
        break;
      }
      case UP_SLOTREUSE: {
        ModelEnv *s1 = envPush(m, env);
        envDefine(m, s1, o->a, o->b);
        const UpDecl *da = planDecl(plan, o->c);
        MObj *ca = makeClosureObj(m, da, s1);
        env = s1->parent;
        envExitScope(s1);

        ModelEnv *s2 = envPush(m, env);
        envDefine(m, s2, o->d, o->e);
        const UpDecl *db = planDecl(plan, o->f);
        MObj *cb = makeClosureObj(m, db, s2);
        env = s2->parent;
        envExitScope(s2);

        modelInvoke(m, ca);
        modelInvoke(m, cb);
        break;
      }
      default:
        modelFail(m, "unsupported main op %d", o->code);
        break;
    }
  }
}

static int rememberCell(ModelCell **unique, int count, ModelCell *cell) {
  if (cell == NULL) return count;
  for (int i = 0; i < count; i++)
    if (unique[i] == cell) return count;
  unique[count] = cell;
  return count + 1;
}

static void modelFree(Model *m) {
  // A cell may be referenced from several environment frames and several
  // closures (one captured declaration is a single shared cell). Collect every
  // distinct cell pointer and free it exactly once.
  ModelCell *unique[UP_MAX_DECLS * 8];
  int uniqueCount = 0;

  for (MObj *o = m->objects; o != NULL; o = o->next)
    for (int k = 0; k < UP_MAX_CAPS; k++)
      uniqueCount = rememberCell(unique, uniqueCount, o->capCells[k]);

  // All lexical frames (including those outlived by a block, fiber or wrapper)
  // are reachable through m->allEnvs.
  for (int i = 0; i < m->envCount; i++)
    for (int j = 0; j < m->allEnvs[i]->count; j++)
      uniqueCount =
          rememberCell(unique, uniqueCount, m->allEnvs[i]->cells[j]);

  for (int i = 0; i < uniqueCount; i++) free(unique[i]);

  for (int i = 0; i < m->envCount; i++) free(m->allEnvs[i]);

  MObj *o = m->objects;
  while (o != NULL) { MObj *next = o->next; free(o); o = next; }
  free(m->expected);
}

// ---------------------------------------------------------------------------
// Lockstep Wren execution and item-wise comparison
// ---------------------------------------------------------------------------
#define UP_MODULE "upfuzz"
#define REQ_GC 200000
#define REQ_HANDLE_BASE 300000
#define REQ_DROP_BASE 500000
#define REQ_STEP 700000

// Acquires a WrenHandle for every module holder h<id> that has become non-null
// since the last boundary. Polling the holders (rather than receiving them as
// yield values) means the host can root closures and fibers created inside a
// suspended child fiber, whose yields never reach the C host directly.
static void rootNewHandles(WrenVM *vm, const UpPlan *plan,
                           WrenHandle **retained) {
  char name[24];
  for (int i = 0; i < plan->declCount; i++) {
    const UpDecl *d = &plan->decls[i];
    if (d->kind != UP_DECL_CLOSURE && d->kind != UP_DECL_FIBER &&
        d->kind != UP_DECL_WRAP)
      continue;
    if (retained[d->id] != NULL) continue;
    snprintf(name, sizeof(name), "h%d", d->id);
    wrenEnsureSlots(vm, 3);
    wrenGetVariable(vm, UP_MODULE, name, 1);
    if (wrenGetSlotType(vm, 1) != WREN_TYPE_NULL)
      retained[d->id] = wrenGetSlotHandle(vm, 1);
  }
}

static char gMismatch[512];

const char *upLastMismatch(void) { return gMismatch; }

static void recordWrenError(WrenVM *vm, WrenErrorType type, const char *module,
                            int line, const char *message) {
  (void)vm; (void)module; (void)type;
  if (gMismatch[0] == '\0')
    snprintf(gMismatch, sizeof(gMismatch),
             "Wren error at line %d: %s", line, message ? message : "?");
}

static void decodeEvent(long code, int *declId, int *tag, int *value) {
  *tag = (int)(code % 8);
  long q = code / 8;
  *declId = (int)(q % UP_DECL_STRIDE);
  *value = (int)(q / UP_DECL_STRIDE) - UP_VALUE_OFFSET;
}

static const char *tagName(int tag) {
  switch (tag) {
    case UP_TAG_READ: return "read";
    case UP_TAG_WRITE: return "write";
    case UP_TAG_YIELD: return "yield";
    case UP_TAG_CALL: return "call";
    default: return "?";
  }
}

// Compares the current Wren Log prefix against the model expectation and
// records the first differing declaration, operation and value.
static int compareLogPrefix(WrenVM *vm, Model *m, UpCounters *counters,
                            long boundary) {
  wrenEnsureSlots(vm, 3);
  wrenGetVariable(vm, UP_MODULE, "Log", 0);
  if (wrenGetSlotType(vm, 0) != WREN_TYPE_LIST) {
    modelFail(m, "Log is not a list");
    return 0;
  }
  int count = wrenGetListCount(vm, 0);
  if ((size_t)count > m->expectedCount) {
    int declId, tag, value;
    wrenGetListElement(vm, 0, (int)m->expectedCount, 1);
    long got = (long)wrenGetSlotDouble(vm, 1);
    decodeEvent(got, &declId, &tag, &value);
    snprintf(gMismatch, sizeof(gMismatch),
             "extra Wren event at boundary %ld: decl=%d op=%s value=%d",
             boundary, declId, tagName(tag), value);
    return 0;
  }
  for (int i = 0; i < count; i++) {
    wrenGetListElement(vm, 0, i, 1);
    long got = (long)wrenGetSlotDouble(vm, 1);
    long want = m->expected[i];
    counters->comparisons++;
    if (got != want) {
      int gd, gt, gv, wd, wt, wv;
      decodeEvent(got, &gd, &gt, &gv);
      decodeEvent(want, &wd, &wt, &wv);
      snprintf(gMismatch, sizeof(gMismatch),
               "event %d differs: Wren decl=%d op=%s value=%d vs model "
               "decl=%d op=%s value=%d",
               i, gd, tagName(gt), gv, wd, tagName(wt), wv);
      return 0;
    }
  }
  return 1;
}

UpRunStatus upExecutePlan(const UpPlan *plan, int replayId, UpCounters *counters) {
  gMismatch[0] = '\0';

  // Build the independent expectation first.
  Model model;
  memset(&model, 0, sizeof(model));
  model.plan = plan;
  modelRun(&model);
  if (model.mismatch[0] != '\0') {
    snprintf(gMismatch, sizeof(gMismatch), "model: %s", model.mismatch);
    modelFree(&model);
    return UP_RUN_RESOURCE_ERROR;
  }

  char *source = upRenderProgram(plan);
  if (source == NULL) {
    modelFree(&model);
    return UP_RUN_RESOURCE_ERROR;
  }

  WrenConfiguration config;
  wrenInitConfiguration(&config);
  config.reallocateFn = upMovingReallocate;
  config.userData = counters;
  config.errorFn = recordWrenError;
  // Small heap so the explicit boundary GCs actually reclaim and re-grow.
  config.initialHeapSize = 4096;
  config.minHeapSize = 2048;
  config.heapGrowthPercent = 50;

  WrenVM *vm = wrenNewVM(&config);
  if (vm == NULL) {
    free(source);
    modelFree(&model);
    return UP_RUN_RESOURCE_ERROR;
  }

  UpRunStatus status = UP_RUN_OK;
  WrenHandle *retained[UP_MAX_DECLS];
  memset(retained, 0, sizeof(retained));
  WrenHandle *mainHandle = NULL;
  WrenHandle *callHandle = NULL;
  WrenHandle *doneHandle = NULL;

  WrenInterpretResult ir = wrenInterpret(vm, UP_MODULE, source);
  if (ir != WREN_RESULT_SUCCESS) {
    status = UP_RUN_WREN_ERROR;
    goto cleanup;
  }

  wrenEnsureSlots(vm, 3);
  wrenGetVariable(vm, UP_MODULE, "Main", 0);
  mainHandle = wrenGetSlotHandle(vm, 0);
  callHandle = wrenMakeCallHandle(vm, "call()");
  doneHandle = wrenMakeCallHandle(vm, "isDone");

  long boundary = 0;
  for (;;) {
    // Run Main until it yields a request code or finishes.
    wrenEnsureSlots(vm, 3);
    wrenSetSlotHandle(vm, 0, mainHandle);
    ir = wrenCall(vm, callHandle);
    if (ir != WREN_RESULT_SUCCESS) {
      status = UP_RUN_WREN_ERROR;
      goto cleanup;
    }
    boundary++;
    counters->fiberResumes++;

    // One wrenCall runs Main until its next yield or completion; the value in
    // slot 0 is the yield argument while suspended and the trailing expression
    // (0) when finished.
    int slotType = wrenGetSlotType(vm, 0);
    double request = slotType == WREN_TYPE_NUM ? wrenGetSlotDouble(vm, 0) : -1.0;

    // Check completion without resuming Main.
    wrenSetSlotHandle(vm, 0, mainHandle);
    if (wrenCall(vm, doneHandle) != WREN_RESULT_SUCCESS) {
      status = UP_RUN_WREN_ERROR;
      goto cleanup;
    }
    int done = wrenGetSlotBool(vm, 0);

    if (done) {
      if (slotType != WREN_TYPE_NUM || request != 0.0) {
        snprintf(gMismatch, sizeof(gMismatch),
                 "finished Main did not return 0 (boundary %ld, type %d)",
                 boundary, slotType);
        status = UP_RUN_MISMATCH;
        goto cleanup;
      }
      break;
    }

    if (slotType != WREN_TYPE_NUM) {
      snprintf(gMismatch, sizeof(gMismatch),
               "boundary %ld returned slot type %d, expected a number",
               boundary, slotType);
      status = UP_RUN_MISMATCH;
      goto cleanup;
    }

    // Compare every event produced so far, item by item, at this boundary.
    if (!compareLogPrefix(vm, &model, counters, boundary)) {
      status = UP_RUN_MISMATCH;
      goto cleanup;
    }

    long req = (long)request;

    // Root every object holder that now exists, in whatever stream it was
    // created, before running another step or collecting garbage.
    rootNewHandles(vm, plan, retained);

    if (req == REQ_GC) {
      wrenCollectGarbage(vm);
      counters->explicitGCs++;
    } else if (req == REQ_STEP) {
      // A resume boundary: no host action beyond rooting and observing.
    } else if (req >= REQ_HANDLE_BASE && req < REQ_HANDLE_BASE + UP_MAX_DECLS) {
      // An explicit creation boundary; rootNewHandles already took the handle.
      int id = (int)(req - REQ_HANDLE_BASE);
      if (retained[id] == NULL) {
        snprintf(gMismatch, sizeof(gMismatch),
                 "creation boundary for decl %d produced no host root", id);
        status = UP_RUN_MISMATCH;
        goto cleanup;
      }
    } else if (req >= REQ_DROP_BASE && req < REQ_DROP_BASE + UP_MAX_DECLS) {
      int id = (int)(req - REQ_DROP_BASE);
      // The module holder is now null, but the host keeps its own root and may
      // still use it. Prove the retained root is a live object.
      if (retained[id] == NULL) {
        snprintf(gMismatch, sizeof(gMismatch),
                 "drop of decl %d without a retained host root", id);
        status = UP_RUN_MISMATCH;
        goto cleanup;
      }
      wrenEnsureSlots(vm, 3);
      wrenSetSlotHandle(vm, 1, retained[id]);
      if (wrenGetSlotType(vm, 1) == WREN_TYPE_NULL) {
        snprintf(gMismatch, sizeof(gMismatch),
                 "retained host root for decl %d was collected", id);
        status = UP_RUN_MISMATCH;
        goto cleanup;
      }
    } else {
      snprintf(gMismatch, sizeof(gMismatch),
               "unknown driver request %ld at boundary %ld", req, boundary);
      status = UP_RUN_MISMATCH;
      goto cleanup;
    }

    // Refresh the read-only observability counters after every boundary.
#ifdef WREN_FUZZ_TEST_HOOKS
    counters->hookCollections = wrenFuzzTestCollections(vm);
    counters->hookMovesOpen = wrenFuzzTestStackMoves(vm);
#endif
  }

  // Final, full item-wise comparison: not just the count, but every remaining
  // event's declaration, operation and value. This also covers events emitted
  // after the last synchronization boundary.
  if (!compareLogPrefix(vm, &model, counters, boundary))
    status = UP_RUN_MISMATCH;
  else {
    wrenEnsureSlots(vm, 3);
    wrenGetVariable(vm, UP_MODULE, "Log", 0);
    int finalCount = wrenGetListCount(vm, 0);
    if ((size_t)finalCount != model.expectedCount) {
      snprintf(gMismatch, sizeof(gMismatch),
               "event count differs: Wren=%d model=%zu",
               finalCount, model.expectedCount);
      status = UP_RUN_MISMATCH;
    }
  }

cleanup:
  for (int i = 0; i < UP_MAX_DECLS; i++)
    if (retained[i] != NULL) wrenReleaseHandle(vm, retained[i]);
  if (mainHandle != NULL) wrenReleaseHandle(vm, mainHandle);
  if (callHandle != NULL) wrenReleaseHandle(vm, callHandle);
  if (doneHandle != NULL) wrenReleaseHandle(vm, doneHandle);
  wrenFreeVM(vm);
  free(source);
  modelFree(&model);
  (void)replayId;
  return status;
}
