// upvalue_plan.c
//
// Wire decoding/validation, the shared plan builder, the Wren source renderer
// and the deterministic structured generator. The decoder is deliberately
// independent of the builder: generated plans travel through the encoder and
// back through the decoder before execution, so the two cannot quietly agree
// on an invalid structure.
#include "upvalue_fuzz.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Bounds-checked big-endian cursor
// ---------------------------------------------------------------------------
typedef struct {
  const uint8_t *p;
  const uint8_t *end;
  int ok;
} Cursor;

static int curU8(Cursor *c) {
  if (c->p >= c->end) { c->ok = 0; return 0; }
  return *c->p++;
}

static int curU16(Cursor *c) {
  if (c->p + 2 > c->end) { c->ok = 0; return 0; }
  int v = (c->p[0] << 8) | c->p[1];
  c->p += 2;
  return v;
}

static int curS16(Cursor *c) { return (int16_t)curU16(c); }

// ---------------------------------------------------------------------------
// Declaration lookup and lexical relationships
// ---------------------------------------------------------------------------
UpDecl *upFindDecl(UpPlan *plan, int id);

UpDecl *upFindDecl(UpPlan *plan, int id) {
  for (int i = 0; i < plan->declCount; i++)
    if (plan->decls[i].id == id) return &plan->decls[i];
  return NULL;
}

static int streamEncloses(const UpPlan *plan, int outer, int inner) {
  int s = inner;
  while (s >= 0) {
    if (s == outer) return 1;
    s = plan->streams[s].parent;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------
static UpDecodeStatus validatePlan(const UpPlan *plan);

UpDecodeStatus upDecodePlan(const uint8_t *data, size_t length, UpPlan *plan) {
  memset(plan, 0, sizeof(*plan));
  if (length < 3) return UP_DECODE_TRUNCATED;
  if (data[0] != UP_MAGIC) return UP_DECODE_BAD_MAGIC;
  if (data[1] != UP_VERSION) return UP_DECODE_BAD_MAGIC;

  Cursor c = { data + 2, data + length, 1 };
  int streamStack[UP_MAX_NESTING + 2];
  int streamTop = 0;
  int scopeDepth[UP_MAX_STREAMS];
  memset(scopeDepth, 0, sizeof(scopeDepth));

  // Stream 0 is the main program.
  plan->streams[0].parent = -1;
  plan->streams[0].depthBase = 0;
  plan->streams[0].firstOp = 0;
  plan->streams[0].lastOp = 0;
  plan->streamCount = 1;
  streamStack[streamTop++] = 0;

  int ended = 0;
  while (c.p < c.end) {
    int op = curU8(&c);
    if (!c.ok) return UP_DECODE_TRUNCATED;
    if (op == UP_OP_END) { ended = 1; break; }
    if (op <= UP_OP_END || op >= UP_OP_COUNT) return UP_DECODE_UNKNOWN_OP;
    if (plan->opCount >= UP_MAX_OPS) return UP_DECODE_LIMIT;

    UpOp *o = &plan->ops[plan->opCount++];
    memset(o, 0, sizeof(*o));
    o->code = (uint8_t)op;
    int curStream = streamStack[streamTop - 1];
    o->stream = curStream;

    switch (op) {
      case UP_SCOPE_ENTER:
        if (++scopeDepth[curStream] > UP_MAX_SCOPES) return UP_DECODE_LIMIT;
        break;
      case UP_SCOPE_EXIT:
        if (scopeDepth[curStream] <= 0) return UP_DECODE_BAD_STRUCTURE;
        scopeDepth[curStream]--;
        break;
      case UP_VAR: {
        int id = curU16(&c), init = curS16(&c);
        if (!c.ok) return UP_DECODE_TRUNCATED;
        if (id >= UP_MAX_DECLS || upFindDecl(plan, id) != NULL)
          return UP_DECODE_BAD_REF;
        if (init < UP_MIN_VALUE || init > UP_MAX_VALUE)
          return UP_DECODE_LIMIT;
        if (plan->declCount >= UP_MAX_DECLS) return UP_DECODE_LIMIT;
        UpDecl *d = &plan->decls[plan->declCount++];
        d->kind = UP_DECL_VAR; d->id = id;
        d->depth = scopeDepth[curStream]; d->stream = curStream;
        d->initValue = init;
        o->a = id;
        break;
      }
      case UP_SET: {
        int id = curU16(&c), val = curS16(&c);
        if (!c.ok) return UP_DECODE_TRUNCATED;
        o->a = id; o->b = val;
        if (val < UP_MIN_VALUE || val > UP_MAX_VALUE) return UP_DECODE_LIMIT;
        break;
      }
      case UP_CLOSURE: {
        int id = curU16(&c);
        int ncap = curU8(&c);
        if (!c.ok) return UP_DECODE_TRUNCATED;
        if (ncap < 1 || ncap > UP_MAX_CAPS) return UP_DECODE_LIMIT;
        if (id >= UP_MAX_DECLS || upFindDecl(plan, id) != NULL)
          return UP_DECODE_BAD_REF;
        if (plan->declCount >= UP_MAX_DECLS) return UP_DECODE_LIMIT;
        UpDecl *d = &plan->decls[plan->declCount++];
        d->kind = UP_DECL_CLOSURE; d->id = id;
        d->depth = scopeDepth[curStream]; d->stream = curStream;
        d->closureObject = plan->declCount - 1;
        d->capCount = ncap;
        for (int i = 0; i < ncap; i++) {
          int varId = curU16(&c), mode = curU8(&c), w = curS16(&c);
          if (!c.ok) return UP_DECODE_TRUNCATED;
          if (mode < UP_MODE_READ || mode > UP_MODE_READ_WRITE)
            return UP_DECODE_BAD_STRUCTURE;
          if (w < UP_MIN_VALUE || w > UP_MAX_VALUE) return UP_DECODE_LIMIT;
          d->caps[i].varId = varId;
          d->caps[i].mode = mode;
          d->caps[i].writeConst = w;
        }
        o->a = id;
        break;
      }
      case UP_ALIAS: {
        int newId = curU16(&c), src = curU16(&c);
        if (!c.ok) return UP_DECODE_TRUNCATED;
        o->a = newId; o->b = src;
        if (newId >= UP_MAX_DECLS || upFindDecl(plan, newId) != NULL)
          return UP_DECODE_BAD_REF;
        if (plan->declCount >= UP_MAX_DECLS) return UP_DECODE_LIMIT;
        UpDecl *d = &plan->decls[plan->declCount++];
        d->kind = UP_DECL_CLOSURE; d->id = newId;
        d->depth = scopeDepth[curStream]; d->stream = curStream;
        d->closureObject = -1;
        d->capCount = 0;
        break;
      }
      case UP_WRAP_BEGIN: {
        int id = curU16(&c);
        if (!c.ok) return UP_DECODE_TRUNCATED;
        if (id >= UP_MAX_DECLS || upFindDecl(plan, id) != NULL)
          return UP_DECODE_BAD_REF;
        if (streamTop - 1 >= UP_MAX_NESTING || plan->streamCount >= UP_MAX_STREAMS)
          return UP_DECODE_LIMIT;
        if (plan->declCount >= UP_MAX_DECLS) return UP_DECODE_LIMIT;
        int s = plan->streamCount++;
        plan->streams[s].parent = curStream;
        plan->streams[s].depthBase = scopeDepth[curStream] + 1;
        plan->streams[s].firstOp = plan->opCount;
        plan->streams[s].lastOp = plan->opCount;
        UpDecl *d = &plan->decls[plan->declCount++];
        d->kind = UP_DECL_WRAP; d->id = id;
        d->depth = scopeDepth[curStream]; d->stream = curStream;
        d->fiberBodyStream = s;
        o->a = id;
        streamStack[streamTop++] = s;
        break;
      }
      case UP_WRAP_END: {
        int id = curU16(&c), resultClosure = curU16(&c);
        if (!c.ok) return UP_DECODE_TRUNCATED;
        if (streamTop <= 1) return UP_DECODE_BAD_STRUCTURE;
        int s = streamStack[--streamTop];
        if (upFindDecl(plan, id) == NULL) return UP_DECODE_BAD_REF;
        if (scopeDepth[s] != 0) return UP_DECODE_BAD_STRUCTURE;
        plan->streams[s].lastOp = plan->opCount;
        o->stream = plan->streams[s].parent;
        o->a = id; o->b = resultClosure;
        break;
      }
      case UP_CALL: {
        int id = curU16(&c);
        if (!c.ok) return UP_DECODE_TRUNCATED;
        o->a = id;
        break;
      }
      case UP_DROP: {
        int id = curU16(&c);
        if (!c.ok) return UP_DECODE_TRUNCATED;
        o->a = id;
        break;
      }
      case UP_FIBER_BEGIN: {
        int id = curU16(&c);
        if (!c.ok) return UP_DECODE_TRUNCATED;
        if (id >= UP_MAX_DECLS || upFindDecl(plan, id) != NULL)
          return UP_DECODE_BAD_REF;
        if (streamTop - 1 >= UP_MAX_NESTING || plan->streamCount >= UP_MAX_STREAMS)
          return UP_DECODE_LIMIT;
        if (plan->declCount >= UP_MAX_DECLS) return UP_DECODE_LIMIT;
        int s = plan->streamCount++;
        plan->streams[s].parent = curStream;
        plan->streams[s].depthBase = scopeDepth[curStream] + 1;
        plan->streams[s].firstOp = plan->opCount;
        plan->streams[s].lastOp = plan->opCount;
        UpDecl *d = &plan->decls[plan->declCount++];
        d->kind = UP_DECL_FIBER; d->id = id;
        d->depth = scopeDepth[curStream]; d->stream = curStream;
        d->fiberBodyStream = s;
        o->a = id;
        streamStack[streamTop++] = s;
        break;
      }
      case UP_FIBER_END: {
        if (streamTop <= 1) return UP_DECODE_BAD_STRUCTURE;
        int s = streamStack[--streamTop];
        if (scopeDepth[s] != 0) return UP_DECODE_BAD_STRUCTURE;
        plan->streams[s].lastOp = plan->opCount;
        o->stream = plan->streams[s].parent;
        break;
      }
      case UP_YIELD: {
        int val = curS16(&c);
        if (!c.ok) return UP_DECODE_TRUNCATED;
        if (val < UP_MIN_VALUE || val > UP_MAX_VALUE) return UP_DECODE_LIMIT;
        o->a = val;
        break;
      }
      case UP_DEPTH: {
        int d = curU8(&c);
        if (!c.ok) return UP_DECODE_TRUNCATED;
        if (d < 1 || d > UP_MAX_DEPTH) return UP_DECODE_LIMIT;
        o->a = d;
        break;
      }
      case UP_GC:
        break;
      case UP_RESUME: {
        int id = curU16(&c);
        if (!c.ok) return UP_DECODE_TRUNCATED;
        o->a = id;
        break;
      }
      case UP_SHADOW: {
        int outer = curU16(&c), outerInit = curS16(&c);
        int inner = curU16(&c), innerInit = curS16(&c);
        int innerClosure = curU16(&c);
        if (!c.ok) return UP_DECODE_TRUNCATED;
        o->a = outer; o->b = outerInit; o->c = inner;
        o->d = innerInit; o->e = innerClosure;
        for (int k = 0; k < 2; k++) {
          int id = k == 0 ? outer : inner;
          int init = k == 0 ? outerInit : innerInit;
          if (id >= UP_MAX_DECLS || upFindDecl(plan, id) != NULL)
            return UP_DECODE_BAD_REF;
          if (plan->declCount >= UP_MAX_DECLS) return UP_DECODE_LIMIT;
          UpDecl *d = &plan->decls[plan->declCount++];
          d->kind = UP_DECL_VAR; d->id = id;
          d->depth = scopeDepth[curStream] + (k == 1 ? 1 : 0);
          d->stream = curStream; d->initValue = init;
        }
        if (innerClosure >= UP_MAX_DECLS ||
            upFindDecl(plan, innerClosure) != NULL)
          return UP_DECODE_BAD_REF;
        if (plan->declCount >= UP_MAX_DECLS) return UP_DECODE_LIMIT;
        UpDecl *d = &plan->decls[plan->declCount++];
        d->kind = UP_DECL_CLOSURE; d->id = innerClosure;
        d->depth = scopeDepth[curStream] + 1; d->stream = curStream;
        d->closureObject = plan->declCount - 1;
        d->capCount = 1;
        d->caps[0].varId = inner;
        d->caps[0].mode = UP_MODE_READ_WRITE;
        d->caps[0].writeConst = innerInit + 1;
        break;
      }
      case UP_SLOTREUSE: {
        int idA = curU16(&c), initA = curS16(&c), cloA = curU16(&c);
        int idB = curU16(&c), initB = curS16(&c), cloB = curU16(&c);
        if (!c.ok) return UP_DECODE_TRUNCATED;
        o->a = idA; o->b = initA; o->c = cloA;
        o->d = idB; o->e = initB; o->f = cloB;
        int ids[2] = { idA, idB }, inits[2] = { initA, initB };
        int clos[2] = { cloA, cloB };
        for (int k = 0; k < 2; k++) {
          if (ids[k] >= UP_MAX_DECLS || upFindDecl(plan, ids[k]) != NULL ||
              clos[k] >= UP_MAX_DECLS || upFindDecl(plan, clos[k]) != NULL)
            return UP_DECODE_BAD_REF;
          if (plan->declCount + 2 >= UP_MAX_DECLS) return UP_DECODE_LIMIT;
          UpDecl *dv = &plan->decls[plan->declCount++];
          dv->kind = UP_DECL_VAR; dv->id = ids[k];
          dv->depth = scopeDepth[curStream] + 1;
          dv->stream = curStream; dv->initValue = inits[k];
          UpDecl *dc = &plan->decls[plan->declCount++];
          dc->kind = UP_DECL_CLOSURE; dc->id = clos[k];
          dc->depth = scopeDepth[curStream] + 1;
          dc->stream = curStream; dc->closureObject = plan->declCount - 1;
          dc->capCount = 1;
          dc->caps[0].varId = ids[k];
          dc->caps[0].mode = UP_MODE_READ_WRITE;
          dc->caps[0].writeConst = inits[k] + 1;
        }
        break;
      }
      default:
        return UP_DECODE_UNKNOWN_OP;
    }
  }
  if (!ended) return UP_DECODE_TRUNCATED;
  if (streamTop != 1 || scopeDepth[0] != 0)
    return UP_DECODE_BAD_STRUCTURE;
  for (int s = 1; s < plan->streamCount; s++)
    if (plan->streams[s].lastOp < plan->streams[s].firstOp)
      return UP_DECODE_BAD_STRUCTURE;
  return validatePlan(plan);
}

// ---------------------------------------------------------------------------
// Structural validation. Every reference must name a declaration that exists
// and is lexically visible at the point of use. This pass never touches Wren.
// ---------------------------------------------------------------------------
static int varVisibleAt(const UpPlan *plan, int varId, int stream) {
  UpDecl *v = upFindDecl((UpPlan *)plan, varId);
  if (v == NULL || v->kind != UP_DECL_VAR) return 0;
  return streamEncloses(plan, v->stream, stream);
}

static UpDecodeStatus validatePlan(const UpPlan *plan) {
  int born[UP_MAX_DECLS];
  int dropped[UP_MAX_DECLS];
  memset(born, 0, sizeof(born));
  memset(dropped, 0, sizeof(dropped));

  // Closure capture references must name visible variables.
  for (int i = 0; i < plan->declCount; i++) {
    const UpDecl *d = &plan->decls[i];
    if (d->kind != UP_DECL_CLOSURE || d->capCount == 0) continue;
    for (int k = 0; k < d->capCount; k++) {
      if (!varVisibleAt(plan, d->caps[k].varId, d->stream))
        return UP_DECODE_BAD_REF;
    }
  }

  // Walk ops in global order, tracking object lifetimes.
  for (int i = 0; i < plan->opCount; i++) {
    const UpOp *o = &plan->ops[i];
    int s = o->stream;
    switch (o->code) {
      case UP_VAR:
      case UP_SCOPE_ENTER:
      case UP_SCOPE_EXIT:
      case UP_SHADOW:
      case UP_SLOTREUSE:
        break;
      case UP_SET: {
        UpDecl *v = upFindDecl((UpPlan *)plan, o->a);
        if (v == NULL || v->kind != UP_DECL_VAR)
          return UP_DECODE_BAD_REF;
        if (!streamEncloses(plan, v->stream, s))
          return UP_DECODE_BAD_REF;
        break;
      }
      case UP_CLOSURE:
      case UP_FIBER_BEGIN:
      case UP_WRAP_BEGIN:
        if (upFindDecl((UpPlan *)plan, o->a) == NULL)
          return UP_DECODE_BAD_REF;
        born[o->a] = 1;
        break;
      case UP_ALIAS: {
        UpDecl *src = upFindDecl((UpPlan *)plan, o->b);
        if (src == NULL ||
            (src->kind != UP_DECL_CLOSURE && src->kind != UP_DECL_WRAP))
          return UP_DECODE_BAD_REF;
        if (!born[o->b] || dropped[o->b]) return UP_DECODE_BAD_REF;
        if (upFindDecl((UpPlan *)plan, o->a) == NULL)
          return UP_DECODE_BAD_REF;
        born[o->a] = 1;
        break;
      }
      case UP_CALL: {
        UpDecl *d = upFindDecl((UpPlan *)plan, o->a);
        if (d == NULL || d->kind != UP_DECL_CLOSURE)
          return UP_DECODE_BAD_REF;
        if (!born[o->a] || dropped[o->a]) return UP_DECODE_BAD_REF;
        break;
      }
      case UP_DROP: {
        UpDecl *d = upFindDecl((UpPlan *)plan, o->a);
        if (d == NULL || (d->kind != UP_DECL_CLOSURE &&
                          d->kind != UP_DECL_FIBER))
          return UP_DECODE_BAD_REF;
        if (!born[o->a] || dropped[o->a]) return UP_DECODE_BAD_REF;
        dropped[o->a] = 1;
        break;
      }
      case UP_RESUME: {
        UpDecl *d = upFindDecl((UpPlan *)plan, o->a);
        if (d == NULL || d->kind != UP_DECL_FIBER)
          return UP_DECODE_BAD_REF;
        if (!born[o->a] || dropped[o->a]) return UP_DECODE_BAD_REF;
        break;
      }
      case UP_YIELD:
        if (s == 0) return UP_DECODE_BAD_STRUCTURE;
        break;
      case UP_DEPTH:
        if (plan->streams[s].parent < 0)
          return UP_DECODE_BAD_STRUCTURE;
        break;
      case UP_GC:
        if (s != 0) return UP_DECODE_BAD_STRUCTURE;
        break;
      case UP_WRAP_END: {
        UpDecl *rc = upFindDecl((UpPlan *)plan, o->b);
        UpDecl *wrap = upFindDecl((UpPlan *)plan, o->a);
        int bodyStream = wrap != NULL ? wrap->fiberBodyStream : -1;
        if (rc == NULL || rc->kind != UP_DECL_CLOSURE ||
            rc->stream != bodyStream || !born[o->b])
          return UP_DECODE_BAD_REF;
        break;
      }
      case UP_FIBER_END:
        break;
      default:
        return UP_DECODE_UNKNOWN_OP;
    }
  }

  // Fiber/wrap body streams must be properly nested.
  for (int i = 0; i < plan->declCount; i++) {
    const UpDecl *d = &plan->decls[i];
    if (d->kind != UP_DECL_FIBER && d->kind != UP_DECL_WRAP) continue;
    int bs = d->fiberBodyStream;
    if (bs <= 0 || bs >= plan->streamCount ||
        plan->streams[bs].parent != d->stream)
      return UP_DECODE_BAD_STRUCTURE;
    if (d->kind == UP_DECL_WRAP) {
      for (int j = 0; j < plan->declCount; j++) {
        const UpDecl *e = &plan->decls[j];
        if (e->kind == UP_DECL_FIBER && e->stream == bs)
          return UP_DECODE_BAD_STRUCTURE;
      }
    }
  }

  // Every cap-less closure decl must be an alias that resolves to a concrete
  // closure object or a wrapper.
  for (int i = 0; i < plan->declCount; i++) {
    const UpDecl *d = &plan->decls[i];
    if (d->kind != UP_DECL_CLOSURE || d->capCount > 0) continue;
    int sid = -1;
    for (int j = 0; j < plan->opCount; j++) {
      if (plan->ops[j].code == UP_ALIAS && plan->ops[j].a == d->id) {
        sid = plan->ops[j].b;
        break;
      }
    }
    if (sid < 0) return UP_DECODE_BAD_STRUCTURE;
    int guard = 0;
    while (guard++ < UP_MAX_DECLS) {
      UpDecl *t = upFindDecl((UpPlan *)plan, sid);
      if (t == NULL) return UP_DECODE_BAD_REF;
      if (t->kind == UP_DECL_WRAP) break;
      if (t->capCount > 0) break;
      int next = -1;
      for (int j = 0; j < plan->opCount; j++) {
        if (plan->ops[j].code == UP_ALIAS && plan->ops[j].a == t->id) {
          next = plan->ops[j].b;
          break;
        }
      }
      if (next < 0) return UP_DECODE_BAD_STRUCTURE;
      sid = next;
    }
    if (guard > UP_MAX_DECLS) return UP_DECODE_BAD_STRUCTURE;
  }
  return UP_DECODE_OK;
}

// ---------------------------------------------------------------------------
// Dynamic string buffer for rendering Wren source
// ---------------------------------------------------------------------------
typedef struct {
  char *data;
  size_t len;
  size_t cap;
} Sb;

static void sbInit(Sb *sb) {
  sb->cap = 4096;
  sb->len = 0;
  sb->data = (char *)malloc(sb->cap);
  if (sb->data) sb->data[0] = '\0';
}

static void sbPut(Sb *sb, const char *s) {
  if (!sb->data) return;
  size_t n = strlen(s);
  if (sb->len + n + 1 > sb->cap) {
    while (sb->len + n + 1 > sb->cap) sb->cap *= 2;
    char *p = (char *)realloc(sb->data, sb->cap);
    if (!p) { free(sb->data); sb->data = NULL; return; }
    sb->data = p;
  }
  memcpy(sb->data + sb->len, s, n);
  sb->len += n;
  sb->data[sb->len] = '\0';
}

static void sbFmt(Sb *sb, const char *fmt, ...) {
  char tmp[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  sbPut(sb, tmp);
}

static void sbIndent(Sb *sb, int depth) {
  for (int i = 0; i < depth; i++) sbPut(sb, "  ");
}

// Request codes handed from the driver fiber back to the host. They live far
// above the event integer range so the two encodings never overlap. Only the
// host-driven Main stream (stream 0) ever emits these; child Fiber bodies
// yield plain values back to Main and never talk to the host directly.
#define REQ_GC 200000
#define REQ_HANDLE_BASE 300000
#define REQ_DROP_BASE 500000
#define REQ_STEP 700000

// Rendered Wren names. Most declarations get a unique v<id>/c<id> name. The
// override table lets shadowing and same-slot-reuse deliberately emit the same
// textual name for two distinct declaration identities.
static const char *nameOverride[UP_MAX_DECLS];

static const char *emitVarName(int id) {
  static char names[UP_MAX_DECLS][16];
  if (nameOverride[id] != NULL) return nameOverride[id];
  snprintf(names[id], sizeof(names[id]), "v%d", id);
  return names[id];
}

static void closureName(char *out, size_t n, int id) {
  snprintf(out, n, "c%d", id);
}

// The per-closure event contract shared by the renderer and the C model.
static void renderClosureBody(Sb *sb, const UpDecl *d, int indent) {
  for (int k = 0; k < d->capCount; k++) {
    const UpCapture *cap = &d->caps[k];
    const char *vn = emitVarName(cap->varId);
    sbIndent(sb, indent);
    sbFmt(sb, "UpTrace.ev(%d, %d, %s)\n", cap->varId, UP_TAG_READ, vn);
    if (cap->mode & UP_MODE_WRITE) {
      sbIndent(sb, indent);
      sbFmt(sb, "%s = %d\n", vn, cap->writeConst);
      sbIndent(sb, indent);
      sbFmt(sb, "UpTrace.ev(%d, %d, %d)\n", cap->varId, UP_TAG_WRITE,
            cap->writeConst);
      sbIndent(sb, indent);
      sbFmt(sb, "UpTrace.ev(%d, %d, %s)\n", cap->varId, UP_TAG_READ, vn);
    }
  }
}

static void renderClosureDecl(Sb *sb, const UpDecl *d, int indent) {
  char cn[24];
  closureName(cn, sizeof(cn), d->id);
  sbIndent(sb, indent);
  sbFmt(sb, "var %s = Fn.new {\n", cn);
  renderClosureBody(sb, d, indent + 1);
  sbIndent(sb, indent);
  sbPut(sb, "}\n");
  sbIndent(sb, indent);
  sbFmt(sb, "h%d = %s\n", d->id, cn);
}

static void renderStream(Sb *sb, const UpPlan *plan, int stream, int indent);

static void renderOp(Sb *sb, const UpPlan *plan, int stream,
                     const UpOp *o, int indent) {
  switch (o->code) {
    case UP_SCOPE_ENTER:
      sbIndent(sb, indent);
      sbPut(sb, "{\n");
      break;
    case UP_SCOPE_EXIT:
      sbIndent(sb, indent);
      sbPut(sb, "}\n");
      break;
    case UP_VAR: {
      UpDecl *d = upFindDecl((UpPlan *)plan, o->a);
      sbIndent(sb, indent);
      sbFmt(sb, "var %s = %d\n", emitVarName(o->a), d->initValue);
      break;
    }
    case UP_SET:
      sbIndent(sb, indent);
      sbFmt(sb, "%s = %d\n", emitVarName(o->a), o->b);
      break;
    case UP_CLOSURE: {
      UpDecl *d = upFindDecl((UpPlan *)plan, o->a);
      renderClosureDecl(sb, d, indent);
      // Objects made in the host-driven Main stream are rooted at an explicit
      // boundary so a later DROP cannot precede the host handle. Objects made
      // inside a child fiber are rooted by the host poll at the resume step.
      if (stream == 0) {
        sbIndent(sb, indent);
        sbFmt(sb, "Fiber.yield(%d)\n", REQ_HANDLE_BASE + o->a);
      }
      break;
    }
    case UP_ALIAS: {
      UpDecl *src = upFindDecl((UpPlan *)plan, o->b);
      char cn[24];
      closureName(cn, sizeof(cn), o->a);
      if (src->kind == UP_DECL_WRAP) {
        sbIndent(sb, indent);
        sbFmt(sb, "var %s = w%d.call(0)\n", cn, o->b);
      } else {
        sbIndent(sb, indent);
        sbFmt(sb, "var %s = c%d\n", cn, o->b);
      }
      sbIndent(sb, indent);
      sbFmt(sb, "h%d = %s\n", o->a, cn);
      if (stream == 0) {
        sbIndent(sb, indent);
        sbFmt(sb, "Fiber.yield(%d)\n", REQ_HANDLE_BASE + o->a);
      }
      break;
    }
    case UP_WRAP_BEGIN: {
      UpDecl *d = upFindDecl((UpPlan *)plan, o->a);
      int bs = d->fiberBodyStream;
      sbIndent(sb, indent);
      sbFmt(sb, "var w%d = Fn.new { |w%darg|\n", o->a, o->a);
      renderStream(sb, plan, bs, indent + 1);
      const UpOp *end = NULL;
      for (int i = 0; i < plan->opCount; i++) {
        if (plan->ops[i].code == UP_WRAP_END && plan->ops[i].a == o->a) {
          end = &plan->ops[i];
          break;
        }
      }
      if (end) {
        sbIndent(sb, indent + 1);
        sbFmt(sb, "return c%d\n", end->b);
      }
      sbIndent(sb, indent);
      sbPut(sb, "}\n");
      sbIndent(sb, indent);
      sbFmt(sb, "h%d = w%d\n", o->a, o->a);
      if (stream == 0) {
        sbIndent(sb, indent);
        sbFmt(sb, "Fiber.yield(%d)\n", REQ_HANDLE_BASE + o->a);
      }
      break;
    }
    case UP_WRAP_END:
      break;
    case UP_FIBER_BEGIN: {
      UpDecl *d = upFindDecl((UpPlan *)plan, o->a);
      int bs = d->fiberBodyStream;
      sbIndent(sb, indent);
      sbFmt(sb, "var fib%d = Fiber.new {\n", o->a);
      renderStream(sb, plan, bs, indent + 1);
      sbIndent(sb, indent + 1);
      sbPut(sb, "return 0\n");
      sbIndent(sb, indent);
      sbPut(sb, "}\n");
      sbIndent(sb, indent);
      sbFmt(sb, "h%d = fib%d\n", o->a, o->a);
      if (stream == 0) {
        sbIndent(sb, indent);
        sbFmt(sb, "Fiber.yield(%d)\n", REQ_HANDLE_BASE + o->a);
      }
      break;
    }
    case UP_FIBER_END:
      break;
    case UP_CALL:
      // Invoke through the always-visible module holder: a closure may have
      // been declared inside a block that has since closed (a closed capture),
      // so its local name is not guaranteed to be in scope here.
      sbIndent(sb, indent);
      sbFmt(sb, "h%d.call()\n", o->a);
      break;
    case UP_DROP: {
      // Only the module-level holder is touched: it is visible from every
      // stream, and the host retains its own WrenHandle regardless.
      sbIndent(sb, indent);
      sbFmt(sb, "h%d = null\n", o->a);
      if (stream == 0) {
        sbIndent(sb, indent);
        sbFmt(sb, "Fiber.yield(%d)\n", REQ_DROP_BASE + o->a);
      }
      break;
    }
    case UP_YIELD: {
      int fiberId = -1;
      for (int i = 0; i < plan->declCount; i++) {
        const UpDecl *d = &plan->decls[i];
        if (d->kind == UP_DECL_FIBER && d->fiberBodyStream == o->stream) {
          fiberId = d->id;
          break;
        }
      }
      sbIndent(sb, indent);
      sbFmt(sb, "UpTrace.ev(%d, %d, %d)\n", fiberId, UP_TAG_YIELD, o->a);
      // A child fiber yields a plain value back to Main, never to the host.
      sbIndent(sb, indent);
      sbFmt(sb, "Fiber.yield(%d)\n", o->a);
      break;
    }
    case UP_DEPTH: {
      // A recursive closure that captures itself, leaving an open upvalue on
      // the child fiber while the bounded call chain forces wrenEnsureStack
      // growth. It emits no host synchronization.
      int seq = (int)(o - plan->ops);
      sbIndent(sb, indent);
      sbFmt(sb, "var dp%d = null\n", seq);
      sbIndent(sb, indent);
      sbFmt(sb, "dp%d = Fn.new { |n|\n", seq);
      sbIndent(sb, indent + 1);
      sbFmt(sb, "var work%d = n * 13\n", seq);
      sbIndent(sb, indent + 1);
      sbFmt(sb, "if (n == 0) return work%d\n", seq);
      sbIndent(sb, indent + 1);
      sbFmt(sb, "return dp%d.call(n - 1)\n", seq);
      sbIndent(sb, indent);
      sbPut(sb, "}\n");
      sbIndent(sb, indent);
      sbFmt(sb, "dp%d.call(%d)\n", seq, o->a);
      break;
    }
    case UP_GC:
      sbIndent(sb, indent);
      sbFmt(sb, "Fiber.yield(%d)\n", REQ_GC);
      break;
    case UP_RESUME:
      // Resume the child fiber, then hand control to the host at a boundary.
      sbIndent(sb, indent);
      sbFmt(sb, "fib%d.call()\n", o->a);
      sbIndent(sb, indent);
      sbFmt(sb, "Fiber.yield(%d)\n", REQ_STEP);
      break;
    case UP_SHADOW: {
      sbIndent(sb, indent);
      sbFmt(sb, "var %s = %d\n", emitVarName(o->a), o->b);
      sbIndent(sb, indent);
      sbPut(sb, "{\n");
      // Same textual name, distinct declaration identity and cell.
      nameOverride[o->c] = emitVarName(o->a);
      sbIndent(sb, indent + 1);
      sbFmt(sb, "var %s = %d\n", emitVarName(o->a), o->d);
      UpDecl *d = upFindDecl((UpPlan *)plan, o->e);
      renderClosureDecl(sb, d, indent + 1);
      sbIndent(sb, indent + 1);
      sbFmt(sb, "c%d.call()\n", o->e);
      nameOverride[o->c] = NULL;
      sbIndent(sb, indent);
      sbPut(sb, "}\n");
      break;
    }
    case UP_SLOTREUSE: {
      // Two sequential scopes reuse one stack slot but keep distinct cells.
      static const char *shared = "sharedSlot";
      nameOverride[o->a] = shared;
      sbIndent(sb, indent);
      sbPut(sb, "{\n");
      sbIndent(sb, indent + 1);
      sbFmt(sb, "var %s = %d\n", shared, o->b);
      UpDecl *da = upFindDecl((UpPlan *)plan, o->c);
      renderClosureDecl(sb, da, indent + 1);
      sbIndent(sb, indent);
      sbPut(sb, "}\n");
      nameOverride[o->a] = NULL;

      nameOverride[o->d] = shared;
      sbIndent(sb, indent);
      sbPut(sb, "{\n");
      sbIndent(sb, indent + 1);
      sbFmt(sb, "var %s = %d\n", shared, o->e);
      UpDecl *db = upFindDecl((UpPlan *)plan, o->f);
      renderClosureDecl(sb, db, indent + 1);
      sbIndent(sb, indent);
      sbPut(sb, "}\n");
      nameOverride[o->d] = NULL;

      // The closure locals are out of scope here; invoke them through the
      // module holders that keep their (now closed) upvalues alive.
      sbIndent(sb, indent);
      sbFmt(sb, "h%d.call()\n", o->c);
      sbIndent(sb, indent);
      sbFmt(sb, "h%d.call()\n", o->f);
      break;
    }
    default:
      break;
  }
}

static void renderStream(Sb *sb, const UpPlan *plan, int stream, int indent) {
  for (int i = 0; i < plan->opCount; i++) {
    const UpOp *o = &plan->ops[i];
    if (o->stream != stream) continue;
    renderOp(sb, plan, stream, o, indent);
  }
}

char *upRenderProgram(const UpPlan *plan) {
  memset(nameOverride, 0, sizeof(nameOverride));
  Sb sb;
  sbInit(&sb);
  if (!sb.data) return NULL;

  sbPut(&sb, "// Generated by upvalue_plan.c; do not edit.\n");
  sbPut(&sb, "var Log = []\n");
  sbFmt(&sb, "var EV_STRIDE = %d\n", UP_DECL_STRIDE);
  sbFmt(&sb, "var EV_OFFSET = %d\n", UP_VALUE_OFFSET);
  sbPut(&sb,
        "class UpTrace {\n"
        "  static ev(id, tag, value) {\n"
        "    Log.add(((value + EV_OFFSET) * EV_STRIDE + id) * 8 + tag)\n"
        "  }\n"
        "}\n");

  for (int i = 0; i < plan->declCount; i++) {
    const UpDecl *d = &plan->decls[i];
    if (d->kind == UP_DECL_CLOSURE || d->kind == UP_DECL_FIBER ||
        d->kind == UP_DECL_WRAP) {
      sbFmt(&sb, "var h%d = null\n", d->id);
    }
  }

  sbPut(&sb, "var Main = Fiber.new {\n");
  renderStream(&sb, plan, 0, 1);
  sbPut(&sb, "  return 0\n");
  sbPut(&sb, "}\n");
  return sb.data;
}

// ---------------------------------------------------------------------------
// Encoder (inverse of the decoder)
// ---------------------------------------------------------------------------
static void putU8(uint8_t **p, int v) { *(*p)++ = (uint8_t)v; }
static void putU16(uint8_t **p, int v) {
  *(*p)++ = (uint8_t)((v >> 8) & 0xff);
  *(*p)++ = (uint8_t)(v & 0xff);
}
static void putS16(uint8_t **p, int v) { putU16(p, (uint16_t)v); }

size_t upEncodePlan(const UpPlan *plan, uint8_t *out, size_t outSize) {
  uint8_t *start = out;
  uint8_t *p = out;
  if (outSize < 3) return 0;
  *p++ = UP_MAGIC;
  *p++ = UP_VERSION;

  for (int i = 0; i < plan->opCount; i++) {
    const UpOp *o = &plan->ops[i];
    if ((size_t)(p - start) + 40 > outSize) return 0;
    switch (o->code) {
      case UP_SCOPE_ENTER:
      case UP_SCOPE_EXIT:
      case UP_GC:
      case UP_FIBER_END:
        putU8(&p, o->code);
        break;
      case UP_VAR: {
        UpDecl *d = upFindDecl((UpPlan *)plan, o->a);
        putU8(&p, UP_VAR);
        putU16(&p, o->a);
        putS16(&p, d->initValue);
        break;
      }
      case UP_SET:
        putU8(&p, UP_SET);
        putU16(&p, o->a);
        putS16(&p, o->b);
        break;
      case UP_CLOSURE: {
        UpDecl *d = upFindDecl((UpPlan *)plan, o->a);
        putU8(&p, UP_CLOSURE);
        putU16(&p, o->a);
        putU8(&p, d->capCount);
        for (int k = 0; k < d->capCount; k++) {
          putU16(&p, d->caps[k].varId);
          putU8(&p, d->caps[k].mode);
          putS16(&p, d->caps[k].writeConst);
        }
        break;
      }
      case UP_ALIAS:
        putU8(&p, UP_ALIAS);
        putU16(&p, o->a);
        putU16(&p, o->b);
        break;
      case UP_WRAP_BEGIN:
        putU8(&p, UP_WRAP_BEGIN);
        putU16(&p, o->a);
        break;
      case UP_WRAP_END:
        putU8(&p, UP_WRAP_END);
        putU16(&p, o->a);
        putU16(&p, o->b);
        break;
      case UP_CALL:
      case UP_DROP:
      case UP_RESUME:
        putU8(&p, o->code);
        putU16(&p, o->a);
        break;
      case UP_FIBER_BEGIN:
        putU8(&p, UP_FIBER_BEGIN);
        putU16(&p, o->a);
        break;
      case UP_YIELD:
        putU8(&p, UP_YIELD);
        putS16(&p, o->a);
        break;
      case UP_DEPTH:
        putU8(&p, UP_DEPTH);
        putU8(&p, o->a);
        break;
      case UP_SHADOW:
        putU8(&p, UP_SHADOW);
        putU16(&p, o->a);
        putS16(&p, o->b);
        putU16(&p, o->c);
        putS16(&p, o->d);
        putU16(&p, o->e);
        break;
      case UP_SLOTREUSE:
        putU8(&p, UP_SLOTREUSE);
        putU16(&p, o->a);
        putS16(&p, o->b);
        putU16(&p, o->c);
        putU16(&p, o->d);
        putS16(&p, o->e);
        putU16(&p, o->f);
        break;
      default:
        break;
    }
  }
  putU8(&p, UP_OP_END);
  return (size_t)(p - start);
}

// ---------------------------------------------------------------------------
// Plan builder: the single construction API used by the fixed corpus and the
// randomized generator, so generated plans are valid by construction.
// ---------------------------------------------------------------------------
typedef struct {
  UpPlan plan;
  int streamStack[UP_MAX_NESTING + 2];
  int streamTop;
  int nextId;
  // Ids of variables that are still lexically in scope at the builder's
  // current position. Entering a block/fiber/wrapper pushes a mark; leaving it
  // truncates the list back to that mark. This is what keeps every generated
  // closure capture legal: a closure can only name a live variable.
  int activeVars[UP_MAX_DECLS];
  int activeCount;
  int activeMarks[UP_MAX_NESTING * 2 + UP_MAX_SCOPES + 8];
  int markTop;
} Builder;

static void bPushMark(Builder *b) {
  b->activeMarks[b->markTop++] = b->activeCount;
}

static void bPopMark(Builder *b) {
  b->activeCount = b->activeMarks[--b->markTop];
}

static void bInit(Builder *b) {
  memset(b, 0, sizeof(*b));
  b->plan.streams[0].parent = -1;
  b->plan.streams[0].firstOp = 0;
  b->plan.streams[0].lastOp = 0;
  b->plan.streamCount = 1;
  b->streamStack[b->streamTop++] = 0;
  b->nextId = 1;
}

static int bStream(Builder *b) { return b->streamStack[b->streamTop - 1]; }

static int bAddOp(Builder *b, uint8_t code) {
  if (b->plan.opCount >= UP_MAX_OPS) return 0;
  UpOp *o = &b->plan.ops[b->plan.opCount++];
  memset(o, 0, sizeof(*o));
  o->code = code;
  o->stream = bStream(b);
  return 1;
}

static int bAddDecl(Builder *b, UpDeclKind kind, int id) {
  if (b->plan.declCount >= UP_MAX_DECLS) return 0;
  UpDecl *d = &b->plan.decls[b->plan.declCount++];
  memset(d, 0, sizeof(*d));
  d->kind = kind;
  d->id = id;
  d->stream = bStream(b);
  d->closureObject = -1;
  return 1;
}

static int bAllocId(Builder *b) {
  if (b->nextId >= UP_MAX_DECLS - 8) return -1;
  return b->nextId++;
}

static int bVar(Builder *b, int init) {
  int id = bAllocId(b);
  if (id < 0 || !bAddDecl(b, UP_DECL_VAR, id)) return -1;
  b->plan.decls[b->plan.declCount - 1].initValue = init;
  if (!bAddOp(b, UP_VAR)) return -1;
  b->plan.ops[b->plan.opCount - 1].a = id;
  b->activeVars[b->activeCount++] = id;
  return id;
}

static int bSet(Builder *b, int id, int value) {
  if (!bAddOp(b, UP_SET)) return 0;
  b->plan.ops[b->plan.opCount - 1].a = id;
  b->plan.ops[b->plan.opCount - 1].b = value;
  return 1;
}

static int bScopeEnter(Builder *b) {
  bPushMark(b);
  return bAddOp(b, UP_SCOPE_ENTER);
}
static int bScopeExit(Builder *b) {
  bPopMark(b);
  return bAddOp(b, UP_SCOPE_EXIT);
}
static int bGC(Builder *b) { return bAddOp(b, UP_GC); }

static int bClosure(Builder *b, const UpCapture *caps, int ncap) {
  int id = bAllocId(b);
  if (id < 0 || !bAddDecl(b, UP_DECL_CLOSURE, id)) return -1;
  UpDecl *d = &b->plan.decls[b->plan.declCount - 1];
  d->closureObject = b->plan.declCount - 1;
  d->capCount = ncap;
  for (int i = 0; i < ncap; i++) d->caps[i] = caps[i];
  if (!bAddOp(b, UP_CLOSURE)) return -1;
  b->plan.ops[b->plan.opCount - 1].a = id;
  return id;
}

static int bAlias(Builder *b, int sourceId) {
  int id = bAllocId(b);
  if (id < 0 || !bAddDecl(b, UP_DECL_CLOSURE, id)) return -1;
  if (!bAddOp(b, UP_ALIAS)) return -1;
  b->plan.ops[b->plan.opCount - 1].a = id;
  b->plan.ops[b->plan.opCount - 1].b = sourceId;
  return id;
}

static int bCall(Builder *b, int closureId) {
  if (!bAddOp(b, UP_CALL)) return 0;
  b->plan.ops[b->plan.opCount - 1].a = closureId;
  return 1;
}

static int bDrop(Builder *b, int id) {
  if (!bAddOp(b, UP_DROP)) return 0;
  b->plan.ops[b->plan.opCount - 1].a = id;
  return 1;
}

static int bBeginFiber(Builder *b) {
  int id = bAllocId(b);
  if (id < 0 || b->plan.streamCount >= UP_MAX_STREAMS) return -1;
  if (!bAddDecl(b, UP_DECL_FIBER, id)) return -1;
  int s = b->plan.streamCount++;
  b->plan.streams[s].parent = bStream(b);
  b->plan.streams[s].firstOp = b->plan.opCount;
  b->plan.streams[s].lastOp = b->plan.opCount;
  b->plan.decls[b->plan.declCount - 1].fiberBodyStream = s;
  if (!bAddOp(b, UP_FIBER_BEGIN)) return -1;
  b->plan.ops[b->plan.opCount - 1].a = id;
  bPushMark(b); // body-local vars are scoped to this stream
  b->streamStack[b->streamTop++] = s;
  return id;
}

static int bEndFiber(Builder *b) {
  int s = b->streamStack[--b->streamTop];
  bPopMark(b);
  b->plan.streams[s].lastOp = b->plan.opCount;
  return bAddOp(b, UP_FIBER_END);
}

static int bResume(Builder *b, int fiberId) {
  if (!bAddOp(b, UP_RESUME)) return 0;
  b->plan.ops[b->plan.opCount - 1].a = fiberId;
  return 1;
}

static int bYield(Builder *b, int value) {
  if (!bAddOp(b, UP_YIELD)) return 0;
  b->plan.ops[b->plan.opCount - 1].a = value;
  return 1;
}

static int bDepth(Builder *b, int depth) {
  if (!bAddOp(b, UP_DEPTH)) return 0;
  b->plan.ops[b->plan.opCount - 1].a = depth;
  return 1;
}

static int bBeginWrap(Builder *b) {
  int id = bAllocId(b);
  if (id < 0 || b->plan.streamCount >= UP_MAX_STREAMS) return -1;
  if (!bAddDecl(b, UP_DECL_WRAP, id)) return -1;
  int s = b->plan.streamCount++;
  b->plan.streams[s].parent = bStream(b);
  b->plan.streams[s].firstOp = b->plan.opCount;
  b->plan.streams[s].lastOp = b->plan.opCount;
  b->plan.decls[b->plan.declCount - 1].fiberBodyStream = s;
  if (!bAddOp(b, UP_WRAP_BEGIN)) return -1;
  b->plan.ops[b->plan.opCount - 1].a = id;
  bPushMark(b); // wrapper-local vars are scoped to this stream
  b->streamStack[b->streamTop++] = s;
  return id;
}

static int bEndWrap(Builder *b, int wrapId, int resultClosureId) {
  int s = b->streamStack[--b->streamTop];
  bPopMark(b);
  b->plan.streams[s].lastOp = b->plan.opCount;
  if (!bAddOp(b, UP_WRAP_END)) return 0;
  b->plan.ops[b->plan.opCount - 1].a = wrapId;
  b->plan.ops[b->plan.opCount - 1].b = resultClosureId;
  return 1;
}

static int bShadow(Builder *b, int outerId, int outerInit,
                   int innerInit, int innerClosureId) {
  if (!bAddOp(b, UP_SHADOW)) return 0;
  UpOp *o = &b->plan.ops[b->plan.opCount - 1];
  int innerId = bAllocId(b);
  if (innerId < 0) return 0;
  o->a = outerId; o->b = outerInit; o->c = innerId;
  o->d = innerInit; o->e = innerClosureId;
  if (!bAddDecl(b, UP_DECL_VAR, outerId)) return 0;
  b->plan.decls[b->plan.declCount - 1].initValue = outerInit;
  if (!bAddDecl(b, UP_DECL_VAR, innerId)) return 0;
  b->plan.decls[b->plan.declCount - 1].initValue = innerInit;
  if (!bAddDecl(b, UP_DECL_CLOSURE, innerClosureId)) return 0;
  UpDecl *d = &b->plan.decls[b->plan.declCount - 1];
  d->closureObject = b->plan.declCount - 1;
  d->capCount = 1;
  d->caps[0].varId = innerId;
  d->caps[0].mode = UP_MODE_READ_WRITE;
  d->caps[0].writeConst = innerInit + 1;
  return 1;
}

static int bSlotReuse(Builder *b, int idA, int initA, int cloA,
                      int idB, int initB, int cloB) {
  if (!bAddOp(b, UP_SLOTREUSE)) return 0;
  UpOp *o = &b->plan.ops[b->plan.opCount - 1];
  o->a = idA; o->b = initA; o->c = cloA;
  o->d = idB; o->e = initB; o->f = cloB;
  int ids[2] = { idA, idB }, inits[2] = { initA, initB };
  int clos[2] = { cloA, cloB };
  for (int k = 0; k < 2; k++) {
    if (!bAddDecl(b, UP_DECL_VAR, ids[k])) return 0;
    b->plan.decls[b->plan.declCount - 1].initValue = inits[k];
    if (!bAddDecl(b, UP_DECL_CLOSURE, clos[k])) return 0;
    UpDecl *d = &b->plan.decls[b->plan.declCount - 1];
    d->closureObject = b->plan.declCount - 1;
    d->capCount = 1;
    d->caps[0].varId = ids[k];
    d->caps[0].mode = UP_MODE_READ_WRITE;
    d->caps[0].writeConst = inits[k] + 1;
  }
  return 1;
}

// ---------------------------------------------------------------------------
// Deterministic PRNG (splitMix64) and the structured generator
// ---------------------------------------------------------------------------
void upRngSeed(UpRng *rng, uint64_t seed) { rng->state = seed; }

uint32_t upRngNext(UpRng *rng) {
  uint64_t z = (rng->state += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  z ^= z >> 31;
  return (uint32_t)(z >> 32);
}

static int rngRange(UpRng *rng, int lo, int hi) {
  if (hi <= lo) return lo;
  return lo + (int)(upRngNext(rng) % (uint32_t)(hi - lo + 1));
}

// Picks a variable that is lexically live at the builder's current position.
// The active-variable stack already encodes scope and stream lifetime, so a
// chosen id is guaranteed to be a legal capture target.
static int pickVar(Builder *b, UpRng *rng) {
  if (b->activeCount == 0) return -1;
  return b->activeVars[upRngNext(rng) % (uint32_t)b->activeCount];
}

static int genClosure(Builder *b, UpRng *rng) {
  UpCapture caps[UP_MAX_CAPS];
  int n = 0;
  int tries = 3;
  while (tries-- > 0 && n < UP_MAX_CAPS) {
    int var = pickVar(b, rng);
    if (var < 0) break;
    int duplicate = 0;
    for (int i = 0; i < n; i++)
      if (caps[i].varId == var) duplicate = 1;
    if (duplicate) continue;
    caps[n].varId = var;
    caps[n].mode = 1 + (int)(upRngNext(rng) % 3);
    caps[n].writeConst = rngRange(rng, UP_MIN_VALUE / 2, UP_MAX_VALUE / 2);
    n++;
  }
  if (n == 0) return -1;
  return bClosure(b, caps, n);
}

static void genFiber(Builder *b, UpRng *rng, int withWrap, int withDepth) {
  int outerVars[2];
  outerVars[0] = bVar(b, rngRange(rng, 1, 50));
  outerVars[1] = bVar(b, rngRange(rng, 1, 50));

  int fiberId = bBeginFiber(b);
  if (fiberId < 0) return;

  // Closures over an outer cell: the upvalues stay open while suspended.
  UpCapture cap = { outerVars[0], UP_MODE_READ_WRITE, rngRange(rng, 1, 99) };
  int reader = bClosure(b, &cap, 1);
  UpCapture writerCaps[2] = {
    { outerVars[0], UP_MODE_WRITE, rngRange(rng, 1, 99) },
    { outerVars[1], UP_MODE_READ, 0 }
  };
  int writer = bClosure(b, writerCaps, 2);

  bCall(b, reader);
  bYield(b, rngRange(rng, 1, 999));
  bCall(b, writer);

  // A randomized extra closure over whichever cells are lexically visible.
  int extra = genClosure(b, rng);
  if (extra > 0) bCall(b, extra);

  if (withWrap) {
    // Two-level indirect capture through an intermediate function.
    int wrapId = bBeginWrap(b);
    int local = bVar(b, rngRange(rng, 1, 40));
    UpCapture innerCaps[2];
    innerCaps[0].varId = local;
    innerCaps[0].mode = UP_MODE_READ_WRITE;
    innerCaps[0].writeConst = rngRange(rng, 1, 40);
    innerCaps[1].varId = outerVars[0];
    innerCaps[1].mode = UP_MODE_READ;
    innerCaps[1].writeConst = 0;
    int inner = bClosure(b, innerCaps, 2);
    bCall(b, inner);
    bEndWrap(b, wrapId, inner);
    int handed = bAlias(b, wrapId);
    bCall(b, handed);
  }

  if (withDepth) bDepth(b, rngRange(rng, 60, UP_MAX_DEPTH));

  bYield(b, rngRange(rng, 1, 999));
  bCall(b, reader);
  bEndFiber(b);

  bResume(b, fiberId);
  bGC(b);
  bResume(b, fiberId);
  bGC(b);
  bResume(b, fiberId);
  if (upRngNext(rng) & 1) bDrop(b, fiberId);
  if (upRngNext(rng) & 1) bDrop(b, reader);
  if (writer > 0 && (upRngNext(rng) & 1)) bDrop(b, writer);
}

void upGeneratePlan(UpRng *rng, UpPlan *plan) {
  Builder b;
  bInit(&b);

  int sections = 2 + (int)(upRngNext(rng) % 3);
  for (int sec = 0; sec < sections && b.plan.opCount < UP_MAX_OPS - 60; sec++) {
    int v1 = bVar(&b, rngRange(rng, 1, 100));
    int v2 = bVar(&b, rngRange(rng, 1, 100));
    if (v1 < 0 || v2 < 0) break;

    // Two closures sharing one mutable cell, plus an alias of the first.
    UpCapture c1 = { v1, UP_MODE_READ_WRITE, rngRange(rng, 1, 99) };
    int a = bClosure(&b, &c1, 1);
    UpCapture c2[2] = {
      { v1, UP_MODE_READ, 0 },
      { v2, UP_MODE_WRITE, rngRange(rng, 1, 99) }
    };
    int c = bClosure(&b, c2, 2);
    int alias = bAlias(&b, a);
    bCall(&b, a);
    bCall(&b, alias);
    bSet(&b, v1, rngRange(rng, 1, 99));
    bCall(&b, c);
    bGC(&b);

    if (upRngNext(rng) % 3 == 0) {
      int outer = bAllocId(&b), innerClo = bAllocId(&b);
      if (outer > 0 && innerClo > 0)
        bShadow(&b, outer, rngRange(rng, 1, 9), rngRange(rng, 10, 19),
                innerClo);
    }
    if (upRngNext(rng) % 3 == 0) {
      int idA = bAllocId(&b), cloA = bAllocId(&b);
      int idB = bAllocId(&b), cloB = bAllocId(&b);
      if (idA > 0 && cloA > 0 && idB > 0 && cloB > 0)
        bSlotReuse(&b, idA, rngRange(rng, 1, 9), cloA,
                   idB, rngRange(rng, 10, 19), cloB);
    }

    // A closed capture: the local dies but the closure retains its cell.
    bScopeEnter(&b);
    int v3 = bVar(&b, rngRange(rng, 1, 30));
    UpCapture c3 = { v3, UP_MODE_READ_WRITE, rngRange(rng, 31, 60) };
    int closed = bClosure(&b, &c3, 1);
    bScopeExit(&b);
    bCall(&b, closed);

    genFiber(&b, rng, (upRngNext(rng) % 2 == 0), 1);
  }

  *plan = b.plan;
}

// ---------------------------------------------------------------------------
// Fixed corpus
// ---------------------------------------------------------------------------

// The combined plan: read/write closures on one cell, a suspended fiber with
// an open capture, two-level indirect capture, a closed capture after scope
// exit, shadowing and same-slot reuse.
static void buildMegaPlan(UpPlan *out) {
  Builder b;
  bInit(&b);

  int x = bVar(&b, 10);
  int y = bVar(&b, 20);

  UpCapture rw = { x, UP_MODE_READ_WRITE, 77 };
  int getSet = bClosure(&b, &rw, 1);
  UpCapture both[2] = { { x, UP_MODE_READ, 0 }, { y, UP_MODE_WRITE, 55 } };
  int observer = bClosure(&b, both, 2);
  int aliasGetSet = bAlias(&b, getSet);

  bCall(&b, getSet);
  bCall(&b, aliasGetSet);
  bSet(&b, x, 123);
  bCall(&b, observer);
  bGC(&b);

  int shadowOuter = bAllocId(&b), shadowClo = bAllocId(&b);
  bShadow(&b, shadowOuter, 1, 2, shadowClo);

  int slotA = bAllocId(&b), slotCloA = bAllocId(&b);
  int slotB = bAllocId(&b), slotCloB = bAllocId(&b);
  bSlotReuse(&b, slotA, 3, slotCloA, slotB, 4, slotCloB);

  bScopeEnter(&b);
  int gone = bVar(&b, 31);
  UpCapture capGone = { gone, UP_MODE_READ_WRITE, 41 };
  int closedClo = bClosure(&b, &capGone, 1);
  bScopeExit(&b);
  bCall(&b, closedClo);
  bGC(&b);

  int fiberId = bBeginFiber(&b);
  UpCapture fiberCap = { x, UP_MODE_READ_WRITE, 88 };
  int fiberReader = bClosure(&b, &fiberCap, 1);
  bCall(&b, fiberReader);
  bYield(&b, 500);

  int wrapId = bBeginWrap(&b);
  int wrapLocal = bVar(&b, 60);
  UpCapture innerCaps[2] = {
    { wrapLocal, UP_MODE_READ_WRITE, 70 },
    { x, UP_MODE_READ, 0 }
  };
  int inner = bClosure(&b, innerCaps, 2);
  bCall(&b, inner);
  bEndWrap(&b, wrapId, inner);
  int handed = bAlias(&b, wrapId);
  bCall(&b, handed);

  bDepth(&b, 120);
  bYield(&b, 501);
  bCall(&b, fiberReader);
  bEndFiber(&b);

  bResume(&b, fiberId);
  bGC(&b);
  bResume(&b, fiberId);
  bGC(&b);
  bResume(&b, fiberId);

  bDrop(&b, fiberId);
  bDrop(&b, getSet);
  bCall(&b, aliasGetSet);
  bCall(&b, handed);
  bCall(&b, closedClo);

  *out = b.plan;
}

static void buildSmallFiberPlan(UpPlan *out) {
  Builder b;
  bInit(&b);
  int x = bVar(&b, 7);
  int f = bBeginFiber(&b);
  UpCapture cap = { x, UP_MODE_READ_WRITE, 9 };
  int clo = bClosure(&b, &cap, 1);
  bCall(&b, clo);
  bYield(&b, 100);
  bCall(&b, clo);
  bDepth(&b, 80);
  bYield(&b, 101);
  bEndFiber(&b);
  bResume(&b, f);
  bGC(&b);
  bResume(&b, f);
  bResume(&b, f);
  *out = b.plan;
}

static void buildAliasPlan(UpPlan *out) {
  Builder b;
  bInit(&b);
  int x = bVar(&b, 5);
  UpCapture cap = { x, UP_MODE_READ_WRITE, 6 };
  int a = bClosure(&b, &cap, 1);
  int a2 = bAlias(&b, a);
  int a3 = bAlias(&b, a2);
  bCall(&b, a);
  bCall(&b, a2);
  bCall(&b, a3);
  bDrop(&b, a);
  bCall(&b, a3);
  *out = b.plan;
}

// Two distinct closures capture the SAME local variable (one stack slot). The
// first closure writes the cell; the second is a pure reader. After the block
// closes, the upvalues must remain one shared cell, so the reader observes the
// writer's new value. Disabling same-slot upvalue reuse gives the reader its
// own closed copy holding the stale value, so the post-close read diverges.
// This is the dedicated regression for captureUpvalue reuse.
static void buildSharedCellPlan(UpPlan *out) {
  Builder b;
  bInit(&b);
  bScopeEnter(&b);
  int shared = bVar(&b, 70);
  UpCapture capA = { shared, UP_MODE_READ_WRITE, 71 };
  int writer = bClosure(&b, &capA, 1);
  UpCapture capB = { shared, UP_MODE_READ, 0 };
  int reader = bClosure(&b, &capB, 1);
  bScopeExit(&b);
  // Both closures outlive the block. The writer sets 71; the reader must see
  // 71 through the shared cell. With reuse broken it reads the stale 70.
  bCall(&b, writer);
  bCall(&b, reader);
  bCall(&b, writer);
  bCall(&b, reader);
  // Force a host boundary after the calls so their events are compared.
  bGC(&b);
  *out = b.plan;
}

int upFixedPlans(UpPlan *out, int capacity) {
  int n = 0;
  if (capacity < 4) return 0;
  buildMegaPlan(&out[n++]);
  buildSmallFiberPlan(&out[n++]);
  buildAliasPlan(&out[n++]);
  buildSharedCellPlan(&out[n++]);
  return n;
}
