#define PCRE2_CODE_UNIT_WIDTH 16
#include "pcre2.h"

#include "task_engine.h"

#include <stdio.h>
#include <string.h>

/* Real library execution at the 16-bit width. Supplementary scalars become
 * surrogate pairs; the independently built boundary table flags the low
 * surrogate unit offset as inside a code point. */
LibResult lib16_run(const uint32_t *pat, int plen,
                    const uint32_t *text, int tlen) {
  LibResult r;
  memset(&r, 0, sizeof(r));
  r.width = 16;

  static PCRE2_UCHAR punits[8192];
  static uint16_t sunits[512];
  int pu = enc_utf16(pat, plen, punits, (int)(sizeof(punits) / 2) - 1);
  punits[pu] = 0;
  int su = enc_utf16(text, tlen, sunits, (int)(sizeof(sunits) / 2) - 1);
  sunits[su] = 0;

  int ec;
  PCRE2_SIZE eo;
  pcre2_code *code = pcre2_compile(punits, (PCRE2_SIZE)pu, PCRE2_UTF, &ec,
                                   &eo, NULL);
  if (!code) {
    r.compile_error = 1;
    PCRE2_UCHAR buf[96];
    pcre2_get_error_message(ec, buf, sizeof(buf) / sizeof(buf[0]));
    snprintf(r.note, sizeof(r.note), "compile rc=%d at=%lu", ec,
             (unsigned long)eo);
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
      pcre2_get_error_message(rc, buf, sizeof(buf) / sizeof(buf[0]));
      snprintf(r.note, sizeof(r.note), "match rc=%d", rc);
    }
    pcre2_match_data_free(md);
    pcre2_code_free(code);
    for (int i = 0; i < (TASK_MAX_GROUPS + 1) * 2; i++) r.ovec[i] = SIZE_MAX;
    return r;
  }

  PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
  uint8_t table[513];
  bound_utf16(sunits, su, table);
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
