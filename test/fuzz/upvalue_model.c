// upvalue_model.c
//
// Independent C reference model and the benign-Wren renderer.
//
// The model interprets a validated Plan using its own malloc-backed state:
// every captured declaration is one mutable Cell shared by every closure that
// names the same declaration identity. Declaration identity, not spelling,
// drives the model, so same-name shadowing and stack-slot reuse always produce
// distinct cells. The model never inspects Wren internals, bytecode, upvalue
// structures, handles or real return values; its expected stream is derived
// solely from the plan.
//
// The renderer emits a Wren program whose observable integer results are
// appended, in plan operation order, to a module-level List named "log". The
// driver diffs that list against the model expectation stream.

#include "upvalue_plan.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Reference model
// ---------------------------------------------------------------------------

typedef struct Cell Cell;
struct Cell
{
  int decl;           // declaration identity of the owning VAR
  int value;          // current mutable value
  int open;           // the declaring frame is still on the lexical stack
};

typedef struct
{
  int fnDecl;         // FN declaration identity
  int cellDecl;       // captured declaration identity
  Cell* cell;         // shared cell
  UpFnKind kind;
  int writeValue;
} MClosure;

typedef enum { FB_NEW = 0, FB_SUSPENDED = 1, FB_DONE = 2 } FiberState;

typedef struct
{
  FiberState state;
  int fiberDecl;      // FIBER declaration identity
  int step;           // resumes consumed so far
  int locDecl;
  int readerDecl;
  int writerDecl;     // -1 if none
  int writeValue;
  int yields;
  Cell* locCell;
  MClosure reader;
  MClosure writer;
  int hasWriter;
} MFiber;

typedef struct
{
  const UpPlan* plan;
  Cell* cells[UP_MAX_DECLS];       // indexed by VAR declaration id
  MClosure closures[UP_MAX_DECLS]; // indexed by FN declaration id
  int hasClosure[UP_MAX_DECLS];
  MClosure* alias[UP_MAX_DECLS];   // VAR decls currently holding a closure
  int hasAlias[UP_MAX_DECLS];
  MFiber fibers[UP_MAX_FIBERS];
  UpExpectations out;
  int ok;
} Model;

static void modelFail(Model* m, const char* why)
{
  (void)why;
  m->ok = 0;
}

static Cell* modelGetCell(Model* m, int decl)
{
  Cell* c = m->cells[decl];
  if (c == NULL)
  {
    c = (Cell*)malloc(sizeof(Cell));
    if (c == NULL) { modelFail(m, "oom"); return NULL; }
    c->decl = decl;
    c->value = m->plan->decls[decl].initial;
    c->open = 1;
    m->cells[decl] = c;
  }
  return c;
}

static void modelEmit(Model* m, UpObsKind kind, int opIndex, int decl,
                      int value)
{
  if (m->out.count >= UP_MAX_OBS) { modelFail(m, "obs overflow"); return; }
  UpObservation* o = &m->out.items[m->out.count++];
  o->kind = kind;
  o->opIndex = opIndex;
  o->decl = decl;
  o->value = value;
}

// Materialize the closure object for FN declaration [fnDecl]. Distinct FN
// declarations are distinct closure objects, even when they capture the same
// cell with the same body shape.
static MClosure* modelMaterializeClosure(Model* m, int fnDecl)
{
  if (m->hasClosure[fnDecl]) return &m->closures[fnDecl];

  const UpDecl* d = &m->plan->decls[fnDecl];
  MClosure* c = &m->closures[fnDecl];
  c->fnDecl = fnDecl;
  c->cellDecl = d->capturedDecl;
  c->cell = modelGetCell(m, d->capturedDecl);
  c->kind = d->fnKind;
  c->writeValue = d->writeValue;
  m->hasClosure[fnDecl] = 1;
  return c;
}

static int modelCallClosure(MClosure* c)
{
  if (c->kind == UP_FN_WRITE) c->cell->value = c->writeValue;
  return c->cell->value;
}

// Mirror of the rendered Fiber body (see upvalueRenderWren):
//
//   var loc = init
//   var reader = Fn.new { loc }
//   [var writer = Fn.new { loc = writeValue; loc }]
//   Fiber.yield(reader.call())                       // resume 0
//   if yields >= 2: Deep.run(d); Fiber.yield(reader())   // resume 1
//   if yields == 3: [writer();] Fiber.yield(reader())    // resume 2
//   [writer()]
//   return reader.call()                             // final resume
//
// Deep calls change no model value, so the DEEPEN step has no cell effect
// here; its stack-growth effect is exercised and counted on the Wren side.
static int modelResumeFiber(Model* m, MFiber* f, int opIndex)
{
  if (f->state == FB_DONE)
  {
    modelFail(m, "resume of finished fiber");
    return 0;
  }

  if (f->state == FB_NEW)
  {
    // The body starts: bind reader/writer to the one shared loc cell.
    f->locCell = modelGetCell(m, f->locDecl);
    f->reader.fnDecl = f->readerDecl;
    f->reader.cellDecl = f->locDecl;
    f->reader.cell = f->locCell;
    f->reader.kind = UP_FN_READ;
    if (f->hasWriter)
    {
      f->writer.fnDecl = f->writerDecl;
      f->writer.cellDecl = f->locDecl;
      f->writer.cell = f->locCell;
      f->writer.kind = UP_FN_WRITE;
      f->writer.writeValue = f->writeValue;
    }
    f->state = FB_SUSPENDED;
  }

  int step = f->step;
  if (step > f->yields) { modelFail(m, "fiber step out of range"); return 0; }

  int value;
  if (step == 2 && f->hasWriter)
  {
    // The third yield point runs the writer before handing control back.
    f->locCell->value = f->writeValue;
  }

  if (step == f->yields)
  {
    // Final resume: run the trailing writer (if any), then return.
    if (f->hasWriter) f->locCell->value = f->writeValue;
    value = f->locCell->value;
    f->state = FB_DONE;
  }
  else
  {
    // A yield point: hand back the current cell value.
    value = f->locCell->value;
  }

  f->step++;
  modelEmit(m, UP_OBS_FIBER_VALUE, opIndex, f->readerDecl, value);
  return value;
}

int upvalueModelRun(const UpPlan* plan, UpExpectations* out)
{
  Model m;
  memset(&m, 0, sizeof(m));
  m.plan = plan;
  m.ok = 1;

  int live[UP_MAX_DECLS];
  memset(live, 0, sizeof(live));
  int depth = 0;
  int fiberCount = 0;

  for (int opi = 0; opi < plan->numOps && m.ok; opi++)
  {
    const UpOp* op = &plan->ops[opi];
    switch (op->code)
    {
      case UP_OP_SCOPE_ENTER:
        depth++;
        break;

      case UP_OP_SCOPE_EXIT:
        depth--;
        // Declarations whose block has ended go out of scope. A captured VAR
        // transitions from an open to a closed binding: its cell survives as
        // long as a closure names it, but the local identity is dead.
        for (int k = 0; k < plan->numDecls; k++)
        {
          if (live[k] && plan->decls[k].blockDepth > depth)
          {
            live[k] = 0;
            if (plan->decls[k].kind == UP_DECL_VAR && m.cells[k] != NULL)
              m.cells[k]->open = 0;
            m.hasAlias[k] = 0;
          }
        }
        break;

      case UP_OP_DECL:
      {
        int id = op->a;
        const UpDecl* d = &plan->decls[id];
        if (d->blockDepth != depth) { modelFail(&m, "decl scope"); break; }
        live[id] = 1;
        if (d->kind == UP_DECL_VAR)
        {
          // Bind the local. The cell is created lazily on first capture so
          // plain uncaptured locals cost nothing.
          if (m.cells[id] == NULL)
          {
            Cell* c = (Cell*)malloc(sizeof(Cell));
            if (c == NULL) { modelFail(&m, "oom"); break; }
            c->decl = id;
            c->value = d->initial;
            c->open = 1;
            m.cells[id] = c;
          }
        }
        else if (d->kind == UP_DECL_FN)
        {
          if (!live[d->capturedDecl]) { modelFail(&m, "capture of dead var"); break; }
          modelMaterializeClosure(&m, id);
        }
        else
        {
          modelFail(&m, "bad decl op");
        }
        break;
      }

      case UP_OP_ALIAS:
      {
        int target = op->a;
        int source = op->b;
        if (!m.hasClosure[source]) { modelFail(&m, "alias of missing fn"); break; }
        m.alias[target] = &m.closures[source];
        m.hasAlias[target] = 1;
        // The renderer probes Object.same(target, source), which must hold.
        modelEmit(&m, UP_OBS_IDENTITY, opi, source, 1);
        break;
      }

      case UP_OP_CALL:
      {
        int id = op->a;
        // Calls may target an FN directly or a VAR holding an alias of one.
        MClosure* target = NULL;
        int sourceDecl = id;
        if (m.hasClosure[id])
        {
          target = &m.closures[id];
        }
        else if (m.hasAlias[id])
        {
          target = m.alias[id];
          sourceDecl = target->fnDecl;
        }
        if (target == NULL) { modelFail(&m, "call of missing fn"); break; }
        int value = modelCallClosure(target);
        modelEmit(&m, UP_OBS_CALL_RETURN, opi, sourceDecl, value);
        break;
      }

      case UP_OP_FIBER_NEW:
      {
        int slot = op->b;
        const UpDecl* d = &plan->decls[op->a];
        MFiber* f = &m.fibers[slot];
        memset(f, 0, sizeof(*f));
        f->state = FB_NEW;
        f->fiberDecl = op->a;
        f->step = 0;
        f->locDecl = d->locDecl;
        f->readerDecl = d->readerFn;
        f->writerDecl = d->writerFn;
        f->hasWriter = d->writerFn >= 0;
        f->writeValue = d->writeValue;
        f->yields = d->yields;
        live[op->a] = 1;
        fiberCount++;
        (void)fiberCount;
        break;
      }

      case UP_OP_FIBER_RESUME:
      {
        int slot = op->a;
        modelResumeFiber(&m, &m.fibers[slot], opi);
        break;
      }

      case UP_OP_GC:
      case UP_OP_DEEPEN:
        // Pure host/stack effects; no model-visible value change.
        break;

      default:
        modelFail(&m, "bad op");
    }
  }

  int result = m.ok;
  if (result) *out = m.out;

  // The model owns all of its own allocations; nothing is handed to Wren.
  for (int k = 0; k < UP_MAX_DECLS; k++) free(m.cells[k]);
  return result;
}

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------

typedef struct
{
  char* buf;
  size_t cap;
  size_t len;
  int ok;
} Sink;

static void sinkPut(Sink* s, const char* text)
{
  size_t n = strlen(text);
  if (s->len + n + 1 > s->cap) { s->ok = 0; return; }
  memcpy(s->buf + s->len, text, n);
  s->len += n;
  s->buf[s->len] = '\0';
}

static void sinkFmt(Sink* s, const char* fmt, ...)
{
  char tmp[160];
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(tmp, sizeof(tmp), fmt, args);
  va_end(args);
  if (n < 0) { s->ok = 0; return; }
  if (s->len + (size_t)n + 1 > s->cap) { s->ok = 0; return; }
  memcpy(s->buf + s->len, tmp, (size_t)n);
  s->len += n;
  s->buf[s->len] = '\0';
}

// Local spelling for a VAR declaration. Shadow-tagged declarations share the
// literal name "s"; the decoder guarantees at most one such live local at a
// time, so this is always a valid Wren program.
static void localName(const UpPlan* plan, int id, char* out, size_t cap)
{
  const UpDecl* d = &plan->decls[id];
  if (d->kind == UP_DECL_VAR && d->nameTag == 1)
    snprintf(out, cap, "s");
  else
    snprintf(out, cap, "v%d", id);
}

// Render a closure expression for FN declaration [id]. With [indirect]
// intermediate functions, the leaf closure is wrapped in nested Fn.new wrappers
// whose upvalues are auto-flattened by the Wren compiler. The expression value
// is the outermost closure: calling it runs the whole chain, so an FN local
// always holds a callable and never a precomputed result.
//
// A write leaf is a single assignment statement; in Wren an assignment block
// evaluates to the assigned value, which matches the model's "return the cell
// after writing" semantics, so the whole closure stays on one line and avoids
// any block-indentation pitfalls.
static void renderClosureExpr(Sink* s, const UpPlan* plan, int id)
{
  const UpDecl* d = &plan->decls[id];
  char cellName[UP_MAX_NAME];
  localName(plan, d->capturedDecl, cellName, sizeof(cellName));

  char expr[400];
  if (d->fnKind == UP_FN_READ)
    snprintf(expr, sizeof(expr), "Fn.new { %s }", cellName);
  else
    snprintf(expr, sizeof(expr), "Fn.new { %s = %d }",
             cellName, d->writeValue);

  // Each intermediate is its own function that only forwards via .call(); the
  // compiler threads the captured cell through every layer as an upvalue.
  for (int k = 0; k < d->indirect; k++)
  {
    char wrapped[512];
    snprintf(wrapped, sizeof(wrapped), "Fn.new { %s.call() }", expr);
    snprintf(expr, sizeof(expr), "%s", wrapped);
  }
  sinkPut(s, expr);
}

// Render the inline body of a Fiber template. The body yields exactly
// decl.yields times and then returns a final reader result.
static void renderFiberBody(Sink* s, const UpPlan* plan, int fiberId,
                            int indent)
{
  const UpDecl* d = &plan->decls[fiberId];
  char loc[UP_MAX_NAME], reader[UP_MAX_NAME], writer[UP_MAX_NAME];
  localName(plan, d->locDecl, loc, sizeof(loc));
  localName(plan, d->readerFn, reader, sizeof(reader));
  if (d->writerFn >= 0)
    localName(plan, d->writerFn, writer, sizeof(writer));

  sinkFmt(s, "%*svar %s = %d\n", indent, "", loc, plan->decls[d->locDecl].initial);
  sinkFmt(s, "%*svar %s = Fn.new { %s }\n", indent, "", reader, loc);
  if (d->writerFn >= 0)
  {
    // Assignment block evaluates to the assigned value; the model treats a
    // write closure as "assign, then return the cell", which is identical.
    sinkFmt(s, "%*svar %s = Fn.new { %s = %d }\n", indent, "", writer,
            loc, d->writeValue);
  }

  // Resume 0: hand back the first reader value.
  sinkFmt(s, "%*sFiber.yield(%s.call())\n", indent, "", reader);

  if (d->yields >= 2)
  {
    // Grow this fiber's own Value stack while the loc cell is still open.
    sinkFmt(s, "%*sDeep.run(%d)\n", indent, "", plan->deepDepth);
    sinkFmt(s, "%*sFiber.yield(%s.call())\n", indent, "", reader);
  }
  if (d->yields >= 3)
  {
    if (d->writerFn >= 0)
      sinkFmt(s, "%*s%s.call()\n", indent, "", writer);
    sinkFmt(s, "%*sFiber.yield(%s.call())\n", indent, "", reader);
  }
  if (d->writerFn >= 0)
    sinkFmt(s, "%*s%s.call()\n", indent, "", writer);
  sinkFmt(s, "%*sreturn %s.call()\n", indent, "", reader);
}

int upvalueRenderWren(const UpPlan* plan, char* buffer, size_t cap)
{
  Sink s;
  s.buf = buffer;
  s.cap = cap;
  s.len = 0;
  s.ok = 1;
  buffer[0] = '\0';

  // Module-level machinery. "log" is the single observation channel shared by
  // the driver fiber and every fiber it calls. "parked*" hand objects to the
  // host at wrenCall boundaries.
  sinkPut(&s, "var log = List.new()\n");
  sinkPut(&s, "var parkedFn = null\n");
  sinkPut(&s, "var parkedFiber = null\n");
  sinkFmt(&s, "class Deep {\n  static run(n) {\n");
  sinkFmt(&s, "    if (n == 0) return 0\n");
  sinkFmt(&s, "    return Deep.run(n - 1)\n  }\n}\n");
  sinkPut(&s, "var Driver = Fiber.new {\n");

  int depth = 0;
  int fiberSlot = 0;
  int fiberSlotOfDecl[UP_MAX_DECLS];
  for (int i = 0; i < UP_MAX_DECLS; i++) fiberSlotOfDecl[i] = -1;

  for (int opi = 0; opi < plan->numOps; opi++)
  {
    const UpOp* op = &plan->ops[opi];
    int ind = 2 + depth * 2;
    switch (op->code)
    {
      case UP_OP_SCOPE_ENTER:
        sinkFmt(&s, "%*s{\n", ind, "");
        depth++;
        break;

      case UP_OP_SCOPE_EXIT:
        depth--;
        sinkFmt(&s, "%*s}\n", ind - 2, "");
        break;

      case UP_OP_DECL:
      {
        int id = op->a;
        const UpDecl* d = &plan->decls[id];
        char name[UP_MAX_NAME];
        localName(plan, id, name, sizeof(name));
        if (d->kind == UP_DECL_VAR)
        {
          sinkFmt(&s, "%*svar %s = %d\n", ind, "", name, d->initial);
        }
        else
        {
          sinkFmt(&s, "%*svar %s = ", ind, "", name);
          renderClosureExpr(&s, plan, id);
          sinkPut(&s, "\n");
        }
        break;
      }

      case UP_OP_ALIAS:
      {
        char target[UP_MAX_NAME], source[UP_MAX_NAME];
        localName(plan, op->a, target, sizeof(target));
        localName(plan, op->b, source, sizeof(source));
        sinkFmt(&s, "%*s%s = %s\n", ind, "", target, source);
        sinkFmt(&s, "%*slog.add(Object.same(%s, %s))\n", ind, "",
                target, source);
        break;
      }

      case UP_OP_CALL:
      {
        char name[UP_MAX_NAME];
        localName(plan, op->a, name, sizeof(name));
        if (op->b)
        {
          // Host boundary. The driver parks the closure in a module variable,
          // the first yield hands control to the host, the host invokes the
          // parked closure through a WrenHandle and appends the result to log
          // itself, then the second yield resumes here so the script can
          // overwrite its own reference. The host root keeps the closure
          // alive independently of the script binding.
          sinkFmt(&s, "%*sparkedFn = %s\n", ind, "", name);
          sinkFmt(&s, "%*sFiber.yield(0)\n", ind, "");
          sinkFmt(&s, "%*sFiber.yield(0)\n", ind, "");
          sinkFmt(&s, "%*sparkedFn = null\n", ind, "");
        }
        else
        {
          sinkFmt(&s, "%*slog.add(%s.call())\n", ind, "", name);
        }
        break;
      }

      case UP_OP_FIBER_NEW:
      {
        char name[UP_MAX_NAME];
        localName(plan, op->a, name, sizeof(name));
        fiberSlotOfDecl[op->a] = fiberSlot++;
        sinkFmt(&s, "%*svar %s = Fiber.new {\n", ind, "", name);
        renderFiberBody(&s, plan, op->a, ind + 2);
        sinkFmt(&s, "%*s}\n", ind, "");
        break;
      }

      case UP_OP_FIBER_RESUME:
      {
        int fiberDecl = -1;
        for (int k = 0; k < plan->numDecls; k++)
        {
          if (fiberSlotOfDecl[k] == op->a) { fiberDecl = k; break; }
        }
        char name[UP_MAX_NAME];
        localName(plan, fiberDecl, name, sizeof(name));
        if (op->b)
        {
          // Same two-yield handshake as UP_OP_CALL, but the host resumes the
          // parked fiber and logs the value it hands back.
          sinkFmt(&s, "%*sparkedFiber = %s\n", ind, "", name);
          sinkFmt(&s, "%*sFiber.yield(0)\n", ind, "");
          sinkFmt(&s, "%*sFiber.yield(0)\n", ind, "");
          sinkFmt(&s, "%*sparkedFiber = null\n", ind, "");
        }
        else
        {
          sinkFmt(&s, "%*slog.add(%s.call())\n", ind, "", name);
        }
        break;
      }

      case UP_OP_GC:
        // A single yield marks a host GC boundary. The host collects between
        // this resume and the next; the observation stream is unchanged.
        sinkFmt(&s, "%*sFiber.yield(0)\n", ind, "");
        break;

      case UP_OP_DEEPEN:
        sinkFmt(&s, "%*sDeep.run(%d)\n", ind, "", plan->deepDepth);
        break;
    }
    if (!s.ok) return -1;
  }

  sinkPut(&s, "}\n");
  return s.ok ? (int)s.len : -1;
}
