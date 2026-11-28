#define PCRE2_CODE_UNIT_WIDTH 8
#include "pcre2.h"

#include "task_engine.h"

#include <stdio.h>
#include <string.h>

/* Real library execution at the 8-bit width: PCRE2_UTF, interpreter only
 * (JIT is disabled in the build), compile errors are reported, never
 * swallowed. Returned code-unit offsets are mapped through a boundary
 * table built from the emitted UTF-8 bytes. */
LibResult lib8_run(const uint32_t *pat, int plen,
                   const uint32_t *text, int tlen) {
  LibResult r;
  memset(&r, 0, sizeof(r));
  r.width = 8;

  static PCRE2_UCHAR punits[8192];
  static uint8_t sunits[1024];
  int pu = enc_utf8(pat, plen, punits, (int)sizeof(punits) - 1);
  punits[pu] = 0;
  int su = enc_utf8(text, tlen, sunits, (int)sizeof(sunits) - 1);
  sunits[su] = 0;

  int ec;
  PCRE2_SIZE eo;
  pcre2_code *code = pcre2_compile(punits, (PCRE2_SIZE)pu, PCRE2_UTF, &ec,
                                   &eo, NULL);
  if (!code) {
    r.compile_error = 1;
    PCRE2_UCHAR buf[96];
    pcre2_get_error_message(ec, buf, sizeof(buf));
    snprintf(r.note, sizeof(r.note), "compile rc=%d at=%lu: %s", ec,
             (unsigned long)eo, buf);
    return r;
  }

  uint32_t capcount = 0;
  pcre2_pattern_info(code, PCRE2_INFO_CAPTURECOUNT, &capcount);
  r.ngroups = (int)capcount;

  pcre2_match_data *md = pcre2_match_data_create_from_pattern(code, NULL);
  int rc = pcre2_match(code, sunits, (PCRE2_SIZE)su, 0, 0, md, NULL);
  if (rc < 0) {
    if (rc == PCRE2_ERROR_NOMATCH) {
      r.matched = 0;
    } else {
      r.runtime_error = 1;
      PCRE2_UCHAR buf[96];
      pcre2_get_error_message(rc, buf, sizeof(buf));
      snprintf(r.note, sizeof(r.note), "match rc=%d: %s", rc, buf);
    }
    pcre2_match_data_free(md);
    pcre2_code_free(code);
    for (int i = 0; i < (TASK_MAX_GROUPS + 1) * 2; i++) r.ovec[i] = SIZE_MAX;
    return r;
  }

  PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
  uint8_t table[1025];
  bound_utf8(sunits, su, table);
  int pairs = rc;
  for (int i = 0; i < (TASK_MAX_GROUPS + 1) * 2; i++) r.ovec[i] = SIZE_MAX;
  for (int i = 0; i < pairs * 2 && i < (TASK_MAX_GROUPS + 1) * 2; i++) {
    if (ov[i] == PCRE2_UNSET) {
      r.ovec[i] = SIZE_MAX;
    } else {
      int sc = map_offset(table, su, ov[i]);
      if (sc < 0) r.boundary_violation = 1;
      r.ovec[i] = sc < 0 ? SIZE_MAX : (size_t)sc;
    }
  }
  r.matched = 1;
  pcre2_match_data_free(md);
  pcre2_code_free(code);
  return r;
}
