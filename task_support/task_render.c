#include "task_render.h"
#include "task_enc.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint32_t *buf; int cap, len; } PatBuf;

static int pb_emit(PatBuf *b, uint32_t cp) {
    if (b->len >= b->cap) return -1;
    b->buf[b->len++] = cp;
    return 0;
}

static int pb_str(PatBuf *b, const char *s) {
    while (*s) if (pb_emit(b, (uint32_t)(unsigned char)*s++)) return -1;
    return 0;
}

static int pb_scalar(PatBuf *b, uint32_t cp) {
    int n = task_emit_scalar_pattern(cp, b->buf + b->len, b->cap - b->len);
    if (n < 0) return -1;
    b->len += n;
    return 0;
}

static int render(PatBuf *b, const TaskNode *n) {
    int i;
    switch (n->kind) {
    case TASK_LIT:
        return pb_scalar(b, n->cp);
    case TASK_CLASS:
        if (pb_str(b, n->neg ? "[^" : "[")) return -1;
        for (i = 0; i < n->nchars; i++)
            if (pb_scalar(b, n->chars[i])) return -1;
        return pb_str(b, "]");
    case TASK_CAT:
        for (i = 0; i < n->nch; i++)
            if (render(b, n->ch[i])) return -1;
        return 0;
    case TASK_ALT:
        if (pb_str(b, "(?:")) return -1;
        for (i = 0; i < n->nch; i++) {
            if (i && pb_str(b, "|")) return -1;
            if (render(b, n->ch[i])) return -1;
        }
        return pb_str(b, ")");
    case TASK_GROUP:
        if (pb_str(b, "(?:") || render(b, n->ch[0])) return -1;
        return pb_str(b, ")");
    case TASK_CAP:
        if (pb_str(b, "(")) return -1;
        if (n->name[0]) {
            char hdr[24];
            snprintf(hdr, sizeof(hdr), "?<%s>", n->name);
            if (pb_str(b, hdr)) return -1;
        }
        if (render(b, n->ch[0]) || pb_str(b, ")")) return -1;
        return 0;
    case TASK_REP: {
        char q[16];
        /* always parenthesize so quantifiers bind one atom at any depth */
        if (pb_str(b, "(?:") || render(b, n->ch[0]) || pb_str(b, ")"))
            return -1;
        if (n->rmin == n->rmax)
            snprintf(q, sizeof(q), "{%d}", n->rmin);
        else
            snprintf(q, sizeof(q), "{%d,%d}", n->rmin, n->rmax);
        return pb_str(b, q);
    }
    }
    return -1;
}

int task_render_pattern(const TaskNode *n, uint32_t *out, int outcap) {
    PatBuf b = { out, outcap, 0 };
    if (render(&b, n)) return -1;
    if (b.len >= outcap) return -1;      /* leave room for terminator */
    out[b.len] = 0;
    return b.len;
}

typedef struct { char *buf; int cap, len; } TxtBuf;

static int tb_put(TxtBuf *b, const char *s) {
    int n = (int)strlen(s);
    if (b->len + n >= b->cap) return -1;
    memcpy(b->buf + b->len, s, n + 1);
    b->len += n;
    return 0;
}

static int tb_fmt(TxtBuf *b, const char *fmt, ...) {
    va_list ap;
    int room = b->cap - b->len, n;
    va_start(ap, fmt);
    n = vsnprintf(b->buf + b->len, (size_t)room, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= room) return -1;
    b->len += n;
    return 0;
}

static int ser(TxtBuf *b, const TaskNode *n) {
    int i;
    switch (n->kind) {
    case TASK_LIT:
        return tb_fmt(b, "LIT(U+%04X)", n->cp);
    case TASK_CLASS:
        if (tb_put(b, n->neg ? "CLASS^[" : "CLASS[")) return -1;
        for (i = 0; i < n->nchars; i++)
            if (tb_fmt(b, "%sU+%04X", i ? "," : "", n->chars[i])) return -1;
        return tb_put(b, "]");
    case TASK_CAT:
        if (tb_put(b, "CAT(")) return -1;
        for (i = 0; i < n->nch; i++)
            if (ser(b, n->ch[i]) || (i + 1 < n->nch && tb_put(b, ","))) return -1;
        return tb_put(b, ")");
    case TASK_ALT:
        if (tb_put(b, "ALT(")) return -1;
        for (i = 0; i < n->nch; i++)
            if (ser(b, n->ch[i]) || (i + 1 < n->nch && tb_put(b, ","))) return -1;
        return tb_put(b, ")");
    case TASK_GROUP:
        if (tb_put(b, "GRP(") || ser(b, n->ch[0])) return -1;
        return tb_put(b, ")");
    case TASK_CAP:
        if (tb_fmt(b, "CAP%d%s(", n->capno,
                   n->name[0] ? n->name : "")) return -1;
        if (ser(b, n->ch[0])) return -1;
        return tb_put(b, ")");
    case TASK_REP:
        if (tb_fmt(b, "REP{%d,%d}(", n->rmin, n->rmax) ||
            ser(b, n->ch[0])) return -1;
        return tb_put(b, ")");
    }
    return -1;
}

int task_serialize_ast(const TaskNode *n, char *out, int outcap) {
    TxtBuf b = { out, outcap, 0 };
    if (outcap <= 0) return -1;
    out[0] = 0;
    if (ser(&b, n)) return -1;
    return b.len;
}

int task_serialize_scalars(const uint32_t *s, int n, char *out, int outcap) {
    TxtBuf b = { out, outcap, 0 };
    int i;
    if (outcap <= 0) return -1;
    out[0] = 0;
    for (i = 0; i < n; i++)
        if (tb_fmt(&b, "%sU+%04X", i ? " " : "", s[i])) return -1;
    return b.len;
}
