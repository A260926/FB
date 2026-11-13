#include "task_enc.h"
#include <stdio.h>

int task_utf8_len(uint32_t cp) {
    if (cp < 0x80u) return 1;
    if (cp < 0x800u) return 2;
    if (cp < 0x10000u) return 3;
    return 4;
}

int task_utf16_len(uint32_t cp) {
    return cp < 0x10000u ? 1 : 2;
}

static int encode_utf8(uint32_t cp, uint8_t *out) {
    if (cp < 0x80u) { out[0] = (uint8_t)cp; return 1; }
    if (cp < 0x800u) {
        out[0] = (uint8_t)(0xC0u | (cp >> 6));
        out[1] = (uint8_t)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (uint8_t)(0xE0u | (cp >> 12));
        out[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (uint8_t)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (uint8_t)(0xF0u | (cp >> 18));
    out[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (uint8_t)(0x80u | (cp & 0x3Fu));
    return 4;
}

int task_text_encode(TaskText *t, const uint32_t *scalars, int n) {
    int i, k8 = 0, k16 = 0, k32 = 0, s;
    for (i = 0; i < n; i++) {
        uint32_t cp = scalars[i];
        int u8n, u16n;
        if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return -1;
        t->scalar[i] = cp;

        u8n = encode_utf8(cp, &t->u8[k8]);
        t->b8[k8] = i;                 /* leading unit marks the boundary */
        for (s = 1; s < u8n; s++) t->b8[k8 + s] = -1;
        k8 += u8n;

        if (cp < 0x10000u) {
            t->u16[k16] = (uint16_t)cp;
            t->b16[k16] = i;
            u16n = 1;
        } else {
            uint32_t v = cp - 0x10000u;
            t->u16[k16]     = (uint16_t)(0xD800u + (v >> 10));
            t->u16[k16 + 1] = (uint16_t)(0xDC00u + (v & 0x3FFu));
            t->b16[k16] = i;
            t->b16[k16 + 1] = -1;      /* low surrogate is interior */
            u16n = 2;
        }
        k16 += u16n;

        t->u32[k32] = cp;
        t->b32[k32] = i;
        k32 += 1;
    }
    t->nscalar = n;
    t->u8_len = k8;  t->u16_len = k16;  t->u32_len = k32;
    t->u8[k8] = 0;    t->u16[k16] = 0;   t->u32[k32] = 0;
    /* end-of-text boundary: every width converges on scalar index n */
    t->b8[k8] = t->b16[k16] = t->b32[k32] = n;
    return 0;
}

int task_emit_scalar_pattern(uint32_t cp, uint32_t *out, int outcap) {
    char buf[16];
    int n, i;
    if (cp < 0x80u) {
        if (outcap < 1) return -1;
        out[0] = cp;
        return 1;
    }
    n = snprintf(buf, sizeof(buf), "\\x{%x}", (unsigned)cp);
    if (n <= 0 || n >= (int)sizeof(buf) || n > outcap) return -1;
    for (i = 0; i < n; i++) out[i] = (uint32_t)(unsigned char)buf[i];
    return n;
}

int task_map_offset_8(const TaskText *t, long offset) {
    if (offset < 0 || offset > t->u8_len) return -1;
    return t->b8[offset];
}
int task_map_offset_16(const TaskText *t, long offset) {
    if (offset < 0 || offset > t->u16_len) return -1;
    return t->b16[offset];
}
int task_map_offset_32(const TaskText *t, long offset) {
    if (offset < 0 || offset > t->u32_len) return -1;
    return t->b32[offset];
}
