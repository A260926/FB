/*
 * YARA scan adapter: compiles rendered rules and scans in-memory inputs via
 * the public API, then compares every published chain-head match (offset and
 * match_length) against the independent oracle. The oracle never links into
 * YARA; this translation unit is the only one that includes <yara.h>.
 */

#include "hex_scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yara.h>

typedef struct {
  hm_match matches[HM_MAX_DATA];
  int count;
  int scan_error;
} collector;

static int scan_callback(YR_SCAN_CONTEXT* ctx, int message, void* message_data,
                         void* user_data) {
  collector* col = user_data;

  if (message == CALLBACK_MSG_TOO_MANY_MATCHES) {
    col->scan_error = ERROR_TOO_MANY_MATCHES;
    return CALLBACK_ABORT;
  }

  if (message != CALLBACK_MSG_RULE_MATCHING) return CALLBACK_CONTINUE;

  YR_RULE* rule = message_data;
  YR_STRING* string;
  YR_MATCH* match;

  // Only the head of a chain publishes user-visible matches; the chain parts
  // are private. This mirrors how a rule consumer observes hex matches.
  yr_rule_strings_foreach(rule, string) {
    if (string->chained_to != NULL) continue;
    yr_string_matches_foreach(ctx, string, match) {
      if (col->count >= HM_MAX_DATA) {
        col->scan_error = ERROR_TOO_MANY_MATCHES;
        return CALLBACK_ABORT;
      }
      col->matches[col->count].start = (int) (match->base + match->offset);
      col->matches[col->count].length = match->match_length;
      col->count++;
    }
  }
  return CALLBACK_CONTINUE;
}

static int match_cmp(const void* a, const void* b) {
  const hm_match* x = a;
  const hm_match* y = b;
  if (x->start != y->start) return (x->start < y->start) ? -1 : 1;
  if (x->length != y->length) return (x->length < y->length) ? -1 : 1;
  return 0;
}

static void print_hex(const uint8_t* data, int len) {
  for (int i = 0; i < len; i++) fprintf(stderr, "%02X", data[i]);
}

static void report_failure(const hm_case* c, int threshold,
                           const hm_scan_result* res, int first_diff) {
  char dump[HM_DUMP_MAX];
  hm_dump(&c->pattern, dump, sizeof(dump));

  fprintf(stderr, "\n==== HEX MODEL MISMATCH (replay-ready) ====\n");
  fprintf(stderr, "case: %s\n", c->name);
  fprintf(stderr, "build YR_STRING_CHAINING_THRESHOLD=%d\n", threshold);
  fprintf(stderr, "%s", dump);
  fprintf(stderr, "rule text:\n%s", c->rule);
  fprintf(stderr, "input (%d bytes): ", c->data_len);
  print_hex(c->data, c->data_len);
  fprintf(stderr, "\n");
  fprintf(stderr, "oracle matches=%d, scanner matches=%d, chain_parts=%d\n",
          c->expected_count, res->observed_count, res->chain_parts);

  if (first_diff >= 0) {
    if (first_diff < c->expected_count && first_diff < res->observed_count) {
      fprintf(stderr,
              "first difference at index %d: oracle(start=%d len=%d) "
              "scanner(start=%d len=%d)\n",
              first_diff, c->expected[first_diff].start,
              c->expected[first_diff].length,
              res->observed[first_diff].start,
              res->observed[first_diff].length);
    } else if (first_diff < c->expected_count) {
      fprintf(stderr,
              "first difference at index %d: missing scanner match "
              "oracle(start=%d len=%d)\n",
              first_diff, c->expected[first_diff].start,
              c->expected[first_diff].length);
    } else {
      fprintf(stderr,
              "first difference at index %d: unexpected scanner match "
              "(start=%d len=%d)\n",
              first_diff, res->observed[first_diff].start,
              res->observed[first_diff].length);
    }
  }
  fprintf(stderr, "===========================================\n\n");
}

int hm_scan_init(void) {
  return yr_initialize();
}

int hm_build_threshold(void) {
  return YR_STRING_CHAINING_THRESHOLD;
}

void hm_scan_finalize(void) {
  yr_finalize();
}

bool hm_scan_case(const hm_case* c, hm_scan_result* res) {
  memset(res, 0, sizeof(*res));

  YR_COMPILER* compiler = NULL;
  YR_RULES* rules = NULL;

  if (yr_compiler_create(&compiler) != ERROR_SUCCESS) {
    res->compiler_error = -1;
    snprintf(res->compiler_message, sizeof(res->compiler_message),
             "yr_compiler_create failed");
    return false;
  }

  int errors = yr_compiler_add_string(compiler, c->rule, NULL);
  if (errors != 0) {
    res->compiler_error = errors;
    yr_compiler_get_error_message(compiler, res->compiler_message,
                                  sizeof(res->compiler_message));
    yr_compiler_destroy(compiler);
    return false;
  }

  if (yr_compiler_get_rules(compiler, &rules) != ERROR_SUCCESS) {
    res->compiler_error = -1;
    snprintf(res->compiler_message, sizeof(res->compiler_message),
             "yr_compiler_get_rules failed");
    yr_compiler_destroy(compiler);
    return false;
  }
  yr_compiler_destroy(compiler);

  // Chain-part count is path coverage only: it proves which representation
  // (single string vs split chain) the build produced, and never feeds the
  // oracle.
  YR_RULE* rule;
  YR_STRING* string;
  yr_rules_foreach(rules, rule) {
    yr_rule_strings_foreach(rule, string) {
      if (STRING_IS_CHAIN_PART(string)) res->chain_parts++;
    }
  }

  collector col;
  memset(&col, 0, sizeof(col));
  int rc = yr_rules_scan_mem(rules, c->data, (size_t) c->data_len, 0,
                             scan_callback, &col, 0);
  res->scan_error = (rc != ERROR_SUCCESS) ? rc : col.scan_error;

  if (res->scan_error == ERROR_SUCCESS) {
    res->observed_count = col.count;
    memcpy(res->observed, col.matches, sizeof(hm_match) * (size_t) col.count);
    qsort(res->observed, (size_t) col.count, sizeof(hm_match), match_cmp);
  }

  yr_rules_destroy(rules);
  return res->scan_error == ERROR_SUCCESS;
}

bool hm_compare(const hm_case* c, int threshold, const hm_scan_result* res) {
  if (res->scan_error != ERROR_SUCCESS) {
    fprintf(stderr, "scan error %d for case %s\n", res->scan_error, c->name);
    return false;
  }

  int first_diff = -1;
  int common = c->expected_count;
  if (res->observed_count < common) common = res->observed_count;

  for (int i = 0; i < common; i++) {
    if (c->expected[i].start != res->observed[i].start ||
        c->expected[i].length != res->observed[i].length) {
      first_diff = i;
      break;
    }
  }

  if (first_diff == -1 && c->expected_count != res->observed_count)
    first_diff = common;

  if (first_diff != -1) {
    report_failure(c, threshold, res, first_diff);
    return false;
  }
  return true;
}

bool hm_expect_rejected(const char* hex_body, int threshold) {
  char rule[HM_RULE_MAX];
  snprintf(rule, sizeof(rule),
           "rule hex_model_reject {\n strings:\n $h = %s\n"
           " condition:\n #h >= 0\n}\n",
           hex_body);

  YR_COMPILER* compiler = NULL;
  YR_RULES* rules = NULL;
  bool rejected;

  if (yr_compiler_create(&compiler) != ERROR_SUCCESS) return false;
  int errors = yr_compiler_add_string(compiler, rule, NULL);
  rejected = (errors != 0);

  if (!rejected && yr_compiler_get_rules(compiler, &rules) == ERROR_SUCCESS)
    yr_rules_destroy(rules);

  yr_compiler_destroy(compiler);

  if (!rejected) {
    fprintf(stderr,
            "expected compiler rejection at threshold=%d but rule compiled: "
            "%s\n",
            threshold, hex_body);
  }
  return rejected;
}
