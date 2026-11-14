/*
 * Emits the STRING_IS_CHAIN_PART fragment count for the two hand-picked
 * two-jump patterns. Used only by tests/hex_model/check.sh to prove which
 * chaining representation each build produced; it performs no matching.
 */

#include <stdio.h>

#include <yara.h>

static void print_parts(const char* tag, const char* hex) {
  char text[256];
  snprintf(text, sizeof(text),
           "rule t { strings: $h = %s condition: #h >= 0 }", hex);

  YR_COMPILER* compiler = NULL;
  YR_RULES* rules = NULL;
  if (yr_compiler_create(&compiler) != ERROR_SUCCESS) return;
  if (yr_compiler_add_string(compiler, text, NULL) != 0) {
    yr_compiler_destroy(compiler);
    return;
  }
  if (yr_compiler_get_rules(compiler, &rules) != ERROR_SUCCESS) {
    yr_compiler_destroy(compiler);
    return;
  }
  yr_compiler_destroy(compiler);

  int parts = 0;
  YR_RULE* rule;
  YR_STRING* string;
  yr_rules_foreach(rules, rule) {
    yr_rule_strings_foreach(rule, string) {
      if (STRING_IS_CHAIN_PART(string)) parts++;
    }
  }

  printf("CHAIN_PROBE %s threshold=%d parts=%d\n", tag,
         YR_STRING_CHAINING_THRESHOLD, parts);
  yr_rules_destroy(rules);
}

int main(void) {
  if (yr_initialize() != ERROR_SUCCESS) return 1;
  // Two short jumps: one string at the default threshold, three fragments
  // once the threshold drops to 2.
  print_parts("two_short_jumps", "{ 41 42 [3-4] 43 44 [3-4] 45 46 }");
  // Two long jumps: three fragments in every build.
  print_parts("two_long_jumps",
              "{ 41 42 [201-202] 43 44 [201-202] 45 46 }");
  yr_finalize();
  return 0;
}
