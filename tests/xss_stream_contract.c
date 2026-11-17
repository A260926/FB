/*
 * Contract tests for the incremental XSS detector.
 *
 * Everything here runs in memory: fragments are generated in C, split
 * at every possible boundary, and compared against the one-shot API.
 * No script is ever executed; no network, files or extra packages are
 * involved.
 *
 * On success the program prints, in order:
 *   XSS_STREAM_POLICY_OK    - API lifecycle and error contract
 *   XSS_STREAM_BOUNDARY_OK  - block splits never change the verdict
 *   XSS_STREAM_SPACE_OK     - bounded memory and single-pass work
 */

#include "libinjection_xss.h"
#include "libinjection_xss_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        g_checks += 1;                                                         \
        if (!(cond)) {                                                         \
            g_failures += 1;                                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                        \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

/* a byte string that may contain NULs */
typedef struct fragment {
    const char *data;
    size_t len;
    int expected; /* hand-written verdict for the all-contexts (-1) API */
    const char *name;
} fragment_t;

static fragment_t frag(const char *s, int expected, const char *name) {
    fragment_t f;
    f.data = s;
    f.len = strlen(s);
    f.expected = expected;
    f.name = name;
    return f;
}

/* feed one stream with arbitrary block sizes */
static int feed_blocks(libinjection_xss_stream *st, const char *s, size_t len,
                       size_t block) {
    size_t pos = 0;
    if (block == 0) {
        return libinjection_xss_stream_feed(st, s, len);
    }
    while (pos < len) {
        size_t n = len - pos;
        if (n > block) {
            n = block;
        }
        if (libinjection_xss_stream_feed(st, s + pos, n) != 0) {
            return -1;
        }
        pos += n;
    }
    return 0;
}

static int stream_eval(int flags, const char *s, size_t len, size_t block) {
    libinjection_xss_stream *st = libinjection_xss_stream_new(flags);
    int r;
    CHECK(st != NULL, "stream allocation/flags");
    if (st == NULL) {
        return LIBINJECTION_RESULT_ERROR;
    }
    CHECK(feed_blocks(st, s, len, block) == 0, "feed accepted");
    r = libinjection_xss_stream_finish(st);
    libinjection_xss_stream_free(st);
    return r;
}

static int oneshot_eval(int flags, const char *s, size_t len) {
    return flags == -1 ? libinjection_xss(s, len)
                       : libinjection_is_xss(s, len, flags);
}

/* raw bytes (NULs allowed), compared in every mode at several block
 * sizes against the one-shot result for that same mode */
static void check_raw_everywhere(const char *name, const unsigned char *s,
                                 size_t len) {
    static const int modes[6] = {-1, DATA_STATE, VALUE_NO_QUOTE,
                                 VALUE_SINGLE_QUOTE, VALUE_DOUBLE_QUOTE,
                                 VALUE_BACK_QUOTE};
    static const size_t blocks[5] = {1, 2, 3, 5, 64};
    int m, k;
    for (m = 0; m < 6; ++m) {
        int expect = oneshot_eval(modes[m], (const char *)s, len);
        for (k = 0; k < 5; ++k) {
            CHECK(stream_eval(modes[m], (const char *)s, len, blocks[k]) ==
                      expect,
                  "raw '%s' mode %d block %zu", name, modes[m], blocks[k]);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 1. Lifecycle and error contract                                     */
/* ------------------------------------------------------------------ */

static void policy_test(void) {
    static const int valid_flags[6] = {-1, DATA_STATE, VALUE_NO_QUOTE,
                                       VALUE_SINGLE_QUOTE, VALUE_DOUBLE_QUOTE,
                                       VALUE_BACK_QUOTE};
    static const int bad_flags[5] = {-2, 5, 6, 42, -100};
    size_t i;
    int r;

    for (i = 0; i < sizeof(valid_flags) / sizeof(valid_flags[0]); ++i) {
        libinjection_xss_stream *st =
            libinjection_xss_stream_new(valid_flags[i]);
        CHECK(st != NULL, "valid flag %d must construct", valid_flags[i]);
        libinjection_xss_stream_free(st);
    }
    for (i = 0; i < sizeof(bad_flags) / sizeof(bad_flags[0]); ++i) {
        CHECK(libinjection_xss_stream_new(bad_flags[i]) == NULL,
              "invalid flag %d must return NULL", bad_flags[i]);
    }

    /* call errors */
    CHECK(libinjection_xss_stream_feed(NULL, "x", 1) == -1,
          "feed on NULL stream is an error");
    CHECK(libinjection_xss_stream_finish(NULL) == LIBINJECTION_RESULT_ERROR,
          "finish on NULL stream is an error");
    libinjection_xss_stream_free(NULL); /* must simply return */

    {
        libinjection_xss_stream *st = libinjection_xss_stream_new(-1);
        char chunk[4];
        CHECK(st != NULL, "construct");
        CHECK(libinjection_xss_stream_feed(st, NULL, 4) == -1,
              "NULL data with positive length is an error");
        CHECK(libinjection_xss_stream_feed(st, NULL, 0) == 0,
              "NULL data with zero length is accepted as an empty block");

        /* zero-length blocks, including non-tail ones, do not change
         * the verdict */
        memcpy(chunk, "<a h", 4);
        CHECK(libinjection_xss_stream_feed(st, chunk, 0) == 0, "empty block");
        CHECK(libinjection_xss_stream_feed(st, chunk, 4) == 0, "feed 1");
        CHECK(libinjection_xss_stream_feed(st, "", 0) == 0,
              "interstitial empty block");
        memcpy(chunk, "ref=", 4);
        CHECK(libinjection_xss_stream_feed(st, chunk, 4) == 0, "feed 2");
        memcpy(chunk, "'jav", 4);
        CHECK(libinjection_xss_stream_feed(st, chunk, 4) == 0, "feed 3");
        CHECK(libinjection_xss_stream_feed(st, "", 0) == 0,
              "another interstitial empty block");
        memcpy(chunk, "ascri", 4);
        CHECK(libinjection_xss_stream_feed(st, chunk, 4) == 0, "feed 4");
        memcpy(chunk, "pt:x'", 4);
        CHECK(libinjection_xss_stream_feed(st, chunk, 4) == 0, "feed 5");

        r = libinjection_xss_stream_finish(st);
        CHECK(r == LIBINJECTION_RESULT_TRUE,
              "interleaved empty blocks preserve the verdict, got %d", r);

        /* finish is repeatable and stable */
        CHECK(libinjection_xss_stream_finish(st) == r,
              "repeat finish returns the stored result");
        CHECK(libinjection_xss_stream_finish(st) == r,
              "third finish still returns the stored result");

        /* feeding after finish is an error and leaves state intact */
        CHECK(libinjection_xss_stream_feed(st, "x", 1) == -1,
              "feed after finish must fail");
        CHECK(libinjection_xss_stream_finish(st) == r,
              "rejected feed did not alter the stored result");
        libinjection_xss_stream_free(st);
    }

    /* feed()'s 0 means "received", never a safety verdict: a dangerous
     * input still reports TRUE only at finish() */
    {
        libinjection_xss_stream *st = libinjection_xss_stream_new(DATA_STATE);
        const char *danger = "<script>alert(1)</script>";
        CHECK(libinjection_xss_stream_feed(st, danger, strlen(danger)) == 0,
              "feed returns 0 for an accepted dangerous block");
        CHECK(libinjection_xss_stream_finish(st) ==
                  LIBINJECTION_RESULT_TRUE,
              "the verdict arrives at finish, not at feed");
        libinjection_xss_stream_free(st);
    }

    /* two streams interleaved must keep independent state */
    {
        static const char danger[] = "<img src=x onerror=alert(1)>";
        static const char clean[] = "<p>ordinary text</p>";
        libinjection_xss_stream *a = libinjection_xss_stream_new(-1);
        libinjection_xss_stream *b = libinjection_xss_stream_new(-1);
        size_t na = strlen(danger);
        size_t nb = strlen(clean);
        size_t n = na > nb ? na : nb;
        size_t k;
        CHECK(a != NULL && b != NULL, "construct interleaved streams");
        for (k = 0; k < n; ++k) {
            if (k < na) {
                CHECK(libinjection_xss_stream_feed(a, danger + k, 1) == 0,
                      "interleaved feed a");
            }
            if (k < nb) {
                CHECK(libinjection_xss_stream_feed(b, clean + k, 1) == 0,
                      "interleaved feed b");
            }
        }
        CHECK(libinjection_xss_stream_finish(a) == LIBINJECTION_RESULT_TRUE,
              "interleaved dangerous stream stays dangerous");
        CHECK(libinjection_xss_stream_finish(b) == LIBINJECTION_RESULT_FALSE,
              "interleaved benign stream stays benign");
        libinjection_xss_stream_free(a);
        libinjection_xss_stream_free(b);
    }

    /* the caller may overwrite the input buffer as soon as feed()
     * returns: no input pointer is borrowed */
    {
        libinjection_xss_stream *st = libinjection_xss_stream_new(-1);
        char scratch[12];
        static const char *const pieces[] = {"<a href=", "'javasc",
                                             "ript:x'", ">"};
        size_t k;
        for (k = 0; k < sizeof(pieces) / sizeof(pieces[0]); ++k) {
            size_t n = strlen(pieces[k]);
            memcpy(scratch, pieces[k], n);
            CHECK(libinjection_xss_stream_feed(st, scratch, n) == 0,
                  "feed from reusable scratch buffer");
            memset(scratch, 0x5a, sizeof(scratch)); /* clobber immediately */
        }
        CHECK(libinjection_xss_stream_finish(st) == LIBINJECTION_RESULT_TRUE,
              "clobbering the caller buffer does not corrupt detection");
        libinjection_xss_stream_free(st);
    }
}

/* ------------------------------------------------------------------ */
/* 2. Boundary invariance against the one-shot API                     */
/* ------------------------------------------------------------------ */

static const char *const MODE_NAMES[6] = {
    "all", "data", "no-quote", "single-quote", "double-quote", "back-quote"};

static void check_fragment_everywhere(const fragment_t *f) {
    static const int modes[6] = {-1, DATA_STATE, VALUE_NO_QUOTE,
                                 VALUE_SINGLE_QUOTE, VALUE_DOUBLE_QUOTE,
                                 VALUE_BACK_QUOTE};
    size_t cut;
    size_t block;
    int m;

    /* the hand-written expectation must match the shipped one-shot API */
    CHECK(oneshot_eval(-1, f->data, f->len) == f->expected,
          "sample '%s' hand-written expectation disagrees with one-shot",
          f->name);

    for (m = 0; m < 6; ++m) {
        int expect = oneshot_eval(modes[m], f->data, f->len);

        /* single feed */
        CHECK(stream_eval(modes[m], f->data, f->len, 0) == expect,
              "sample '%s' mode %s single feed", f->name, MODE_NAMES[m]);

        /* every two-block cut, including zero-length edges */
        for (cut = 0; cut <= f->len; ++cut) {
            libinjection_xss_stream *st =
                libinjection_xss_stream_new(modes[m]);
            int got;
            CHECK(st != NULL, "construct");
            CHECK(libinjection_xss_stream_feed(st, f->data, cut) == 0,
                  "first slice");
            CHECK(libinjection_xss_stream_feed(st, f->data + cut,
                                               f->len - cut) == 0,
                  "second slice");
            got = libinjection_xss_stream_finish(st);
            CHECK(got == expect,
                  "sample '%s' mode %s cut %zu: expected %d got %d", f->name,
                  MODE_NAMES[m], cut, expect, got);
            libinjection_xss_stream_free(st);
        }

        /* fixed small block sizes, so every multi-byte token is split
         * repeatedly */
        for (block = 1; block <= 7; ++block) {
            CHECK(stream_eval(modes[m], f->data, f->len, block) == expect,
                  "sample '%s' mode %s block %zu", f->name, MODE_NAMES[m],
                  block);
        }
    }
}

static void boundary_test(void) {
    /* plain strings, with hand-written all-context verdicts */
    fragment_t samples[] = {
        frag("<p>ordinary text</p>", 0, "plain paragraph"),
        frag("just some text where 1 < 2 and 3 > 0", 0, "text with brackets"),
        frag("<a href='javascript:x'>", 1, "baseline javascript href"),
        frag("<script>alert(1)</script>", 1, "script tag"),
        frag("<scriptx>", 0, "script prefix is not a full tag name"),
        frag("<img src=x onerror=alert(1)>", 1, "event handler attribute"),
        frag("<b onclick='a'>x</b>", 1, "onclick black attribute"),
        frag("<!--[if IE]><img src=x>", 1, "IE conditional comment"),
        frag("<!DOCTYPE html>", 1, "doctype"),
        frag("<div style='color:red'>x</div>", 1, "style attribute"),
        frag("<svg onload=1>", 1, "svg tag"),
        frag("<a href='java&#115;cript:1'>", 1, "decimal entity in URL"),
        frag("<a href='java&#115cript:1'>", 1,
             "decimal entity without semicolon"),
        frag("<a href='&#x6a;avascript:1'>", 1, "hex entity in URL"),
        frag("<a href='  javascript:1'>", 1, "control chars before URL"),
        frag("<!-- a perfectly normal comment -->", 0, "normal comment"),
        frag("<!--a`b-->", 1, "backtick inside comment"),
        frag("<!-- unterminated comment holding <script", 0,
             "unterminated comment at EOF is not a tag"),
        frag("<a attributename='onclick'>", 1, "indirect attribute name"),
        frag("<a xml:lang='en' href='data:text/x'>", 1, "data URL"),
        frag("<a href='vbscript:x'>", 1, "vbscript URL"),
        frag("<![CDATA[abc]]>", 0, "cdata section"),
        frag("<%x`y%>", 1, "IE alternate comment with backtick"),
        frag("<?x`y>", 1, "processing instruction comment"),
        frag("<img src=\"x\" onerror=1>", 1, "double quoted attribute"),
        frag("<a href=`javascript:x`>", 1, "backtick quoted attribute"),
        frag("<sCrIpT>x</sCrIpT>", 1, "case insensitive tag name"),
        frag("<a href='javascript:x", 1, "unterminated URL value at EOF"),
        frag("<!-- --!><script>x</script>-->", 1, "bang comment terminator"),
        frag("<!-- -- ---> <img src=x onerror=1>", 1,
             "triple dash comment terminator"),
        frag("</div>", 0, "benign close tag"),
        frag("<a href='https://example.com/'>x</a>", 0, "benign https href"),
    };
    size_t k;
    for (k = 0; k < sizeof(samples) / sizeof(samples[0]); ++k) {
        check_fragment_everywhere(&samples[k]);
    }

    /* byte arrays let NUL and high bytes take part in splits */
    {
        static const unsigned char nul_tag[] = {'<', 's', 'c', 0,   'r',
                                                'i', 'p', 't', '>'};
        static const unsigned char high_url[] = {
            '<', 'a', ' ', 'h', 'r', 'e', 'f', '=', '\'',
            (unsigned char)0xff, 'j', 'a', 'v', 'a', 's', 'c',
            'r', 'i', 'p', 't', ':', '1', '\'', '>'};
        static const unsigned char nul_url[] = {
            '<', 'a', ' ', 'h', 'r', 'e', 'f', '=', '\'', 'j', 'a', 'v',
            'a', 0,   's', 'c', 'r', 'i', 'p', 't', ':', '1', '\'', '>'};
        static const unsigned char nul_comment[] = {'<', '!', '-', '-', 0,
                                                    'x', '`', 'y', '-'};
        fragment_t raw[4];
        raw[0].data = (const char *)nul_tag;
        raw[0].len = sizeof(nul_tag);
        raw[0].expected = 1;
        raw[0].name = "NUL inside script name";
        raw[1].data = (const char *)high_url;
        raw[1].len = sizeof(high_url);
        raw[1].expected = 1;
        raw[1].name = "high byte before URL scheme";
        raw[2].data = (const char *)nul_url;
        raw[2].len = sizeof(nul_url);
        raw[2].expected = 1;
        raw[2].name = "NUL inside URL value";
        raw[3].data = (const char *)nul_comment;
        raw[3].len = sizeof(nul_comment);
        raw[3].expected = 1;
        raw[3].name = "NUL then backtick in comment";
        for (k = 0; k < 4; ++k) {
            check_fragment_everywhere(&raw[k]);
        }
    }

    /* the exact scenario from the task description: a name split in
     * the middle must not be judged at the block boundary */
    {
        static const char whole[] = "<scriptx>";
        size_t cut;
        for (cut = 1; cut < sizeof(whole) - 1; ++cut) {
            libinjection_xss_stream *st = libinjection_xss_stream_new(-1);
            int got;
            libinjection_xss_stream_feed(st, whole, cut);
            /* the first block alone would have ended here; it must
             * not have produced a verdict */
            libinjection_xss_stream_feed(st, whole + cut,
                                         sizeof(whole) - 1 - cut);
            got = libinjection_xss_stream_finish(st);
            CHECK(got == LIBINJECTION_RESULT_FALSE,
                  "split '<scriptx>' at %zu must resolve to the full name",
                  cut);
            libinjection_xss_stream_free(st);
        }
        /* but the same prefix completed by '>' is a real script tag */
        {
            libinjection_xss_stream *st = libinjection_xss_stream_new(-1);
            libinjection_xss_stream_feed(st, "<script", 7);
            libinjection_xss_stream_feed(st, ">", 1);
            CHECK(libinjection_xss_stream_finish(st) ==
                      LIBINJECTION_RESULT_TRUE,
                  "'<script' then '>' is detected across the boundary");
            libinjection_xss_stream_free(st);
        }
    }

    /* a dangerous attribute value must not end just because a block
     * does: the closing quote lands in the next block */
    {
        static const char s[] = "<a href='javascript:x'>";
        size_t len = sizeof(s) - 1;
        size_t cut;
        for (cut = 1; cut < len; ++cut) {
            libinjection_xss_stream *st = libinjection_xss_stream_new(-1);
            int got;
            libinjection_xss_stream_feed(st, s, cut);
            libinjection_xss_stream_feed(st, s + cut, len - cut);
            got = libinjection_xss_stream_finish(st);
            CHECK(got == LIBINJECTION_RESULT_TRUE,
                  "quoted URL value split at %zu stays dangerous", cut);
            libinjection_xss_stream_free(st);
        }
    }

    /* comment terminator split across a block, and EOF-only flush */
    {
        static const char closed[] = "<!-- safe-looking --><script>x</script>";
        static const char open_comment[] = "<!-- still open at block end ";
        size_t len = strlen(closed);
        size_t cut = 20; /* right inside the "-->" terminator */
        libinjection_xss_stream *st = libinjection_xss_stream_new(DATA_STATE);
        libinjection_xss_stream_feed(st, closed, cut);
        libinjection_xss_stream_feed(st, closed + cut, len - cut);
        CHECK(libinjection_xss_stream_finish(st) ==
                  libinjection_xss(closed, len),
              "comment terminator crossing a block is handled at finish");
        libinjection_xss_stream_free(st);

        st = libinjection_xss_stream_new(DATA_STATE);
        libinjection_xss_stream_feed(st, open_comment, strlen(open_comment));
        CHECK(libinjection_xss_stream_finish(st) ==
                  libinjection_xss(open_comment, strlen(open_comment)),
              "EOF comment flush matches the one-shot EOF handling");
        libinjection_xss_stream_free(st);
    }

    /* Regression cases reduced from randomized differential testing.
     * Each must match the one-shot verdict in every context and at
     * every small block size. */
    {
        /* NUL inside a name must not make the raw "xsl" rule fire */
        static const unsigned char nul_xsl[] = {'<', 'x', 0, 's', 'l', '-'};
        check_raw_everywhere("NUL-padded xsl-like name", nul_xsl,
                             sizeof(nul_xsl));

        /* is_close quirk: a close tag whose '>' is reached while
         * scanning attributes leaves is_close set; a later open tag
         * whose name starts with xsl must still be an open tag */
        static const unsigned char close_then_xsl[] = {
            'f', '\'', '>', '1', 0,   '<', '/', 'a', '\n', 'e',
            '>', '>',  'n', '<', 'x', 's', 'l', 'n', 'w',  '>', '\t'};
        check_raw_everywhere("close-tag is_close then xsl", close_then_xsl,
                             sizeof(close_then_xsl));

        /* consecutive '%' candidates in the IE alternate comment */
        static const unsigned char bogus2_pct[] = {'<', '%', 'x', '%',
                                                   '%', '>', 'y'};
        check_raw_everywhere("consecutive percent comment", bogus2_pct,
                             sizeof(bogus2_pct));

        /* NUL counts as whitespace when delimiting attributes */
        static const unsigned char nul_whitespace[] = {
            '"', '6', 'b', '=', '<', 0,   (unsigned char)0xca, '8',
            '\n', '=', '`', ']', 'b', '>', (unsigned char)0x90, '<',
            '%', 'Y', '`', 'o', 'x'};
        check_raw_everywhere("NUL attribute whitespace", nul_whitespace,
                             sizeof(nul_whitespace));

        /* empty bogus comment from a bare "<!>" */
        static const unsigned char bare_bang[] = {'<', '!', '>', 'x'};
        check_raw_everywhere("bare bang close", bare_bang,
                             sizeof(bare_bang));

        /* declaration prefix that only resolves after the block ended */
        static const unsigned char split_doctype[] = {
            '<', '!', 'D', 'O', 'C', 'T', 'Y', 'P', 'E', 'h', 't', 'm', 'l',
            '>'};
        check_raw_everywhere("split doctype prefix", split_doctype,
                             sizeof(split_doctype));
    }
}

/* ------------------------------------------------------------------ */
/* 3. Bounded memory and single-pass work over 16 MiB                  */
/* ------------------------------------------------------------------ */

#define SIXTEEN_MIB (16u * 1024u * 1024u)
#define BLOCK 4096u

static void space_test(void) {
    size_t struct_size = libinjection_xss_stream_struct_size();
    size_t peak;

    CHECK(struct_size <= 32768, "stream struct is %zu bytes, limit 32768",
          struct_size);

    /* (a) 16 MiB of ordinary text: nothing is retained */
    {
        libinjection_xss_stream *st = libinjection_xss_stream_new(DATA_STATE);
        static const char text_pat[] = "<a>AAAA";
        char *full = (char *)malloc(SIXTEEN_MIB);
        size_t i;
        int stream_result;
        int oneshot_result;
        CHECK(st != NULL && full != NULL, "allocations");
        CHECK(feed_blocks(st, text_pat, 0, 0) == 0, "noop");
        for (i = 0; i < SIXTEEN_MIB; ++i) {
            full[i] = 'A' + (char)(i % 26);
        }
        CHECK(feed_blocks(st, full, SIXTEEN_MIB, BLOCK) == 0,
              "feed 16 MiB of text");
        stream_result = libinjection_xss_stream_finish(st);
        oneshot_result = libinjection_xss(full, SIXTEEN_MIB);
        CHECK(stream_result == oneshot_result,
              "16 MiB text: stream %d != one-shot %d", stream_result,
              oneshot_result);
        CHECK(stream_result == LIBINJECTION_RESULT_FALSE,
              "16 MiB plain text is benign");
        CHECK(libinjection_xss_stream_bytes_accepted(st) == SIXTEEN_MIB,
              "accepted byte count");
        CHECK(libinjection_xss_stream_bytes_worked(st) == SIXTEEN_MIB,
              "each byte visited exactly once (no rescan)");
        peak = libinjection_xss_stream_alloc_peak(st);
        CHECK(peak == struct_size && peak <= 32768,
              "text run peak allocation %zu", peak);
        libinjection_xss_stream_free(st);
        free(full);
    }

    /* (b) 16 MiB long token: a URL scheme matched near the start, then
     * megabytes of value before the closing quote.  This is the case
     * that proves a long value does not need to be retained. */
    {
        libinjection_xss_stream *st = libinjection_xss_stream_new(-1);
        char *full = (char *)malloc(SIXTEEN_MIB);
        static const char head[] = "<a href='javascript:";
        static const char tail[] = "1'>";
        size_t head_len = sizeof(head) - 1;
        size_t tail_len = sizeof(tail) - 1;
        size_t middle = SIXTEEN_MIB - head_len - tail_len;
        int stream_result;
        int oneshot_result;
        size_t i;
        CHECK(st != NULL && full != NULL, "allocations");
        memcpy(full, head, head_len);
        for (i = 0; i < middle; ++i) {
            full[head_len + i] = 'A' + (char)(i % 26);
        }
        memcpy(full + head_len + middle, tail, tail_len);

        CHECK(feed_blocks(st, full, SIXTEEN_MIB, BLOCK) == 0,
              "feed 16 MiB long attribute value");
        stream_result = libinjection_xss_stream_finish(st);
        oneshot_result = libinjection_xss(full, SIXTEEN_MIB);
        CHECK(stream_result == oneshot_result,
              "16 MiB long value: stream %d != one-shot %d", stream_result,
              oneshot_result);
        CHECK(stream_result == LIBINJECTION_RESULT_TRUE,
              "URL scheme matched at the start of a 16 MiB value fires");
        CHECK(libinjection_xss_stream_bytes_accepted(st) == SIXTEEN_MIB,
              "accepted byte count");
        CHECK(libinjection_xss_stream_bytes_worked(st) ==
                      SIXTEEN_MIB * 5u,
              "all-contexts mode visits each byte once per context");
        peak = libinjection_xss_stream_alloc_peak(st);
        CHECK(peak == struct_size && peak <= 32768,
              "long value run peak allocation %zu", peak);
        libinjection_xss_stream_free(st);
        free(full);
    }

    /* (b2) a scheme buried only at the very end of a huge value is
     * FALSE for both paths: the heuristic matches a value prefix, so
     * the stream must not invent a different (whole-value) semantics */
    {
        libinjection_xss_stream *st = libinjection_xss_stream_new(-1);
        char *full = (char *)malloc(SIXTEEN_MIB);
        static const char head2[] = "<a href='";
        static const char tail2[] = "javascript:1'>";
        size_t head_len = sizeof(head2) - 1;
        size_t tail_len = sizeof(tail2) - 1;
        size_t middle = SIXTEEN_MIB - head_len - tail_len;
        int stream_result;
        int oneshot_result;
        size_t i;
        CHECK(st != NULL && full != NULL, "allocations");
        memcpy(full, head2, head_len);
        for (i = 0; i < middle; ++i) {
            full[head_len + i] = 'A' + (char)(i % 26);
        }
        memcpy(full + head_len + middle, tail2, tail_len);
        CHECK(feed_blocks(st, full, SIXTEEN_MIB, BLOCK) == 0,
              "feed 16 MiB value with a trailing scheme");
        stream_result = libinjection_xss_stream_finish(st);
        oneshot_result = libinjection_xss(full, SIXTEEN_MIB);
        CHECK(stream_result == oneshot_result,
              "trailing-scheme value: stream %d != one-shot %d",
              stream_result, oneshot_result);
        CHECK(stream_result == LIBINJECTION_RESULT_FALSE,
              "a scheme after megabytes of value is not a prefix match");
        CHECK(libinjection_xss_stream_alloc_peak(st) <= 32768,
              "trailing-scheme peak allocation");
        libinjection_xss_stream_free(st);
        free(full);
    }

    /* (c) long tag name: far beyond every blacklist name, must stay
     * bounded and benign */
    {
        libinjection_xss_stream *st = libinjection_xss_stream_new(-1);
        char *full = (char *)malloc(SIXTEEN_MIB);
        size_t i;
        int r;
        CHECK(st != NULL && full != NULL, "allocations");
        full[0] = '<';
        for (i = 1; i < SIXTEEN_MIB - 1; ++i) {
            full[i] = 'A';
        }
        full[SIXTEEN_MIB - 1] = '>';
        CHECK(feed_blocks(st, full, SIXTEEN_MIB, BLOCK) == 0,
              "feed 16 MiB tag name");
        r = libinjection_xss_stream_finish(st);
        CHECK(r == libinjection_xss(full, SIXTEEN_MIB),
              "long tag name verdict matches one-shot");
        CHECK(r == LIBINJECTION_RESULT_FALSE, "long tag name is benign");
        CHECK(libinjection_xss_stream_alloc_peak(st) <= 32768,
              "long name peak allocation");
        libinjection_xss_stream_free(st);
        free(full);
    }

    /* (d) long comment whose backtick sits at the very last byte: the
     * any-position backtick flag must survive an arbitrarily long
     * comment without buffering it */
    {
        libinjection_xss_stream *st = libinjection_xss_stream_new(DATA_STATE);
        char *full = (char *)malloc(SIXTEEN_MIB);
        size_t i;
        int r;
        CHECK(st != NULL && full != NULL, "allocations");
        memcpy(full, "<!--", 4);
        for (i = 4; i < SIXTEEN_MIB - 1; ++i) {
            full[i] = 'x';
        }
        full[SIXTEEN_MIB - 1] = '`';
        CHECK(feed_blocks(st, full, SIXTEEN_MIB, BLOCK) == 0,
              "feed 16 MiB comment");
        r = libinjection_xss_stream_finish(st);
        CHECK(r == libinjection_xss(full, SIXTEEN_MIB),
              "long comment verdict matches one-shot");
        CHECK(r == LIBINJECTION_RESULT_TRUE,
              "backtick at the end of a 16 MiB comment still fires");
        CHECK(libinjection_xss_stream_alloc_peak(st) <= 32768,
              "long comment peak allocation");
        libinjection_xss_stream_free(st);
        free(full);
    }

    /* Recorded, not merely bounded: print the actual peak allocation
     * and the single-pass work counters so a hidden whole-input cache
     * or a per-block rescan would show up as a number, not a timing
     * guess.  The stream is one fixed allocation, so peak == struct
     * size regardless of the 16 MiB inputs above. */
    {
        static const char probe[] = "<a href='javascript:x'>";
        libinjection_xss_stream *one =
            libinjection_xss_stream_new(DATA_STATE);
        libinjection_xss_stream *all = libinjection_xss_stream_new(-1);
        size_t probe_len = sizeof(probe) - 1;
        CHECK(one != NULL && all != NULL, "probe allocation");
        CHECK(libinjection_xss_stream_feed(one, probe, probe_len) == 0,
              "probe feed one");
        CHECK(libinjection_xss_stream_feed(all, probe, probe_len) == 0,
              "probe feed all");
        CHECK(libinjection_xss_stream_finish(one) ==
                  LIBINJECTION_RESULT_TRUE,
              "one-context probe verdict");
        CHECK(libinjection_xss_stream_finish(all) ==
                  LIBINJECTION_RESULT_TRUE,
              "all-context probe verdict");
        printf("SPACE stream_struct=%zu bytes (limit 32768); "
               "one-context worked=%zu/accepted=%zu; "
               "all-context worked=%zu/accepted=%zu (5 visits/byte)\n",
               libinjection_xss_stream_struct_size(),
               libinjection_xss_stream_bytes_worked(one),
               libinjection_xss_stream_bytes_accepted(one),
               libinjection_xss_stream_bytes_worked(all),
               libinjection_xss_stream_bytes_accepted(all));
        CHECK(libinjection_xss_stream_alloc_peak(one) == struct_size,
              "recorded peak equals the single allocation");
        CHECK(libinjection_xss_stream_bytes_worked(one) ==
                      libinjection_xss_stream_bytes_accepted(one),
              "one-context work is exactly one visit per byte");
        CHECK(libinjection_xss_stream_bytes_worked(all) ==
                      libinjection_xss_stream_bytes_accepted(all) * 5u,
              "all-context work is one visit per byte per context");
        libinjection_xss_stream_free(one);
        libinjection_xss_stream_free(all);
    }
}

int main(void) {
    policy_test();
    if (g_failures != 0) {
        printf("policy section failed: %d failure(s) of %d checks\n",
               g_failures, g_checks);
        return 1;
    }
    printf("XSS_STREAM_POLICY_OK\n");

    boundary_test();
    if (g_failures != 0) {
        printf("boundary section failed: %d failure(s) of %d checks\n",
               g_failures, g_checks);
        return 1;
    }
    printf("XSS_STREAM_BOUNDARY_OK\n");

    space_test();
    if (g_failures != 0) {
        printf("space section failed: %d failure(s) of %d checks\n",
               g_failures, g_checks);
        return 1;
    }
    printf("XSS_STREAM_SPACE_OK\n");

    printf("all stream contract checks passed (%d assertions)\n", g_checks);
    return 0;
}
