#define PCRE2_CODE_UNIT_WIDTH 0
#include "pcre2.h"
#include "task_engine.h"
#include <string.h>

/* The rendered pattern source is pure ASCII (non-ASCII scalars are written as
 * \x{...}), so one source scalar is one code unit in every width. A non-ASCII
 * source scalar would be a renderer bug and is reported, not guessed. */
static int map8(const TaskText *t, long o)  { return task_map_offset_8(t, o); }
static int map16(const TaskText *t, long o) { return task_map_offset_16(t, o); }
static int map32(const TaskText *t, long o) { return task_map_offset_32(t, o); }

#define ENGINE_FOR_WIDTH(W)                                                      \
static TaskEngResult run##W(const uint32_t *pat, int patlen,                    \
                            const TaskText *text) {                             \
    PCRE2_UCHAR##W pbuf[TASK_MAX_PATLEN + 1];                                   \
    pcre2_code_##W *code;                                                        \
    pcre2_match_data_##W *md;                                                    \
    int e, i, rc;                                                                \
    PCRE2_SIZE eoff;                                                             \
    TaskEngResult out;                                                           \
    memset(&out, 0, sizeof(out));                                                \
    out.status = TASK_ENG_RUNTIME_ERROR;                                         \
    for (i = 0; i < patlen; i++) {                                               \
        if (pat[i] >= 0x80u) { out.status = TASK_ENG_COMPILE_ERROR;              \
                               out.errcode = -999; return out; }                 \
        pbuf[i] = (PCRE2_UCHAR##W)pat[i];                                        \
    }                                                                            \
    pbuf[patlen] = 0;                                                            \
    code = pcre2_compile_##W(pbuf, (PCRE2_SIZE)patlen, PCRE2_UTF,               \
                             &e, &eoff, NULL);                                   \
    if (!code) { out.status = TASK_ENG_COMPILE_ERROR; out.errcode = e;          \
                 return out; }                                                   \
    {                                                                            \
        uint32_t capcount = 0;                                                   \
        pcre2_pattern_info_##W(code, PCRE2_INFO_CAPTURECOUNT, &capcount);       \
        out.ncaps = (int)capcount;                                               \
    }                                                                            \
    md = pcre2_match_data_create_from_pattern_##W(code, NULL);                   \
    if (!md) { pcre2_code_free_##W(code); return out; }                          \
    rc = pcre2_match_##W(code,                                                   \
        (PCRE2_SPTR##W)(W == 8  ? (const void*)text->u8  :                       \
                        W == 16 ? (const void*)text->u16 :                       \
                                  (const void*)text->u32),                      \
        (PCRE2_SIZE)(W == 8 ? text->u8_len :                                     \
                     W == 16 ? text->u16_len : text->u32_len),                  \
        0, 0, md, NULL);                                                         \
    if (rc == PCRE2_ERROR_NOMATCH) {                                             \
        out.status = TASK_ENG_NOMATCH;                                           \
    } else if (rc < 0) {                                                         \
        out.status = TASK_ENG_RUNTIME_ERROR; out.errcode = rc;                   \
    } else {                                                                     \
        PCRE2_SIZE *ov = pcre2_get_ovector_pointer_##W(md);                     \
        uint32_t ovc = pcre2_get_ovector_count_##W(md);                         \
        int g;                                                                   \
        out.status = TASK_ENG_MATCH;                                             \
        for (g = 0; g < (int)ovc && g <= TASK_MAX_CAPS; g++) {                  \
            PCRE2_SIZE a = ov[2*g], b = ov[2*g+1];                              \
            int sa, sb;                                                          \
            if (a == PCRE2_UNSET || b == PCRE2_UNSET) {                         \
                out.cap[g].set = 0;                                              \
                out.cap[g].start = out.cap[g].end = TASK_CAP_UNSET;             \
                continue;                                                        \
            }                                                                    \
            sa = map##W(text, (long)a);                                          \
            sb = map##W(text, (long)b);                                          \
            if (sa < 0 || sb < 0) { out.status = TASK_ENG_BAD_BOUNDARY;          \
                                    out.errcode = g; break; }                    \
            out.cap[g].set = 1;                                                  \
            out.cap[g].start = sa; out.cap[g].end = sb;                          \
        }                                                                        \
        if (out.status == TASK_ENG_MATCH) {                                      \
            out.start = out.cap[0].start; out.end = out.cap[0].end;             \
        }                                                                        \
    }                                                                            \
    pcre2_match_data_free_##W(md);                                               \
    pcre2_code_free_##W(code);                                                   \
    return out;                                                                  \
}

ENGINE_FOR_WIDTH(8)
ENGINE_FOR_WIDTH(16)
ENGINE_FOR_WIDTH(32)

TaskEngResult task_engine_run(int width, const uint32_t *pattern, int patlen,
                              const TaskText *text) {
    if (width == 8)  return run8(pattern, patlen, text);
    if (width == 16) return run16(pattern, patlen, text);
    if (width == 32) return run32(pattern, patlen, text);
    {
        TaskEngResult bad;
        memset(&bad, 0, sizeof(bad));
        bad.status = TASK_ENG_RUNTIME_ERROR;
        bad.errcode = -998;
        return bad;
    }
}

#define NAME_FOR_WIDTH(W)                                                         \
static int name##W(const uint32_t *pat, int patlen, const char *name) {          \
    PCRE2_UCHAR##W pbuf[TASK_MAX_PATLEN + 1];                                    \
    PCRE2_UCHAR##W nbuf[16];                                                      \
    pcre2_code_##W *code;                                                         \
    int e, i, rc;                                                                 \
    PCRE2_SIZE eoff;                                                              \
    for (i = 0; i < patlen; i++) {                                                \
        if (pat[i] >= 0x80u) return -1;                                           \
        pbuf[i] = (PCRE2_UCHAR##W)pat[i];                                         \
    }                                                                             \
    pbuf[patlen] = 0;                                                             \
    for (i = 0; name[i] && i < (int)(sizeof(nbuf)/sizeof(nbuf[0])) - 1; i++)     \
        nbuf[i] = (PCRE2_UCHAR##W)(unsigned char)name[i];                        \
    nbuf[i] = 0;                                                                  \
    code = pcre2_compile_##W(pbuf, (PCRE2_SIZE)patlen, PCRE2_UTF,                \
                             &e, &eoff, NULL);                                    \
    if (!code) return -1;                                                         \
    rc = pcre2_substring_number_from_name_##W(code, nbuf);                       \
    pcre2_code_free_##W(code);                                                    \
    return rc < 0 ? -1 : rc;                                                      \
}

NAME_FOR_WIDTH(8)
NAME_FOR_WIDTH(16)
NAME_FOR_WIDTH(32)

int task_engine_name_resolves(int width, const uint32_t *pattern, int patlen,
                              const char *name) {
    if (width == 8)  return name8(pattern, patlen, name);
    if (width == 16) return name16(pattern, patlen, name);
    if (width == 32) return name32(pattern, patlen, name);
    return -1;
}
