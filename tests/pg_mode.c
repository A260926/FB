/*
 * PostgreSQL mode tests for libinjection.
 *
 * Covers the FLAG_SQL_PG / FLAG_PG_LEGACY_STRINGS lexer:
 *   - raw token type, source position, length and open/close flags
 *   - folding and the fingerprint pipeline
 *   - both decision entries (libinjection_is_sqli and libinjection_sqli_pg)
 *   - mode alternation, state reuse, lookup callback retention
 *   - ordinary vs simulated outer single-quote contexts
 *   - slice boundaries, embedded NUL bytes and long token contents
 *
 * Exits non-zero on any failed check, and refuses to pass if no checks
 * ran.  Built and driven by tests/run_pg_mode.sh.
 */
#include "libinjection.h"
#include "libinjection_sqli.h"

#include <stdio.h>
#include <string.h>

#define MAX_RAW_TOKENS 32

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        g_checks += 1;                                                        \
        if (!(cond)) {                                                        \
            g_failures += 1;                                                  \
            printf("not ok - %s (line %d)\n", (msg), __LINE__);              \
        }                                                                     \
    } while (0)

/*
 * Stream every raw token out of the state into `out`.  tokenize() keeps
 * writing into the same state slot, so each token is copied before the
 * next call.
 */
static int collect_tokens(const char *s, size_t len, int flags,
                          stoken_t *out, int cap) {
    struct libinjection_sqli_state sf;
    int n = 0;
    libinjection_sqli_init(&sf, s, len, flags);
    while (n < cap && libinjection_sqli_tokenize(&sf) == 1) {
        memcpy(&out[n], sf.current, sizeof(stoken_t));
        n += 1;
    }
    return n;
}

static void expect_token(const stoken_t *t, char type, size_t pos, size_t len,
                         char open, char close, const char *msg) {
    char buf[160];
    snprintf(buf, sizeof(buf),
             "%s: got type=%c pos=%zu len=%zu open=%d close=%d", msg, t->type,
             t->pos, t->len, t->str_open, t->str_close);
    CHECK(t->type == type, buf);
    CHECK(t->pos == pos, buf);
    CHECK(t->len == len, buf);
    CHECK(t->str_open == open, buf);
    CHECK(t->str_close == close, buf);
}

static int g_callback_calls = 0;
static void *g_callback_userdata = (void *)0;
static ptr_lookup_fn g_wrapped_lookup = (ptr_lookup_fn)0;

static char counting_lookup(struct libinjection_sqli_state *sf, int lookuptype,
                            const char *word, size_t len) {
    g_callback_calls += 1;
    g_callback_userdata = sf->userdata;
    return g_wrapped_lookup(sf, lookuptype, word, len);
}

static void test_nested_comments(void) {
    const char *q = "SELECT /* outer /* inner */ tail */ 7";
    size_t qlen = strlen(q);
    stoken_t toks[MAX_RAW_TOKENS];
    int n = collect_tokens(q, qlen, FLAG_SQL_PG | FLAG_QUOTE_NONE, toks,
                           MAX_RAW_TOKENS);

    /* PG: SELECT, one whole comment, then 7 -- no manufactured X */
    CHECK(n == 3, "nested comment yields three tokens");
    expect_token(&toks[0], 'E', 0, 6, 0, 0, "nested select");
    expect_token(&toks[1], 'c', 7, 28, 0, 0, "nested comment whole");
    expect_token(&toks[2], '1', 36, 1, 0, 0, "nested trailing number");

    /* same input, legacy dialect keeps the X behavior */
    n = collect_tokens(q, qlen, FLAG_QUOTE_NONE | FLAG_SQL_ANSI, toks,
                       MAX_RAW_TOKENS);
    CHECK(n >= 2 && toks[0].type == 'E' && toks[1].type == 'X',
          "legacy nested comment stays X");

    /* unclosed comment runs to the slice end, still no X */
    const char *uc = "/* unclosed";
    n = collect_tokens(uc, strlen(uc), FLAG_SQL_PG | FLAG_QUOTE_NONE, toks,
                       MAX_RAW_TOKENS);
    CHECK(n == 1, "unclosed comment is one token");
    expect_token(&toks[0], 'c', 0, strlen(uc), 0, 0, "unclosed comment span");

    /* quotes, dollar markers and the MySQL conditional opener are content */
    const char *cc = "/* ' $x$ /*! */ tail */ 9";
    n = collect_tokens(cc, strlen(cc), FLAG_SQL_PG | FLAG_QUOTE_NONE, toks,
                       MAX_RAW_TOKENS);
    CHECK(n == 2, "comment content stays in one comment");
    expect_token(&toks[0], 'c', 0, 23, 0, 0, "comment content span");
    expect_token(&toks[1], '1', 24, 1, 0, 0, "token after comment");
}

static void test_dollar_strings(void) {
    stoken_t toks[MAX_RAW_TOKENS];
    int n;

    /* the differently-cased inner tag must not close the string */
    const char *dc = "$a_1$one $A_1$ two$a_1$";
    n = collect_tokens(dc, strlen(dc), FLAG_SQL_PG | FLAG_QUOTE_NONE, toks,
                       MAX_RAW_TOKENS);
    CHECK(n == 1, "case-sensitive dollar tag gives one string");
    expect_token(&toks[0], 's', 5, 13, '$', '$', "dollar string span");

    n = collect_tokens("$$x$$", 5, FLAG_SQL_PG | FLAG_QUOTE_NONE, toks,
                       MAX_RAW_TOKENS);
    CHECK(n == 1, "empty dollar tag");
    expect_token(&toks[0], 's', 2, 1, '$', '$', "empty dollar tag span");

    n = collect_tokens("$$unclosed", 10, FLAG_SQL_PG | FLAG_QUOTE_NONE, toks,
                       MAX_RAW_TOKENS);
    expect_token(&toks[0], 's', 2, 8, '$', 0, "unclosed dollar representation");

    /* a digit-leading tag never opens a dollar string */
    n = collect_tokens("$1$ x", 5, FLAG_SQL_PG | FLAG_QUOTE_NONE, toks,
                       MAX_RAW_TOKENS);
    CHECK(n == 3 && toks[0].type == '1' && toks[1].type == 'n' &&
              toks[1].val[0] == '$',
          "digit-leading tag is not a dollar string");

    /* '$' glued inside an unquoted identifier cannot open one either */
    n = collect_tokens("abc$1$ x", 8, FLAG_SQL_PG | FLAG_QUOTE_NONE, toks,
                       MAX_RAW_TOKENS);
    CHECK(n == 2 && toks[0].type == 'n' && toks[0].len == 6,
          "dollar inside bareword does not open a string");

    /* the same holds for a otherwise-valid tag glued to a bareword */
    n = collect_tokens("abc$x$ z", 8, FLAG_SQL_PG | FLAG_QUOTE_NONE, toks,
                       MAX_RAW_TOKENS);
    CHECK(n == 2 && toks[0].type == 'n' && toks[0].len == 6,
          "valid dollar tag inside bareword does not open a string");

    /* byte-for-byte, case-sensitive closing tag */
    n = collect_tokens("$Tag$x$tag$", 11, FLAG_SQL_PG | FLAG_QUOTE_NONE, toks,
                       MAX_RAW_TOKENS);
    expect_token(&toks[0], 's', 5, 6, '$', 0, "wrong-case tag never closes");

    n = collect_tokens("$_a1$x$_a1$", 11, FLAG_SQL_PG | FLAG_QUOTE_NONE, toks,
                       MAX_RAW_TOKENS);
    expect_token(&toks[0], 's', 5, 1, '$', '$', "underscore tag closes");

    /* NUL inside dollar content must not stop the byte-wise scan */
    char nuldl[10];
    memcpy(nuldl, "$t$a\0b$t$z", 10);
    n = collect_tokens(nuldl, 10, FLAG_SQL_PG | FLAG_QUOTE_NONE, toks,
                       MAX_RAW_TOKENS);
    CHECK(n == 2, "NUL in dollar content does not stop scan");
    expect_token(&toks[0], 's', 3, 3, '$', '$', "dollar span across NUL");
    expect_token(&toks[1], 'n', 9, 1, 0, 0, "token after NUL dollar string");

    /* long content: token.val is truncated to 31 bytes, scanning is not */
    char longbuf[52];
    longbuf[0] = '$';
    longbuf[1] = 't';
    longbuf[2] = '$';
    memset(longbuf + 3, 'a', 40);
    memcpy(longbuf + 43, "$t$ after", 9);
    n = collect_tokens(longbuf, 52, FLAG_SQL_PG | FLAG_QUOTE_NONE, toks,
                       MAX_RAW_TOKENS);
    CHECK(n == 2, "long dollar string still closes");
    /* token.len/val are capped at 31 bytes by design; the proof that the
     * scan was not truncated is that the closing tag was still found and
     * tokenization resumes at the exact position after it */
    CHECK(toks[0].len == 31, "long dollar token.val is capped at 31 bytes");
    CHECK(toks[0].str_close == '$', "long dollar closed flag");
    expect_token(&toks[1], 'n', 47, 5, 0, 0, "scan resumes after long dollar");
}

static void test_strings_and_identifiers(void) {
    stoken_t toks[MAX_RAW_TOKENS];
    int pg_std = FLAG_SQL_PG | FLAG_QUOTE_NONE;
    int pg_legacy = pg_std | FLAG_PG_LEGACY_STRINGS;

    /* same bytes, different ordinary-string option: different end position */
    const char *bs = "'a\\'b' x"; /* 'a\'b' x */
    int n = collect_tokens(bs, strlen(bs), pg_std, toks, MAX_RAW_TOKENS);
    CHECK(n == 3, "standard strings split at backslashed quote");
    expect_token(&toks[0], 's', 1, 2, '\'', '\'', "standard first string");

    n = collect_tokens(bs, strlen(bs), pg_legacy, toks, MAX_RAW_TOKENS);
    CHECK(n == 2, "legacy strings swallow backslashed quote");
    expect_token(&toks[0], 's', 1, 4, '\'', '\'', "legacy whole string");

    /* E'...' always honors backslashes, even under standard_strings */
    const char *es = "E'a\\'b' x";
    n = collect_tokens(es, strlen(es), pg_std, toks, MAX_RAW_TOKENS);
    CHECK(n == 2, "E-string always escape-aware");
    expect_token(&toks[0], 's', 2, 4, '\'', '\'', "E-string span");

    /* a bare 'e' not followed by a quote is just a word */
    n = collect_tokens("enum", 4, pg_std, toks, MAX_RAW_TOKENS);
    CHECK(n == 1 && toks[0].type == 'n', "e without quote is a bareword");

    /* N'...' national literal honors doubled quotes and the string option */
    n = collect_tokens("n'a''b'", 7, pg_std, toks, MAX_RAW_TOKENS);
    CHECK(n == 1, "N-prefix string tokenizes");
    expect_token(&toks[0], 's', 2, 4, '\'', '\'', "N-string doubled span");

    /* doubled quotes never terminate the string */
    n = collect_tokens("'a''b' x", 8, pg_std, toks, MAX_RAW_TOKENS);
    CHECK(n == 2, "doubled quote stays in string");
    expect_token(&toks[0], 's', 1, 4, '\'', '\'', "doubled quote span");

    n = collect_tokens("'unclosed", 9, pg_std, toks, MAX_RAW_TOKENS);
    expect_token(&toks[0], 's', 1, 8, '\'', 0, "unclosed string representation");

    /* double quotes are identifiers; doubled quotes stay inside */
    n = collect_tokens("\"a\"\"b\" x", 8, pg_std, toks, MAX_RAW_TOKENS);
    CHECK(n == 2 && toks[0].type == 'n', "double quote is an identifier");
    expect_token(&toks[0], 'n', 0, 6, '"', '"', "identifier span");

    /* backslash cannot eat the closing identifier quote */
    n = collect_tokens("\"a\\\"\" z", 7, pg_std, toks, MAX_RAW_TOKENS);
    CHECK(toks[0].type == 'n' && toks[0].str_close == 0,
          "backslash cannot close identifier");

    /* NUL byte, no terminator: the quoted slice past the NUL still scans */
    char nulslice[5];
    memcpy(nulslice, "a\0b'c", 5);
    n = collect_tokens(nulslice, 5, pg_std, toks, MAX_RAW_TOKENS);
    CHECK(n == 3, "NUL slice tokenizes past the NUL");
    expect_token(&toks[0], 'n', 0, 1, 0, 0, "token before NUL");
    expect_token(&toks[1], 'n', 2, 1, 0, 0, "token after NUL");
    expect_token(&toks[2], 's', 4, 1, '\'', 0, "unclosed string after NUL");
}

static void test_fold_and_decisions(void) {
    struct libinjection_sqli_state sf;
    char fp[8];
    const char *nested = "SELECT /* outer /* inner */ tail */ 7";

    /* folding reuses the existing rules on top of PG tokens */
    libinjection_sqli_init(&sf, nested, strlen(nested),
                           FLAG_SQL_PG | FLAG_QUOTE_NONE);
    int tlen = libinjection_sqli_fold(&sf);
    CHECK(tlen == 2 && sf.tokenvec[0].type == 'E' &&
              sf.tokenvec[1].type == '1',
          "PG fold of nested comment is E1");

    libinjection_sqli_init(&sf, nested, strlen(nested),
                           FLAG_QUOTE_NONE | FLAG_SQL_ANSI);
    tlen = libinjection_sqli_fold(&sf);
    int has_evil = 0;
    for (int i = 0; i < tlen; ++i) {
        has_evil |= (sf.tokenvec[i].type == 'X');
    }
    CHECK(has_evil, "legacy fold still produces X");

    /* simple PG entry point */
    CHECK(libinjection_sqli_pg("1 OR 1=1", 8, 1, fp) == 1 &&
              strcmp(fp, "1&1") == 0,
          "PG api detects classic tautology (standard)");
    CHECK(libinjection_sqli_pg("1 OR 1=1", 8, 0, fp) == 1 &&
              strcmp(fp, "1&1") == 0,
          "PG api detects classic tautology (legacy)");

    CHECK(libinjection_sqli_pg("hello world", 11, 1, fp) == 0 && fp[0] == 0,
          "PG api benign input clears fingerprint");
    CHECK(libinjection_sqli_pg("hello world", 11, 0, fp) == 0 && fp[0] == 0,
          "PG api benign input legacy option also clears");

    CHECK(libinjection_sqli_pg("x", 1, 2, fp) == LIBINJECTION_RESULT_ERROR &&
              fp[0] == 0,
          "invalid standard_strings is an error");
    CHECK(libinjection_sqli_pg("x", 1, -1, fp) == LIBINJECTION_RESULT_ERROR &&
              fp[0] == 0,
          "negative standard_strings is an error");
    CHECK(libinjection_sqli_pg("", 0, 1, fp) == 0 && fp[0] == 0,
          "empty input is benign with cleared output");

    /* NUL-bearing input judged through an explicit length */
    char nulinput[9];
    memcpy(nulinput, "1 OR\0 1=1", 9);
    CHECK(libinjection_sqli_pg(nulinput, 8, 1, fp) == 0 && fp[0] == 0,
          "NUL input does not read past given length");

    /* PG never tries the double-quote string context the legacy path uses */
    const char *dq = "1\" OR \"1\"=\"1";
    CHECK(libinjection_sqli(dq, strlen(dq), fp) == 1,
          "legacy path still catches double-quote payload");
    CHECK(libinjection_sqli_pg(dq, strlen(dq), 1, fp) == 0 && fp[0] == 0,
          "PG path treats double quotes as identifiers");

    /* state-based entry, plus callback and userdata survival over reparse */
    const char *payload = "1' OR '1'='1";
    libinjection_sqli_init(&sf, payload, strlen(payload),
                           FLAG_SQL_PG | FLAG_QUOTE_NONE);
    int userdata_marker = 42;
    g_wrapped_lookup = sf.lookup;
    g_callback_calls = 0;
    g_callback_userdata = (void *)0;
    libinjection_sqli_callback(&sf, counting_lookup, &userdata_marker);
    CHECK(libinjection_is_sqli(&sf) == 1 && strcmp(sf.fingerprint, "s&sos") == 0,
          "state PG is_sqli detects quote tautology");
    CHECK(g_callback_calls > 0, "custom lookup callback was invoked");
    CHECK(g_callback_userdata == &userdata_marker,
          "callback userdata survives reparses");
    CHECK(sf.lookup == counting_lookup, "lookup callback remains installed");

    /* fingerprint reparse must keep the PG string option bits */
    libinjection_sqli_init(&sf, "E'x'", 4,
                           FLAG_SQL_PG | FLAG_QUOTE_NONE |
                               FLAG_PG_LEGACY_STRINGS);
    libinjection_sqli_fingerprint(&sf, FLAG_SQL_PG | FLAG_QUOTE_NONE |
                                          FLAG_PG_LEGACY_STRINGS);
    CHECK((sf.flags & FLAG_SQL_PG) && (sf.flags & FLAG_PG_LEGACY_STRINGS),
          "fingerprint reparse retains PG options");
}

static void test_mode_alternation_and_reuse(void) {
    struct libinjection_sqli_state sf;
    const char *q = "SELECT /* a /* b */ c */ 1";
    stoken_t toks[MAX_RAW_TOKENS];
    int n;

    /* PG, then legacy, then PG again over the same state storage */
    n = collect_tokens(q, strlen(q), FLAG_SQL_PG | FLAG_QUOTE_NONE, toks,
                       MAX_RAW_TOKENS);
    CHECK(n == 3 && toks[1].type == 'c', "first PG pass nests comments");

    n = collect_tokens(q, strlen(q), FLAG_QUOTE_NONE | FLAG_SQL_ANSI, toks,
                       MAX_RAW_TOKENS);
    CHECK(n >= 2 && toks[1].type == 'X', "alternating legacy pass sees X");

    n = collect_tokens(q, strlen(q), FLAG_SQL_PG | FLAG_QUOTE_NONE, toks,
                       MAX_RAW_TOKENS);
    CHECK(n == 3 && toks[1].type == 'c' && toks[1].len == 17,
          "second PG pass is reproducible");

    /* reuse: two independent runs over identical slices must match */
    stoken_t run1[MAX_RAW_TOKENS], run2[MAX_RAW_TOKENS];
    int n1 = collect_tokens(q, strlen(q), FLAG_SQL_PG | FLAG_QUOTE_NONE, run1,
                            MAX_RAW_TOKENS);
    int n2 = collect_tokens(q, strlen(q), FLAG_SQL_PG | FLAG_QUOTE_NONE, run2,
                            MAX_RAW_TOKENS);
    CHECK(n1 == n2 && memcmp(run1, run2, n1 * sizeof(stoken_t)) == 0,
          "state reuse produces identical tokens");

    /* slice boundary: comment open inside the slice, close outside */
    libinjection_sqli_init(&sf, q, 20, FLAG_SQL_PG | FLAG_QUOTE_NONE);
    n = 0;
    while (n < MAX_RAW_TOKENS && libinjection_sqli_tokenize(&sf) == 1) {
        memcpy(&toks[n], sf.current, sizeof(stoken_t));
        n += 1;
    }
    CHECK(n == 2, "slice ends inside unclosed comment");
    expect_token(&toks[0], 'E', 0, 6, 0, 0, "slice select token");
    expect_token(&toks[1], 'c', 7, 13, 0, 0, "slice comment consumes to end");
}

static void test_simulated_outer_quote(void) {
    struct libinjection_sqli_state sf;
    const char *s = "1\\' OR 2"; /* 1\' OR 2 */
    stoken_t t;

    /* standard strings: simulated outer quote ends before the escaped pair */
    libinjection_sqli_init(&sf, s, strlen(s),
                           FLAG_SQL_PG | FLAG_QUOTE_SINGLE);
    CHECK(libinjection_sqli_tokenize(&sf) == 1, "simulated quote tokenizes");
    memcpy(&t, sf.current, sizeof(stoken_t));
    CHECK(t.type == 's' && t.pos == 0 && t.len == 2 && t.str_open == 0 &&
              t.str_close == '\'' && sf.pos == 3,
          "simulated standard string ends early");

    /* legacy strings: the backslash eats the quote, string runs to EOL */
    libinjection_sqli_init(
        &sf, s, strlen(s),
        FLAG_SQL_PG | FLAG_QUOTE_SINGLE | FLAG_PG_LEGACY_STRINGS);
    CHECK(libinjection_sqli_tokenize(&sf) == 1, "legacy simulated tokenizes");
    memcpy(&t, sf.current, sizeof(stoken_t));
    CHECK(t.type == 's' && t.len == 8 && t.str_close == 0 &&
              sf.pos == strlen(s),
          "simulated legacy string stays open to end");

    /* PG mode never honors a simulated double quote as a string */
    libinjection_sqli_init(&sf, "\"id\" x", 7,
                           FLAG_SQL_PG | FLAG_QUOTE_DOUBLE);
    CHECK(libinjection_sqli_tokenize(&sf) == 1, "double-quote flag still runs");
    CHECK(sf.current->type == 'n' && sf.current->str_open == '"',
          "leading double quote is an identifier, not a simulated string");
}

int main(void) {
    test_nested_comments();
    test_dollar_strings();
    test_strings_and_identifiers();
    test_fold_and_decisions();
    test_mode_alternation_and_reuse();
    test_simulated_outer_quote();

    /* guard against an accidentally empty test binary */
    if (g_checks < 30) {
        printf("not ok - only %d checks ran\n", g_checks);
        g_failures += 1;
    }

    printf("1..%d\n", g_checks);
    if (g_failures == 0) {
        printf("PG_MODE_OK checks=%d\n", g_checks);
        return 0;
    }
    printf("PG_MODE_FAILED checks=%d failures=%d\n", g_checks, g_failures);
    return 1;
}
