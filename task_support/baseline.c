#include <yara.h>
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  const char *name;
  int count;
  int64_t offsets[8];
  int lengths[8];
} OBS;

static int callback(YR_SCAN_CONTEXT *ctx, int msg, void *data, void *user) {
  if (msg != CALLBACK_MSG_RULE_MATCHING) return CALLBACK_CONTINUE;
  OBS *o = user;
  YR_STRING *s;
  YR_MATCH *m;
  YR_RULE *rule = data;
  yr_rule_strings_foreach(rule, s) {
    if (s->chained_to != NULL) continue;
    yr_string_matches_foreach(ctx, s, m) {
      assert(o->count < 8);
      o->offsets[o->count] = m->base + m->offset;
      o->lengths[o->count] = m->match_length;
      o->count++;
    }
  }
  return CALLBACK_CONTINUE;
}

static void check(const char *name, const char *hex, const uint8_t *buf, size_t n,
                  int count, const int64_t *offsets, const int *lengths,
                  int expected_parts) {
  char text[2048];
  snprintf(text, sizeof(text), "rule test { strings: $h = %s condition: #h >= 0 }", hex);
  YR_COMPILER *c = NULL;
  YR_RULES *r = NULL;
  YR_RULE *rule;
  YR_STRING *s;
  int parts = 0;
  assert(yr_compiler_create(&c) == ERROR_SUCCESS);
  assert(yr_compiler_add_string(c, text, NULL) == 0);
  assert(yr_compiler_get_rules(c, &r) == ERROR_SUCCESS);
  yr_compiler_destroy(c);
  yr_rules_foreach(r, rule) {
    yr_rule_strings_foreach(rule, s) {
      if (STRING_IS_CHAIN_PART(s)) parts++;
    }
  }
  assert(parts == expected_parts);
  OBS o = {.name = name};
  assert(yr_rules_scan_mem(r, buf, n, 0, callback, &o, 0) == ERROR_SUCCESS);
  assert(o.count == count);
  for (int i=0; i<count; ++i) {
    assert(o.offsets[i] == offsets[i]);
    assert(o.lengths[i] == lengths[i]);
    printf("MATCH %s offset=%" PRId64 " length=%d\n",name,o.offsets[i],o.lengths[i]);
  }
  printf("CASE %s matches=%d chain_parts=%d\n", name, o.count, parts);
  yr_rules_destroy(r);
}

int main(void) {
  assert(yr_initialize() == ERROR_SUCCESS);
  const uint8_t shortbuf[] = {'A','A',0,'B','B'};
  const int64_t shortoffs[] = {0,1};
  const int shortlens[] = {4,4};
  check("overlap", "{ 41 [2-4] 42 }", shortbuf,sizeof(shortbuf),2,shortoffs,shortlens,
        YR_STRING_CHAINING_THRESHOLD < 4 ? 2 : 0);
  const uint8_t masked[] = {0x41,0x19,0x5a,0x42};
  const int64_t zero[] = {0};
  const int four[] = {4};
  check("masked_alt", "{ 41 (1? | ~?F) ?? 42 }",masked,sizeof(masked),1,zero,four,0);
  uint8_t chain[408];
  memset(chain,0,sizeof(chain));
  memcpy(chain,"AB",2); memcpy(chain+203,"CD",2); memcpy(chain+406,"EF",2);
  const int longlen[] = {408};
  check("native_chain", "{ 41 42 [201-202] 43 44 [201-202] 45 46 }",chain,sizeof(chain),1,zero,longlen,3);
  memset(chain,0,sizeof(chain));
  memcpy(chain,"AB",2); memcpy(chain+5,"CD",2); memcpy(chain+10,"EF",2);
  const int smalllen[] = {12};
  check("threshold_switch", "{ 41 42 [3-4] 43 44 [3-4] 45 46 }",chain,12,1,zero,smalllen,
        YR_STRING_CHAINING_THRESHOLD < 4 ? 3 : 0);
  assert(yr_finalize() == ERROR_SUCCESS);
  printf("YARA_HEX_BASELINE_OK threshold=%d cases=4\n",YR_STRING_CHAINING_THRESHOLD);
  return 0;
}
