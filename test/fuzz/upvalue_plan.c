// upvalue_plan.c
//
// Binary plan decoder/encoder and structural validation.
//
// Input wire format (all integers are unsigned bytes; multi-byte fields are
// little endian):
//
//   offset  size  field
//   0       1     magic, must be UP_MAGIC (0xA5)
//   1       1     version, must be UP_VERSION (0x01)
//   2       1     deep-call depth seed (normalized into [4, UP_MAX_DEPTH])
//   3       1     declaration count N, 1..UP_MAX_DECLS
//   4       ..    N declaration records (see below)
//   next    1     operation count M, 1..UP_MAX_OPS
//   next    ..    M operation records (see below)
//
// Declaration record:
//   VAR   (kind=1): [kind][flags: bit0 = nameTag][blockDepth]
//                   [initial.lo][initial.hi]        (masked to 10 bits)
//   FN    (kind=2): [kind][fnKind 1..2][blockDepth]
//                   [capturedDecl][indirect 0..2]
//                   [write.lo][write.hi]            (masked to 10 bits)
//   FIBER (kind=3): [kind][blockDepth][yields 0..3]
//                   [hasWriter][write.lo][write.hi]
//                   [locDecl][readerFnDecl][writerFnDecl or 0xFF]
//
// Operation record:
//   SCOPE_ENTER/EXIT, GC, DEEPEN : [code]
//   DECL        : [code][decl]
//   ALIAS       : [code][targetVarDecl][sourceFnDecl]
//   CALL        : [code][fnDecl][viaHost 0/1]
//   FIBER_NEW   : [code][fiberDecl]
//   FIBER_RESUME: [code][fiberSlot][viaHost 0/1]
//
// A reference is legal only when it names a declaration of the right kind that
// has already been materialized by an earlier DECL/FIBER_NEW operation. Every
// fetch is bounds checked, so a truncated or malformed input is rejected with
// an UpDecodeResult instead of being dereferenced.

#include "upvalue_plan.h"

#include <string.h>

#define VALUE_MASK 0x3FF

typedef struct {
  const uint8_t* p;
  size_t size;
  size_t pos;
  int truncated;
} Reader;

static int rdByte(Reader* r)
{
  if (r->pos >= r->size)
  {
    r->truncated = 1;
    return 0;
  }
  return r->p[r->pos++];
}

static int rdU16(Reader* r)
{
  int lo = rdByte(r);
  int hi = rdByte(r);
  return (lo | (hi << 8)) & VALUE_MASK;
}

const char* upvaluePlanDecodeError(UpDecodeResult result)
{
  switch (result)
  {
    case UP_DECODE_OK:          return "ok";
    case UP_DECODE_EMPTY:       return "empty input";
    case UP_DECODE_TRUNCATED:   return "truncated input";
    case UP_DECODE_BAD_HEADER:  return "bad magic or version";
    case UP_DECODE_BAD_OP:      return "unknown operation code";
    case UP_DECODE_BAD_REF:     return "illegal or out of range reference";
    case UP_DECODE_BAD_SCOPE:   return "unbalanced or too deep scope";
    case UP_DECODE_BAD_NAME:    return "duplicate live shadow name";
    case UP_DECODE_LIMIT:       return "plan exceeds a built-in limit";
  }
  return "unknown decode error";
}

static void initPlan(UpPlan* plan)
{
  memset(plan, 0, sizeof(*plan));
  for (int i = 0; i < UP_MAX_DECLS; i++)
  {
    plan->decls[i].readerFn = -1;
    plan->decls[i].writerFn = -1;
    plan->decls[i].locDecl = -1;
    plan->decls[i].capturedDecl = -1;
  }
  plan->deepDepth = 4;
}

UpDecodeResult upvaluePlanDecode(const uint8_t* data, size_t size,
                                 UpPlan* plan)
{
  if (data == NULL || size == 0) return UP_DECODE_EMPTY;

  Reader r;
  r.p = data;
  r.size = size;
  r.pos = 0;
  r.truncated = 0;

  int magic = rdByte(&r);
  int version = rdByte(&r);
  if (r.truncated) return UP_DECODE_TRUNCATED;
  if (magic != UP_MAGIC || version != UP_VERSION) return UP_DECODE_BAD_HEADER;

  int depthSeed = rdByte(&r);
  int numDecls = rdByte(&r);
  if (r.truncated) return UP_DECODE_TRUNCATED;
  if (numDecls < 1 || numDecls > UP_MAX_DECLS) return UP_DECODE_LIMIT;

  UpPlan tmp;
  initPlan(&tmp);
  tmp.deepDepth = 4 + (depthSeed % (UP_MAX_DEPTH - 3));

  int nextFiber = 0;

  for (int i = 0; i < numDecls; i++)
  {
    int kind = rdByte(&r);
    if (r.truncated) return UP_DECODE_TRUNCATED;

    UpDecl* d = &tmp.decls[i];
    d->id = i;

    if (kind == UP_DECL_VAR)
    {
      int flags = rdByte(&r);
      int depth = rdByte(&r);
      int initial = rdU16(&r);
      if (r.truncated) return UP_DECODE_TRUNCATED;
      if (depth < 0 || depth > UP_MAX_NEST) return UP_DECODE_BAD_SCOPE;
      d->kind = UP_DECL_VAR;
      d->nameTag = flags & 1;
      d->blockDepth = depth;
      d->initial = initial;
      tmp.numVars++;
    }
    else if (kind == UP_DECL_FN)
    {
      int fnKind = rdByte(&r);
      int depth = rdByte(&r);
      int captured = rdByte(&r);
      int indirect = rdByte(&r);
      int writeValue = rdU16(&r);
      if (r.truncated) return UP_DECODE_TRUNCATED;
      if (fnKind != UP_FN_READ && fnKind != UP_FN_WRITE)
        return UP_DECODE_BAD_OP;
      if (depth < 0 || depth > UP_MAX_NEST) return UP_DECODE_BAD_SCOPE;
      if (captured >= i) return UP_DECODE_BAD_REF;
      if (tmp.decls[captured].kind != UP_DECL_VAR)
        return UP_DECODE_BAD_REF;
      if (indirect < 0 || indirect > UP_MAX_INDIRECT)
        return UP_DECODE_LIMIT;
      d->kind = UP_DECL_FN;
      d->fnKind = fnKind;
      d->blockDepth = depth;
      d->capturedDecl = captured;
      d->indirect = indirect;
      d->writeValue = writeValue;
      tmp.numFns++;
    }
    else if (kind == UP_DECL_FIBER)
    {
      int depth = rdByte(&r);
      int yields = rdByte(&r);
      int hasWriter = rdByte(&r);
      int writeValue = rdU16(&r);
      int locDecl = rdByte(&r);
      int readerFn = rdByte(&r);
      int writerRef = rdByte(&r);
      if (r.truncated) return UP_DECODE_TRUNCATED;
      if (depth < 0 || depth > UP_MAX_NEST) return UP_DECODE_BAD_SCOPE;
      if (yields < 0 || yields > UP_MAX_YIELDS) return UP_DECODE_LIMIT;
      if (hasWriter != 0 && hasWriter != 1) return UP_DECODE_BAD_REF;
      if (writerRef == 0xFF) hasWriter = 0;

      // The fiber references an already encoded VAR cell and reader/writer FN
      // declarations. They are ordinary records in one shared id namespace;
      // the operation pass below enforces that they are never DECL'd at the
      // top level (they are emitted inside the Fiber body instead).
      if (locDecl >= i || readerFn >= i) return UP_DECODE_BAD_REF;
      if (tmp.decls[locDecl].kind != UP_DECL_VAR) return UP_DECODE_BAD_REF;
      if (tmp.decls[readerFn].kind != UP_DECL_FN ||
          tmp.decls[readerFn].fnKind != UP_FN_READ ||
          tmp.decls[readerFn].capturedDecl != locDecl)
        return UP_DECODE_BAD_REF;

      int writerFn = -1;
      if (hasWriter)
      {
        if (writerRef >= i) return UP_DECODE_BAD_REF;
        if (tmp.decls[writerRef].kind != UP_DECL_FN ||
            tmp.decls[writerRef].fnKind != UP_FN_WRITE ||
            tmp.decls[writerRef].capturedDecl != locDecl)
          return UP_DECODE_BAD_REF;
        writerFn = writerRef;
      }

      d->kind = UP_DECL_FIBER;
      d->blockDepth = depth;
      d->yields = yields;
      d->writeValue = writeValue;
      d->locDecl = locDecl;
      d->readerFn = readerFn;
      d->writerFn = writerFn;
      nextFiber++;
    }
    else
    {
      return UP_DECODE_BAD_OP;
    }
  }

  tmp.numDecls = numDecls;
  tmp.numFiberDecls = nextFiber;
  tmp.encodedTopDecls = numDecls;

  // Declarations owned by a Fiber body are emitted there and must never be
  // materialized by a top level DECL operation.
  int fiberOwned[UP_MAX_DECLS];
  memset(fiberOwned, 0, sizeof(fiberOwned));
  for (int i = 0; i < numDecls; i++)
  {
    if (tmp.decls[i].kind != UP_DECL_FIBER) continue;
    fiberOwned[tmp.decls[i].locDecl] = 1;
    fiberOwned[tmp.decls[i].readerFn] = 1;
    if (tmp.decls[i].writerFn >= 0) fiberOwned[tmp.decls[i].writerFn] = 1;
  }

  int numOps = rdByte(&r);
  if (r.truncated) return UP_DECODE_TRUNCATED;
  if (numOps < 1 || numOps > UP_MAX_OPS) return UP_DECODE_LIMIT;

  int depth = 0;
  int declared[UP_MAX_DECLS];
  int live[UP_MAX_DECLS];
  memset(declared, 0, sizeof(declared));
  memset(live, 0, sizeof(live));

  int fiberOrdinals = 0;
  int fiberNewSeen[UP_MAX_FIBERS];
  int resumeCount[UP_MAX_FIBERS];
  int aliasedVar[UP_MAX_DECLS];
  int capturedVar[UP_MAX_DECLS];
  memset(fiberNewSeen, 0, sizeof(fiberNewSeen));
  memset(resumeCount, 0, sizeof(resumeCount));
  memset(aliasedVar, 0, sizeof(aliasedVar));
  memset(capturedVar, 0, sizeof(capturedVar));
  // Every VAR that a closure captures is a typed integer cell and must never
  // be reassigned to hold a closure, or a reader could hand back a non-integer.
  for (int i = 0; i < numDecls; i++)
  {
    if (tmp.decls[i].kind == UP_DECL_FN)
      capturedVar[tmp.decls[i].capturedDecl] = 1;
  }

  for (int op = 0; op < numOps; op++)
  {
    int code = rdByte(&r);
    if (r.truncated) return UP_DECODE_TRUNCATED;

    UpOp* o = &tmp.ops[tmp.numOps];

    switch (code)
    {
      case UP_OP_SCOPE_ENTER:
        depth++;
        if (depth > UP_MAX_NEST) return UP_DECODE_BAD_SCOPE;
        o->code = UP_OP_SCOPE_ENTER;
        break;

      case UP_OP_SCOPE_EXIT:
        if (depth == 0) return UP_DECODE_BAD_SCOPE;
        depth--;
        o->code = UP_OP_SCOPE_EXIT;
        // Declarations whose block depth is now below the current depth die.
        for (int k = 0; k < tmp.numDecls; k++)
        {
          if (live[k] && tmp.decls[k].blockDepth > depth) live[k] = 0;
        }
        break;

      case UP_OP_DECL:
      {
        int decl = rdByte(&r);
        if (r.truncated) return UP_DECODE_TRUNCATED;
        if (decl < 0 || decl >= numDecls) return UP_DECODE_BAD_REF;
        UpDecl* d = &tmp.decls[decl];
        if (d->kind == UP_DECL_FIBER || fiberOwned[decl])
        {
          // Fiber templates and fiber-private declarations are materialized
          // only inside a Fiber body / FIBER_NEW, never by a top level DECL.
          return UP_DECODE_BAD_REF;
        }
        if (declared[decl]) return UP_DECODE_BAD_REF;
        if (d->blockDepth != depth) return UP_DECODE_BAD_SCOPE;
        if (d->kind == UP_DECL_FN)
        {
          UpDecl* cell = &tmp.decls[d->capturedDecl];
          if (!live[d->capturedDecl]) return UP_DECODE_BAD_REF;
          if (cell->blockDepth > d->blockDepth) return UP_DECODE_BAD_SCOPE;
          // A closure cell may not be a local that already holds an aliased
          // closure, or a read would hand back a function rather than an int.
          if (aliasedVar[d->capturedDecl]) return UP_DECODE_BAD_REF;
          // When the captured cell shares the spelling "s", Wren's name
          // resolution binds the innermost same-named local in scope. Require
          // the plan to name that same innermost declaration, so the rendered
          // program and the id-based model cannot diverge on a shadowed outer
          // local.
          if (cell->kind == UP_DECL_VAR && cell->nameTag)
          {
            for (int k = 0; k < tmp.numDecls; k++)
            {
              UpDecl* other = &tmp.decls[k];
              if (live[k] && other->kind == UP_DECL_VAR && other->nameTag &&
                  other->blockDepth > cell->blockDepth)
              {
                return UP_DECODE_BAD_REF;
              }
            }
          }
        }
        if (d->kind == UP_DECL_VAR && d->nameTag)
        {
          // Same spelling is only a hard collision inside one scope depth.
          // Nested same-name locals are deliberate shadowing: they keep
          // distinct declaration ids (and cells), and the FN capture rule
          // below guarantees each closure binds the innermost one in scope,
          // exactly as Wren's name resolution does.
          for (int k = 0; k < tmp.numDecls; k++)
          {
            UpDecl* other = &tmp.decls[k];
            if (live[k] && other->kind == UP_DECL_VAR && other->nameTag &&
                other->blockDepth == d->blockDepth && k != decl)
            {
              return UP_DECODE_BAD_NAME;
            }
          }
        }
        declared[decl] = 1;
        live[decl] = 1;
        o->code = UP_OP_DECL;
        o->a = decl;
        break;
      }

      case UP_OP_ALIAS:
      {
        int target = rdByte(&r);
        int source = rdByte(&r);
        if (r.truncated) return UP_DECODE_TRUNCATED;
        if (target < 0 || target >= tmp.numDecls ||
            source < 0 || source >= tmp.numDecls) return UP_DECODE_BAD_REF;
        if (!live[target] || !live[source]) return UP_DECODE_BAD_REF;
        if (tmp.decls[target].kind != UP_DECL_VAR ||
            tmp.decls[source].kind != UP_DECL_FN) return UP_DECODE_BAD_REF;
        if (capturedVar[target])
        {
          // A captured cell stays an integer for its whole lifetime; do not
          // allow a closure to be stored into it via an alias.
          return UP_DECODE_BAD_REF;
        }
        aliasedVar[target] = 1;
        o->code = UP_OP_ALIAS;
        o->a = target;
        o->b = source;
        break;
      }

      case UP_OP_CALL:
      {
        int fn = rdByte(&r);
        int viaHost = rdByte(&r);
        if (r.truncated) return UP_DECODE_TRUNCATED;
        if (fn < 0 || fn >= tmp.numDecls) return UP_DECODE_BAD_REF;
        if (!live[fn]) return UP_DECODE_BAD_REF;
        // A callable is either an FN declaration or a VAR that a prior ALIAS
        // made to hold a closure. The latter exercises calls through an alias.
        int isFn = tmp.decls[fn].kind == UP_DECL_FN;
        int isAliasedVar = tmp.decls[fn].kind == UP_DECL_VAR &&
                           aliasedVar[fn];
        if (!isFn && !isAliasedVar) return UP_DECODE_BAD_REF;
        if (viaHost != 0 && viaHost != 1) return UP_DECODE_BAD_REF;
        o->code = UP_OP_CALL;
        o->a = fn;
        o->b = viaHost;
        break;
      }

      case UP_OP_FIBER_NEW:
      {
        int fiberDecl = rdByte(&r);
        if (r.truncated) return UP_DECODE_TRUNCATED;
        if (fiberDecl < 0 || fiberDecl >= numDecls)
          return UP_DECODE_BAD_REF;
        if (tmp.decls[fiberDecl].kind != UP_DECL_FIBER)
          return UP_DECODE_BAD_REF;
        if (declared[fiberDecl]) return UP_DECODE_BAD_REF;
        if (tmp.decls[fiberDecl].blockDepth != depth)
          return UP_DECODE_BAD_SCOPE;
        if (fiberOrdinals >= UP_MAX_FIBERS) return UP_DECODE_LIMIT;
        declared[fiberDecl] = 1;
        live[fiberDecl] = 1;
        fiberNewSeen[fiberOrdinals] = fiberDecl;
        o->code = UP_OP_FIBER_NEW;
        o->a = fiberDecl;
        o->b = fiberOrdinals;
        fiberOrdinals++;
        break;
      }

      case UP_OP_FIBER_RESUME:
      {
        int slot = rdByte(&r);
        int viaHost = rdByte(&r);
        if (r.truncated) return UP_DECODE_TRUNCATED;
        if (slot < 0 || slot >= fiberOrdinals) return UP_DECODE_BAD_REF;
        if (viaHost != 0 && viaHost != 1) return UP_DECODE_BAD_REF;
        // The fiber must still be in scope; a block-scoped Fiber local cannot
        // be resumed after its block has closed.
        int resumedFiberDecl = fiberNewSeen[slot];
        if (!live[resumedFiberDecl]) return UP_DECODE_BAD_REF;
        // A fiber hands back control exactly yields+1 times; one extra resume
        // would be a runtime error in Wren and is rejected up front.
        int resumesAllowed = tmp.decls[resumedFiberDecl].yields + 1;
        if (resumeCount[slot] >= resumesAllowed) return UP_DECODE_BAD_REF;
        resumeCount[slot]++;
        o->code = UP_OP_FIBER_RESUME;
        o->a = slot;
        o->b = viaHost;
        break;
      }

      case UP_OP_GC:
        o->code = UP_OP_GC;
        break;

      case UP_OP_DEEPEN:
        o->code = UP_OP_DEEPEN;
        break;

      default:
        return UP_DECODE_BAD_OP;
    }
    tmp.numOps++;
  }

  if (r.pos != size)
  {
    // Trailing bytes are tolerated only when zero; anything else means the
    // input targeted a different version and must be rejected.
    for (size_t k = r.pos; k < size; k++)
    {
      if (r.p[k] != 0) return UP_DECODE_BAD_HEADER;
    }
  }

  tmp.numFibers = fiberOrdinals;
  tmp.maxDepth = depth;

  // Collect the distinct captured cells.
  for (int i = 0; i < tmp.numDecls; i++)
  {
    UpDecl* d = &tmp.decls[i];
    if (d->kind != UP_DECL_FN) continue;
    int cell = d->capturedDecl;
    int found = 0;
    for (int k = 0; k < tmp.numCells; k++)
    {
      if (tmp.cellDecls[k] == cell) { found = 1; break; }
    }
    if (!found)
    {
      if (tmp.numCells >= UP_MAX_CELLS) return UP_DECODE_LIMIT;
      tmp.cellDecls[tmp.numCells++] = cell;
    }
  }

  *plan = tmp;
  return UP_DECODE_OK;
}

// Encoder mirroring the decoder above.
typedef struct {
  uint8_t* p;
  size_t cap;
  size_t pos;
  int overflow;
} Writer;

static void wrByte(Writer* w, int v)
{
  if (w->pos >= w->cap) { w->overflow = 1; return; }
  w->p[w->pos++] = (uint8_t)(v & 0xFF);
}

static void wrU16(Writer* w, int v)
{
  v &= VALUE_MASK;
  wrByte(w, v & 0xFF);
  wrByte(w, (v >> 8) & 0xFF);
}

size_t upvaluePlanEncode(const UpPlan* plan, uint8_t* out, size_t cap)
{
  Writer w;
  w.p = out;
  w.cap = cap;
  w.pos = 0;
  w.overflow = 0;

  wrByte(&w, UP_MAGIC);
  wrByte(&w, UP_VERSION);
  wrByte(&w, plan->deepDepth & 0xFF);

  // Only the explicitly encoded declaration records go on the wire; the
  // fiber-private VAR/FN triples are derived again by the decoder.
  int encodedCount = plan->encodedTopDecls;
  wrByte(&w, encodedCount);

  for (int i = 0; i < encodedCount; i++)
  {
    UpDecl const* d = &plan->decls[i];
    if (d->kind == UP_DECL_VAR)
    {
      wrByte(&w, UP_DECL_VAR);
      wrByte(&w, d->nameTag & 1);
      wrByte(&w, d->blockDepth);
      wrU16(&w, d->initial);
    }
    else if (d->kind == UP_DECL_FN)
    {
      wrByte(&w, UP_DECL_FN);
      wrByte(&w, d->fnKind);
      wrByte(&w, d->blockDepth);
      wrByte(&w, d->capturedDecl);
      wrByte(&w, d->indirect);
      wrU16(&w, d->writeValue);
    }
    else if (d->kind == UP_DECL_FIBER)
    {
      wrByte(&w, UP_DECL_FIBER);
      wrByte(&w, d->blockDepth);
      wrByte(&w, d->yields);
      wrByte(&w, d->writerFn >= 0 ? 1 : 0);
      wrU16(&w, d->writeValue);
      wrByte(&w, d->locDecl);
      wrByte(&w, d->readerFn);
      wrByte(&w, d->writerFn >= 0 ? d->writerFn : 0xFF);
    }
  }

  wrByte(&w, plan->numOps);
  for (int i = 0; i < plan->numOps; i++)
  {
    UpOp const* o = &plan->ops[i];
    wrByte(&w, o->code);
    switch (o->code)
    {
      case UP_OP_SCOPE_ENTER:
      case UP_OP_SCOPE_EXIT:
      case UP_OP_GC:
      case UP_OP_DEEPEN:
        break;
      case UP_OP_DECL:
      case UP_OP_FIBER_NEW:
        wrByte(&w, o->a);
        break;
      case UP_OP_ALIAS:
        wrByte(&w, o->a);
        wrByte(&w, o->b);
        break;
      case UP_OP_CALL:
      case UP_OP_FIBER_RESUME:
        wrByte(&w, o->a);
        wrByte(&w, o->b);
        break;
    }
  }

  if (w.overflow) return 0;
  return w.pos;
}
