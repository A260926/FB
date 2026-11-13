#ifndef TASK_SUPPORT_TASK_ENC_H
#define TASK_SUPPORT_TASK_ENC_H
/* Scalar-sequence <-> code-unit encoding, with per-width boundary tables.
 * Each table maps every code-unit boundary back to the scalar index that
 * starts there, so an engine offset that lands inside a code point is
 * detectable rather than silently reinterpreted. */
#include <stdint.h>
#include <stddef.h>
#include "task_ast.h"   /* TASK_MAX_SUBJ */

/* A scalar string plus its rendered code-unit forms. All arrays are sized for
 * up to TASK_MAX_SUBJ scalars; UTF-8 needs at most 4 units/scalar, UTF-16 at
 * most 2, UTF-32 exactly 1. */
#define TASK_MAX_U8  (TASK_MAX_SUBJ * 4)
#define TASK_MAX_U16 (TASK_MAX_SUBJ * 2)
#define TASK_MAX_U32 (TASK_MAX_SUBJ)

typedef struct {
    /* scalar view */
    uint32_t scalar[TASK_MAX_SUBJ + 1];
    int nscalar;
    /* code-unit views (NUL terminators included in *_len as stored length) */
    uint8_t  u8 [TASK_MAX_U8  + 1];
    uint16_t u16[TASK_MAX_U16 + 1];
    uint32_t u32[TASK_MAX_U32 + 1];
    int u8_len, u16_len, u32_len;   /* length excluding terminator */
    /* boundary[k] = scalar index at code-unit boundary k; length is units+1 */
    int b8 [TASK_MAX_U8  + 2];
    int b16[TASK_MAX_U16 + 2];
    int b32[TASK_MAX_U32 + 2];
} TaskText;

/* Encode n scalars into all three widths and build the boundary tables.
 * Returns 0 on success, -1 on a non-scalar input. */
int task_text_encode(TaskText *t, const uint32_t *scalars, int n);

/* Number of code units used to encode one scalar in each width. */
int task_utf8_len(uint32_t cp);
int task_utf16_len(uint32_t cp);

/* Render one scalar into a pattern buffer as PCRE source. In UTF mode a raw
 * scalar is valid in all widths, but supplementary scalars cannot be raw in
 * UTF-16 source (no surrogate encoding), so every non-ASCII scalar is emitted
 * as \x{...}, which all widths accept. ASCII metacharacters in the pool are
 * none, but escaping is centralized here anyway. */
int task_emit_scalar_pattern(uint32_t cp, uint32_t *out, int outcap);

/* Map a code-unit offset to a scalar index through a boundary table.
 * Returns the scalar index, or -1 if the offset lands inside a code point. */
int task_map_offset_8(const TaskText *t, long offset);
int task_map_offset_16(const TaskText *t, long offset);
int task_map_offset_32(const TaskText *t, long offset);
#endif
