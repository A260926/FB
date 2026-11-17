/*
 * Incremental XSS detector.
 *
 * This is the same heuristic as libinjection_xss(): the HTML5 lexer
 * states from libinjection_html5.c and the classification rules from
 * libinjection_xss.c, re-expressed so a byte stream can be fed in
 * arbitrary blocks.  No input bytes are retained beyond a handful of
 * bounded summary buffers: long text, long names, long values and long
 * comments all advance in one pass, so a stream's memory use never
 * grows with the input length.
 *
 * Invariants kept with the one-shot path:
 *  - block boundaries are not EOF: only finish() runs the lexer's EOF
 *    actions (flushing the pending tag name / attribute / comment);
 *  - NUL bytes, high bytes, backticks, indirect attribute names,
 *    leading URL control bytes and semicolon-less numeric references
 *    are handled by the exact same rules and tables;
 *  - every classification decision goes through routines that live in
 *    libinjection_xss.c next to the single set of blacklists.
 */

#include "libinjection_xss.h"
#include "libinjection_xss_internal.h"

#include <stdlib.h>
#include <string.h>

/* character constants, same values as the private ones in the h5 lexer */
#define STREAM_CHAR_NULL 0
#define STREAM_CHAR_BANG 33
#define STREAM_CHAR_DOUBLE 34
#define STREAM_CHAR_PERCENT 37
#define STREAM_CHAR_SINGLE 39
#define STREAM_CHAR_DASH 45
#define STREAM_CHAR_SLASH 47
#define STREAM_CHAR_LT 60
#define STREAM_CHAR_EQUALS 61
#define STREAM_CHAR_GT 62
#define STREAM_CHAR_QUESTION 63
#define STREAM_CHAR_RIGHTB 93
#define STREAM_CHAR_TICK 96

/* lexer states, one per h5_state_* plus an explicit EOF state */
enum stream_state {
    ST_DATA = 0,
    ST_TAG_OPEN,
    ST_END_TAG_OPEN,
    ST_TAG_NAME,
    ST_BEFORE_ATTR_NAME,
    ST_ATTR_NAME,
    ST_AFTER_ATTR_NAME,
    ST_BEFORE_ATTR_VALUE,
    ST_ATTR_VALUE_DQ,
    ST_ATTR_VALUE_SQ,
    ST_ATTR_VALUE_BQ,
    ST_ATTR_VALUE_NQ,
    ST_AFTER_ATTR_VALUE,
    ST_SELF_CLOSING,
    ST_COMMENT,
    ST_COMMENT_DASH,        /* '-' seen, NULs may follow before '-'/'!' */
    ST_COMMENT_SECOND,      /* "--" or "-!" seen, expecting '>' */
    ST_BOGUS,
    ST_BOGUS2,
    ST_BOGUS2_PCT,          /* bogus2 with a trailing '%' pending */
    ST_MARKUP_DECL,
    ST_CDATA,
    ST_DOCTYPE,
    ST_EOF,
    ST_NUM_STATES
};

/* numeric character reference decoder phases */
enum dec_phase {
    DEC_RAW = 0,
    DEC_AMP,    /* saw '&' */
    DEC_HASH,   /* saw '&#' */
    DEC_HEXP,   /* saw '&#x', waiting for the first hex digit */
    DEC_DEC,    /* saw '&#' plus at least one decimal digit */
    DEC_HEX     /* saw '&#x' plus at least one hex digit */
};

#define STREAM_CONTEXTS 5
#define COMMENT_HEAD_CAP 6             /* comment rules only inspect 6 bytes */
#define PREFIX_CAP 7                   /* DOCTYPE/[CDATA[ are 7 bytes */
#define URL_WORDS 4
#define URL_DEAD 12                    /* position marker: word can't match */
#define STREAM_MEMORY_LIMIT 32768u
#define ENTITY_LIMIT 0x1000FFL

/* URL scheme words tried by is_black_url(), in its exact order */
static const char *const URL_SCHEMES[URL_WORDS] = {"DATA", "VIEW-SOURCE",
                                                    "JAVA", "VBSCRIPT"};

typedef struct stream_ctx {
    int state;
    int is_close;
    size_t byte_count;      /* 0-based position of the next input byte */

    /* pending tag/attribute name, matched incrementally against the
     * same blacklists the one-shot path uses */
    xss_name_matcher_t name;

    /* pending comment: backtick flag plus at most six head bytes */
    char comment_head[COMMENT_HEAD_CAP];
    size_t comment_head_len;
    size_t comment_len;     /* total bytes, capped at COMMENT_HEAD_CAP + 1 */
    int comment_backtick;
    size_t comment_nulls;   /* NULs skipped between a dangling '-' and
                               the next decision byte */
    char comment_second;    /* second byte of a "--" / "-!" candidate */

    /* markup declaration open: bounded prefix while it stays ambiguous */
    char prefix[PREFIX_CAP];
    size_t prefix_len;

    /* cdata "]]>" terminator feeder, 0..2 matched bytes */
    int cdata_match;

    /* pending attribute value */
    xss_name_matcher_t value_name; /* raw bytes, for ATTRIBUTENAME values */

    /* numeric character reference inside the value */
    int dec_phase;
    long dec_val;

    /* URL prefix matchers running over the decoded value stream */
    int url_active;         /* value belongs to a URL-typed attribute */
    int url_skipping;       /* leading raw control/high-byte skip */
    int url_first[URL_WORDS]; /* htmlencode_startswith "first" flag */
    size_t url_pos[URL_WORDS];
    int url_hit;

    /* classification state shared with the one-shot path */
    xss_attr_type_t attr;
    int fatal;
    injection_result_t result;
} stream_ctx_t;

struct libinjection_xss_stream {
    stream_ctx_t ctx[STREAM_CONTEXTS];
    int all_contexts;
    int finished;
    injection_result_t stored;
    size_t bytes_accepted;
    size_t bytes_worked;   /* byte visits across every active context */
    size_t alloc_current;
    size_t alloc_peak;
};

/* the whole stream, all five contexts included, must fit in 32 KiB */
typedef char stream_memory_fit[(sizeof(struct libinjection_xss_stream) <=
                                STREAM_MEMORY_LIMIT)
                                   ? 1
                                   : -1];

static void step_byte(stream_ctx_t *ctx, unsigned char b);

/* Mirrors h5_is_white(): it is built on strchr(" \t\n\v\f\r", ch),
 * and strchr() with ch == '\0' matches the literal's terminating NUL,
 * so a NUL byte counts as whitespace here too.  The tag-name state
 * keeps its own explicit NUL rule (NULs inside a name are ignored),
 * exactly like the h5 lexer. */
static int stream_is_white(unsigned char ch) {
    return ch == STREAM_CHAR_NULL || ch == ' ' || ch == 0x09 ||
           ch == 0x0A || ch == 0x0B || ch == 0x0C || ch == 0x0D;
}

static int stream_is_alpha(unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

static int stream_is_hexdigit(unsigned char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
           (ch >= 'A' && ch <= 'F');
}

static int stream_hexval(unsigned char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    return ch - 'A' + 10;
}

static void name_reset(stream_ctx_t *ctx) {
    libinjection_xss_name_matcher_init(&ctx->name);
}

static void comment_push(stream_ctx_t *ctx, unsigned char b) {
    if (ctx->comment_len < COMMENT_HEAD_CAP) {
        ctx->comment_head[ctx->comment_head_len++] = (char)b;
        ctx->comment_len += 1;
    } else if (ctx->comment_len == COMMENT_HEAD_CAP) {
        /* remember that the head is no longer the whole comment */
        ctx->comment_len += 1;
    }
    if (b == '`') {
        ctx->comment_backtick = 1;
    }
}

static void comment_reset(stream_ctx_t *ctx) {
    ctx->comment_head_len = 0;
    ctx->comment_len = 0;
    ctx->comment_backtick = 0;
    ctx->comment_nulls = 0;
}

static void ctx_fatal(stream_ctx_t *ctx, injection_result_t r) {
    ctx->fatal = 1;
    ctx->result = r;
}

/*
 * Token finishers.  They mirror the branches of
 * libinjection_xss_classify_token() but read the bounded summaries the
 * stream keeps instead of a contiguous token; every table lookup still
 * happens in libinjection_xss.c.
 */

/* any emitted token other than ATTR_VALUE clears the pending attr */
static void token_clear_attr(stream_ctx_t *ctx) {
    ctx->attr = XSS_ATTR_NONE;
}

/* h5_state_tag_name_close() runs as an entry action: it clears
 * is_close, emits the closing token and returns to data.  Only the
 * transitions that h5 routes through that state take this path; the
 * before-attribute-name and quoted-value '>' handlers return to data
 * directly and deliberately leave is_close untouched. */
static void tag_close_action(stream_ctx_t *ctx) {
    ctx->is_close = 0;
    token_clear_attr(ctx);
    ctx->state = ST_DATA;
}

static void tag_name_finish(stream_ctx_t *ctx) {
    if (libinjection_xss_name_matcher_is_tag(&ctx->name)) {
        ctx_fatal(ctx, LIBINJECTION_RESULT_TRUE);
    }
    name_reset(ctx);
    token_clear_attr(ctx);
}

static void attr_name_finish(stream_ctx_t *ctx) {
    ctx->attr = libinjection_xss_name_matcher_attr(&ctx->name);
    name_reset(ctx);
}

static void comment_finish(stream_ctx_t *ctx) {
    injection_result_t r = libinjection_xss_classify_comment(
        ctx->comment_head, ctx->comment_len, ctx->comment_backtick);
    if (r != LIBINJECTION_RESULT_FALSE) {
        ctx_fatal(ctx, r);
    }
    comment_reset(ctx);
    token_clear_attr(ctx);
}

/* ---- URL scheme matching over decoded value bytes -------------------- */

static void url_reset(stream_ctx_t *ctx) {
    int i;
    ctx->url_skipping = 1;
    ctx->url_hit = 0;
    for (i = 0; i < URL_WORDS; ++i) {
        ctx->url_first[i] = 1;
        ctx->url_pos[i] = 0;
    }
}

static void attr_value_begin(stream_ctx_t *ctx) {
    libinjection_xss_name_matcher_init(&ctx->value_name);
    ctx->dec_phase = DEC_RAW;
    ctx->dec_val = 0;
    ctx->url_active = (ctx->attr == XSS_ATTR_URL);
    url_reset(ctx);
}

/* fold one decoded character exactly like htmlencode_startswith() */
static void url_decoded_byte(stream_ctx_t *ctx, int cb) {
    int i;
    if (!ctx->url_active || ctx->url_hit) {
        return;
    }
    for (i = 0; i < URL_WORDS; ++i) {
        size_t pos = ctx->url_pos[i];
        if (pos >= URL_DEAD) {
            continue;
        }
        if (URL_SCHEMES[i][pos] == '\0') {
            ctx->url_hit = 1; /* fully matched on an earlier byte */
            continue;
        }
        if (ctx->url_first[i] && cb <= 32) {
            continue; /* ignore all leading whitespace/control characters */
        }
        ctx->url_first[i] = 0;
        if (cb == 0 || cb == 10) {
            continue; /* NULs and vertical tabs are ignored anywhere */
        }
        if (cb >= 'a' && cb <= 'z') {
            cb -= 0x20;
        }
        if ((char)cb != URL_SCHEMES[i][pos]) {
            ctx->url_pos[i] = URL_DEAD; /* this word can no longer match */
            continue;
        }
        pos += 1;
        ctx->url_pos[i] = pos;
        if (URL_SCHEMES[i][pos] == '\0') {
            ctx->url_hit = 1;
        }
    }
}

/* is_black_url()'s raw leading skip: c <= 32 or signed c >= 127 */
static int url_raw_skippable(unsigned char b) {
    return b <= 32u || b >= 127u;
}

/*
 * A reference that html_decode_char_at() rejects only consumed its
 * leading '&'; the remaining bytes are reprocessed raw.  The first of
 * them is always '#' (or a non-'#' byte in the AMP case), and no URL
 * scheme word contains '#' so every still-live word dies there; the
 * rest of the replay therefore needs no buffering.
 */
static void url_fail_reference(stream_ctx_t *ctx) {
    url_decoded_byte(ctx, '&');
    url_decoded_byte(ctx, '#');
}

/* feed one raw value byte: the indirect-name matcher sees raw bytes
 * (is_black_attr runs on the undecoded token); only the URL matcher
 * goes through the numeric-reference decoder. */
static void value_byte(stream_ctx_t *ctx, unsigned char b) {
    libinjection_xss_name_matcher_byte(&ctx->value_name, b);

    if (!ctx->url_active) {
        return;
    }
    if (ctx->url_skipping) {
        if (url_raw_skippable(b)) {
            return;
        }
        ctx->url_skipping = 0;
    }

    switch (ctx->dec_phase) {
    case DEC_RAW:
        if (b == '&') {
            ctx->dec_phase = DEC_AMP;
        } else {
            url_decoded_byte(ctx, (int)b);
        }
        break;

    case DEC_AMP:
        if (b == '#') {
            ctx->dec_phase = DEC_HASH;
        } else {
            /* '&' was consumed alone; reprocess this byte raw */
            url_decoded_byte(ctx, '&');
            if (b == '&') {
                ctx->dec_phase = DEC_AMP;
            } else {
                ctx->dec_phase = DEC_RAW;
                url_decoded_byte(ctx, (int)b);
            }
        }
        break;

    case DEC_HASH:
        if (b >= '0' && b <= '9') {
            ctx->dec_phase = DEC_DEC;
            ctx->dec_val = b - '0';
        } else if (b == 'x' || b == 'X') {
            ctx->dec_phase = DEC_HEXP;
        } else {
            url_fail_reference(ctx);
            ctx->dec_phase = (b == '&') ? DEC_AMP : DEC_RAW;
        }
        break;

    case DEC_HEXP:
        if (stream_is_hexdigit(b)) {
            ctx->dec_phase = DEC_HEX;
            ctx->dec_val = stream_hexval(b);
        } else {
            url_fail_reference(ctx);
            ctx->dec_phase = (b == '&') ? DEC_AMP : DEC_RAW;
        }
        break;

    case DEC_DEC:
        if (b == ';') {
            url_decoded_byte(ctx, (int)ctx->dec_val);
            ctx->dec_phase = DEC_RAW;
        } else if (b >= '0' && b <= '9') {
            ctx->dec_val = ctx->dec_val * 10 + (b - '0');
            if (ctx->dec_val > ENTITY_LIMIT) {
                /* decoder returns '&' having consumed only one byte */
                url_fail_reference(ctx);
                ctx->dec_phase = DEC_RAW;
            }
        } else {
            /* semicolon-less reference: value out, terminator reprocessed */
            url_decoded_byte(ctx, (int)ctx->dec_val);
            if (b == '&') {
                ctx->dec_phase = DEC_AMP;
            } else {
                ctx->dec_phase = DEC_RAW;
                url_decoded_byte(ctx, (int)b);
            }
        }
        break;

    case DEC_HEX:
        if (b == ';') {
            url_decoded_byte(ctx, (int)ctx->dec_val);
            ctx->dec_phase = DEC_RAW;
        } else if (stream_is_hexdigit(b)) {
            ctx->dec_val = ctx->dec_val * 16 + stream_hexval(b);
            if (ctx->dec_val > ENTITY_LIMIT) {
                url_fail_reference(ctx);
                ctx->dec_phase = DEC_RAW;
            }
        } else {
            url_decoded_byte(ctx, (int)ctx->dec_val);
            if (b == '&') {
                ctx->dec_phase = DEC_AMP;
            } else {
                ctx->dec_phase = DEC_RAW;
                url_decoded_byte(ctx, (int)b);
            }
        }
        break;
    }
}

/* EOF rules of html_decode_char_at(): a dangling reference resolves. */
static void value_flush_entity(stream_ctx_t *ctx) {
    if (!ctx->url_active) {
        return;
    }
    switch (ctx->dec_phase) {
    case DEC_DEC:
    case DEC_HEX:
        url_decoded_byte(ctx, (int)ctx->dec_val);
        break;
    case DEC_HEXP:
    case DEC_HASH:
        url_fail_reference(ctx);
        break;
    case DEC_AMP:
        url_decoded_byte(ctx, '&');
        break;
    case DEC_RAW:
        break;
    }
    ctx->dec_phase = DEC_RAW;
}

/* an attribute value token is complete */
static void value_finish(stream_ctx_t *ctx) {
    value_flush_entity(ctx);
    switch (ctx->attr) {
    case XSS_ATTR_NONE:
        break;
    case XSS_ATTR_BLACK:
    case XSS_ATTR_STYLE:
        ctx_fatal(ctx, LIBINJECTION_RESULT_TRUE);
        break;
    case XSS_ATTR_URL:
        if (ctx->url_hit) {
            ctx_fatal(ctx, LIBINJECTION_RESULT_TRUE);
        }
        break;
    case XSS_ATTR_INDIRECT:
        /* an attribute name is specified in the _value_; the matcher
         * applies the same is_black_attr rules to the raw value */
        if (libinjection_xss_name_matcher_attr(&ctx->value_name) !=
            XSS_ATTR_NONE) {
            ctx_fatal(ctx, LIBINJECTION_RESULT_TRUE);
        }
        break;
    }
    libinjection_xss_name_matcher_init(&ctx->value_name);
    ctx->url_active = 0;
    token_clear_attr(ctx);
}

/* ---- incremental re-expression of the h5 lexer states --------------- */

static void markup_replay_as_bogus(stream_ctx_t *ctx) {
    char saved[PREFIX_CAP];
    size_t n = ctx->prefix_len;
    size_t i;

    memcpy(saved, ctx->prefix, n);
    ctx->prefix_len = 0;
    comment_reset(ctx);
    ctx->state = ST_BOGUS;
    /* h5 falls back to bogus_comment without rewinding: it scans the
     * same bytes, so replay them through the bogus-state rules where a
     * '>' terminates the comment instead of becoming its content */
    for (i = 0; i < n && !ctx->fatal; ++i) {
        step_byte(ctx, (unsigned char)saved[i]);
    }
}

static void markup_decl_byte(stream_ctx_t *ctx, unsigned char b) {
    static const char doctype_word[PREFIX_CAP] = {'D', 'O', 'C', 'T',
                                                  'Y', 'P', 'E'};
    static const char cdata_word[PREFIX_CAP] = {'[', 'C', 'D', 'A',
                                                'T', 'A', '['};
    int doctype_live;
    int cdata_live;
    size_t i;

    if (ctx->prefix_len < PREFIX_CAP) {
        ctx->prefix[ctx->prefix_len++] = (char)b;
    }

    if (ctx->prefix_len >= 2 &&
        ctx->prefix[0] == (char)STREAM_CHAR_DASH &&
        ctx->prefix[1] == (char)STREAM_CHAR_DASH) {
        /* comment open consumes the two dashes */
        comment_reset(ctx);
        ctx->state = ST_COMMENT;
        return;
    }

    /* h5 can only choose doctype / [CDATA[ / bogus once seven bytes
     * are available; until then keep buffering -- the decision may
     * depend on a byte in the next block.  At EOF stream_eof() replays
     * whatever is buffered as a bogus comment, matching h5's
     * "remaining < 7" fallback. */
    if (ctx->prefix_len < PREFIX_CAP) {
        return;
    }

    doctype_live = 1;
    cdata_live = 1;
    for (i = 0; i < PREFIX_CAP; ++i) {
        unsigned char p = (unsigned char)ctx->prefix[i];
        unsigned char up = (p >= 'a' && p <= 'z') ? (unsigned char)(p - 0x20)
                                                  : p;
        if (up != (unsigned char)doctype_word[i]) {
            doctype_live = 0;
        }
        if (p != (unsigned char)cdata_word[i]) {
            cdata_live = 0;
        }
    }

    if (doctype_live) {
        ctx->state = ST_DOCTYPE;
        ctx_fatal(ctx, LIBINJECTION_RESULT_TRUE);
    } else if (cdata_live) {
        ctx->cdata_match = 0;
        ctx->state = ST_CDATA;
    } else {
        markup_replay_as_bogus(ctx);
    }
}

static void value_quote_byte(stream_ctx_t *ctx, unsigned char b,
                             unsigned char quote) {
    /* h5 does not skip a quote at position zero: it scans it like any
     * other byte, so a leading quote closes an empty value */
    if (b == quote) {
        value_finish(ctx);
        ctx->state = ST_AFTER_ATTR_VALUE;
    } else {
        value_byte(ctx, b);
    }
}

/* push back the bytes stranded by a dangling comment dash */
static void comment_unwind_dash(stream_ctx_t *ctx, unsigned char next) {
    size_t k;
    comment_push(ctx, STREAM_CHAR_DASH);
    for (k = 0; k < ctx->comment_nulls; ++k) {
        comment_push(ctx, STREAM_CHAR_NULL);
    }
    ctx->comment_nulls = 0;
    comment_push(ctx, next);
    ctx->state = ST_COMMENT;
}

/*
 * h5 backtracks a failed "--x"/"-!x" candidate to the position of the
 * second byte: the first dash (and any NULs skipped after it) become
 * comment content, while the candidate byte is rescanned.  Rescanning a
 * '-' there is what makes "--!>" terminate through its second dash.
 */
static void comment_unwind_second(stream_ctx_t *ctx) {
    size_t k;
    unsigned char second = (unsigned char)ctx->comment_second;
    comment_push(ctx, STREAM_CHAR_DASH);
    for (k = 0; k < ctx->comment_nulls; ++k) {
        comment_push(ctx, STREAM_CHAR_NULL);
    }
    ctx->comment_nulls = 0;
    ctx->state = ST_COMMENT;
    /* reprocess the candidate byte in the plain comment state */
    step_byte(ctx, second);
}

static void step_byte(stream_ctx_t *ctx, unsigned char b) {
    if (ctx->fatal || ctx->state == ST_EOF) {
        return;
    }

reprocess:
    switch (ctx->state) {
    case ST_DATA:
        if (b == STREAM_CHAR_LT) {
            ctx->state = ST_TAG_OPEN;
        }
        /* plain data text never reaches a classification rule */
        return;

    case ST_TAG_OPEN:
        if (b == STREAM_CHAR_BANG) {
            ctx->prefix_len = 0;
            ctx->state = ST_MARKUP_DECL;
        } else if (b == STREAM_CHAR_SLASH) {
            ctx->is_close = 1;
            ctx->state = ST_END_TAG_OPEN;
        } else if (b == STREAM_CHAR_QUESTION) {
            comment_reset(ctx);
            ctx->state = ST_BOGUS;
        } else if (b == STREAM_CHAR_PERCENT) {
            comment_reset(ctx);
            ctx->state = ST_BOGUS2;
        } else if (stream_is_alpha(b) || b == STREAM_CHAR_NULL) {
            /* h5 enters tag_name with this byte still unconsumed; a
             * leading NUL is the first token byte (ignored by the
             * folded view but kept in the raw head) */
            name_reset(ctx);
            libinjection_xss_name_matcher_byte(&ctx->name, b);
            ctx->state = ST_TAG_NAME;
        } else {
            /* user input mistake in configuring state.  When the
             * offending byte is itself '<', the h5 lexer re-enters data
             * state and immediately rediscovers that same '<', opening
             * another tag; anything else becomes one byte of data
             * text (which no classification rule inspects). */
            token_clear_attr(ctx);
            if (b == STREAM_CHAR_LT) {
                ctx->state = ST_TAG_OPEN;
            } else {
                ctx->state = ST_DATA;
            }
        }
        return;

    case ST_END_TAG_OPEN:
        if (b == STREAM_CHAR_GT) {
            ctx->state = ST_DATA;
        } else if (stream_is_alpha(b)) {
            name_reset(ctx);
            libinjection_xss_name_matcher_byte(&ctx->name, b);
            ctx->state = ST_TAG_NAME;
        } else {
            /* h5's end_tag_open drops into bogus_comment without
             * advancing past this byte, so it is the first byte of
             * the comment token */
            ctx->is_close = 0;
            comment_reset(ctx);
            comment_push(ctx, b);
            ctx->state = ST_BOGUS;
        }
        return;

    case ST_TAG_NAME:
        if (b == STREAM_CHAR_NULL) {
            /* IE-ism: NULs inside a tag name do not end the name (the
             * h5 lexer handles NUL before its whitespace test).  They
             * must still land in the matcher's raw head because the
             * raw-position SVG/XSL rule counts them, while the folded
             * view skips them exactly like cstrcasecmp_with_null. */
            libinjection_xss_name_matcher_byte(&ctx->name, b);
            return;
        }
        if (stream_is_white(b)) {
            tag_name_finish(ctx);
            ctx->state = ST_BEFORE_ATTR_NAME;
        } else if (b == STREAM_CHAR_SLASH) {
            tag_name_finish(ctx);
            ctx->state = ST_SELF_CLOSING;
        } else if (b == STREAM_CHAR_GT) {
            if (ctx->is_close) {
                name_reset(ctx);
                ctx->is_close = 0;
                token_clear_attr(ctx);
                ctx->state = ST_DATA;
            } else {
                /* h5's tag_name_close runs as an entry action on this
                 * same '>' byte: classify the name, clear is_close and
                 * return to data */
                tag_name_finish(ctx);
                tag_close_action(ctx);
            }
        } else {
            libinjection_xss_name_matcher_byte(&ctx->name, b);
        }
        return;

    case ST_BEFORE_ATTR_NAME:
        if (stream_is_white(b)) {
            return;
        }
        if (b == STREAM_CHAR_SLASH) {
            ctx->state = ST_SELF_CLOSING;
        } else if (b == STREAM_CHAR_GT) {
            token_clear_attr(ctx);
            ctx->state = ST_DATA;
        } else {
            name_reset(ctx);
            libinjection_xss_name_matcher_byte(&ctx->name, b);
            ctx->state = ST_ATTR_NAME;
        }
        return;

    case ST_ATTR_NAME:
        if (stream_is_white(b)) {
            attr_name_finish(ctx);
            ctx->state = ST_AFTER_ATTR_NAME;
        } else if (b == STREAM_CHAR_SLASH) {
            attr_name_finish(ctx);
            ctx->state = ST_SELF_CLOSING;
        } else if (b == STREAM_CHAR_EQUALS) {
            attr_name_finish(ctx);
            ctx->state = ST_BEFORE_ATTR_VALUE;
        } else if (b == STREAM_CHAR_GT) {
            attr_name_finish(ctx);
            tag_close_action(ctx);
        } else {
            libinjection_xss_name_matcher_byte(&ctx->name, b);
        }
        return;

    case ST_AFTER_ATTR_NAME:
        if (stream_is_white(b)) {
            return;
        }
        if (b == STREAM_CHAR_SLASH) {
            ctx->state = ST_SELF_CLOSING;
        } else if (b == STREAM_CHAR_EQUALS) {
            ctx->state = ST_BEFORE_ATTR_VALUE;
        } else if (b == STREAM_CHAR_GT) {
            tag_close_action(ctx);
        } else {
            name_reset(ctx);
            libinjection_xss_name_matcher_byte(&ctx->name, b);
            ctx->state = ST_ATTR_NAME;
        }
        return;

    case ST_BEFORE_ATTR_VALUE:
        if (stream_is_white(b)) {
            return;
        }
        attr_value_begin(ctx);
        if (b == STREAM_CHAR_DOUBLE) {
            ctx->state = ST_ATTR_VALUE_DQ;
        } else if (b == STREAM_CHAR_SINGLE) {
            ctx->state = ST_ATTR_VALUE_SQ;
        } else if (b == STREAM_CHAR_TICK) {
            ctx->state = ST_ATTR_VALUE_BQ;
        } else {
            /* h5 enters the no-quote state without consuming this
             * byte: a leading '>' closes an empty value, a leading
             * whitespace starts another attribute */
            ctx->state = ST_ATTR_VALUE_NQ;
            goto reprocess;
        }
        return;

    case ST_ATTR_VALUE_DQ:
        value_quote_byte(ctx, b, STREAM_CHAR_DOUBLE);
        return;

    case ST_ATTR_VALUE_SQ:
        value_quote_byte(ctx, b, STREAM_CHAR_SINGLE);
        return;

    case ST_ATTR_VALUE_BQ:
        value_quote_byte(ctx, b, STREAM_CHAR_TICK);
        return;

    case ST_ATTR_VALUE_NQ:
        if (stream_is_white(b)) {
            value_finish(ctx);
            ctx->state = ST_BEFORE_ATTR_NAME;
        } else if (b == STREAM_CHAR_GT) {
            /* h5 routes an unquoted value's '>' through tag_name_close
             * (which also clears is_close); quoted values do not */
            value_finish(ctx);
            tag_close_action(ctx);
        } else {
            value_byte(ctx, b);
        }
        return;

    case ST_AFTER_ATTR_VALUE:
        if (stream_is_white(b)) {
            ctx->state = ST_BEFORE_ATTR_NAME;
            return;
        }
        if (b == STREAM_CHAR_SLASH) {
            ctx->state = ST_SELF_CLOSING;
        } else if (b == STREAM_CHAR_GT) {
            token_clear_attr(ctx);
            ctx->state = ST_DATA;
        } else {
            ctx->state = ST_BEFORE_ATTR_NAME;
            goto reprocess;
        }
        return;

    case ST_SELF_CLOSING:
        if (b == STREAM_CHAR_GT) {
            token_clear_attr(ctx);
            ctx->state = ST_DATA;
        } else {
            ctx->state = ST_BEFORE_ATTR_NAME;
            goto reprocess;
        }
        return;

    case ST_COMMENT:
        if (b == STREAM_CHAR_DASH) {
            ctx->state = ST_COMMENT_DASH;
        } else {
            comment_push(ctx, b);
        }
        return;

    case ST_COMMENT_DASH:
        if (b == STREAM_CHAR_NULL) {
            /* the h5 lexer skips NULs between a dash and its next
             * decision byte; remember them so the token stays exact */
            ctx->comment_nulls += 1;
            return;
        }
        if (b == STREAM_CHAR_DASH || b == STREAM_CHAR_BANG) {
            ctx->comment_second = (char)b;
            ctx->state = ST_COMMENT_SECOND;
        } else {
            comment_unwind_dash(ctx, b);
        }
        return;

    case ST_COMMENT_SECOND:
        if (b == STREAM_CHAR_GT) {
            comment_finish(ctx);
            ctx->state = ST_DATA;
        } else {
            comment_unwind_second(ctx);
            goto reprocess;
        }
        return;

    case ST_BOGUS:
        if (b == STREAM_CHAR_GT) {
            comment_finish(ctx);
            ctx->state = ST_DATA;
        } else {
            comment_push(ctx, b);
        }
        return;

    case ST_BOGUS2:
        if (b == STREAM_CHAR_PERCENT) {
            ctx->state = ST_BOGUS2_PCT;
        } else {
            comment_push(ctx, b);
        }
        return;

    case ST_BOGUS2_PCT:
        if (b == STREAM_CHAR_GT) {
            comment_finish(ctx);
            ctx->state = ST_DATA;
        } else {
            /* the candidate '%' was ordinary content; if this byte is
             * itself '%' it immediately becomes the next candidate,
             * which is how "%%>" terminates in h5 */
            comment_push(ctx, STREAM_CHAR_PERCENT);
            if (b == STREAM_CHAR_PERCENT) {
                ctx->state = ST_BOGUS2_PCT;
            } else {
                comment_push(ctx, b);
                ctx->state = ST_BOGUS2;
            }
        }
        return;

    case ST_MARKUP_DECL:
        markup_decl_byte(ctx, b);
        return;

    case ST_CDATA:
        if (b == (unsigned char)"]]>"[ctx->cdata_match]) {
            ctx->cdata_match += 1;
            if (ctx->cdata_match == 3) {
                ctx->cdata_match = 0;
                ctx->state = ST_DATA;
            }
        } else {
            ctx->cdata_match = (b == STREAM_CHAR_RIGHTB) ? 1 : 0;
        }
        return;

    case ST_DOCTYPE:
        if (b == STREAM_CHAR_GT) {
            ctx->state = ST_DATA;
        }
        return;

    case ST_EOF:
        return;
    }
}

/* the lexer's EOF actions; only finish() ever reaches these */
static void stream_eof(stream_ctx_t *ctx) {
    size_t k;

    if (ctx->fatal || ctx->state == ST_EOF) {
        return;
    }
    switch (ctx->state) {
    case ST_TAG_NAME:
        /* at EOF the h5 lexer emits TAG_NAME_OPEN even for is_close */
        tag_name_finish(ctx);
        break;
    case ST_ATTR_NAME:
        attr_name_finish(ctx);
        break;
    case ST_ATTR_VALUE_DQ:
    case ST_ATTR_VALUE_SQ:
    case ST_ATTR_VALUE_BQ:
    case ST_ATTR_VALUE_NQ:
        value_finish(ctx);
        break;
    case ST_COMMENT:
        comment_finish(ctx);
        break;
    case ST_COMMENT_DASH:
        /* the dangling dash and its NULs belong to the EOF comment */
        comment_push(ctx, STREAM_CHAR_DASH);
        for (k = 0; k < ctx->comment_nulls; ++k) {
            comment_push(ctx, STREAM_CHAR_NULL);
        }
        ctx->comment_nulls = 0;
        comment_finish(ctx);
        break;
    case ST_COMMENT_SECOND:
        comment_push(ctx, STREAM_CHAR_DASH);
        for (k = 0; k < ctx->comment_nulls; ++k) {
            comment_push(ctx, STREAM_CHAR_NULL);
        }
        ctx->comment_nulls = 0;
        comment_push(ctx, (unsigned char)ctx->comment_second);
        comment_finish(ctx);
        break;
    case ST_BOGUS:
    case ST_BOGUS2:
        comment_finish(ctx);
        break;
    case ST_BOGUS2_PCT:
        comment_push(ctx, STREAM_CHAR_PERCENT);
        comment_finish(ctx);
        break;
    case ST_MARKUP_DECL:
        markup_replay_as_bogus(ctx);
        /* a '>' inside the replayed prefix may already have closed
         * the comment; only flush when h5 would still be scanning */
        if (ctx->state == ST_BOGUS || ctx->state == ST_BOGUS2 ||
            ctx->state == ST_BOGUS2_PCT) {
            comment_finish(ctx);
        }
        break;
    case ST_CDATA:
        /* h5 emits one final DATA_TEXT token, which no rule inspects */
        break;
    default:
        /* data, tag open, end tag open, before/after attribute name,
         * self-closing, before attribute value, after quoted value,
         * doctype: the h5 lexer emits nothing at EOF */
        break;
    }
    ctx->state = ST_EOF;
}

/* ---- public API ------------------------------------------------------ */

static void ctx_init(stream_ctx_t *ctx, enum html5_flags flag) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->attr = XSS_ATTR_NONE;
    ctx->result = LIBINJECTION_RESULT_FALSE;
    libinjection_xss_name_matcher_init(&ctx->name);
    libinjection_xss_name_matcher_init(&ctx->value_name);
    switch (flag) {
    case DATA_STATE:
        ctx->state = ST_DATA;
        break;
    case VALUE_NO_QUOTE:
        ctx->state = ST_BEFORE_ATTR_NAME;
        break;
    case VALUE_SINGLE_QUOTE:
        ctx->state = ST_ATTR_VALUE_SQ;
        break;
    case VALUE_DOUBLE_QUOTE:
        ctx->state = ST_ATTR_VALUE_DQ;
        break;
    case VALUE_BACK_QUOTE:
        ctx->state = ST_ATTR_VALUE_BQ;
        break;
    }
}

libinjection_xss_stream *libinjection_xss_stream_new(int flags) {
    static const enum html5_flags contexts[STREAM_CONTEXTS] = {
        DATA_STATE, VALUE_NO_QUOTE, VALUE_SINGLE_QUOTE, VALUE_DOUBLE_QUOTE,
        VALUE_BACK_QUOTE};
    libinjection_xss_stream *stream;
    int i;

    if (flags != -1 && (flags < DATA_STATE || flags > VALUE_BACK_QUOTE)) {
        return NULL;
    }

    /* one allocation holds the stream and every per-context summary */
    stream = (libinjection_xss_stream *)calloc(1, sizeof(*stream));
    if (stream == NULL) {
        return NULL;
    }
    stream->all_contexts = (flags == -1);
    stream->stored = LIBINJECTION_RESULT_FALSE;
    stream->alloc_current = sizeof(*stream);
    stream->alloc_peak = sizeof(*stream);

    if (stream->all_contexts) {
        for (i = 0; i < STREAM_CONTEXTS; ++i) {
            ctx_init(&stream->ctx[i], contexts[i]);
        }
    } else {
        ctx_init(&stream->ctx[0], (enum html5_flags)flags);
    }
    return stream;
}

int libinjection_xss_stream_feed(libinjection_xss_stream *stream,
                                 const char *data, size_t len) {
    size_t i;
    int active;
    int c;

    if (stream == NULL || stream->finished) {
        return -1;
    }
    if (len > 0 && data == NULL) {
        return -1;
    }
    if (len == 0) {
        /* zero-length blocks do not touch parser state */
        return 0;
    }

    active = stream->all_contexts ? STREAM_CONTEXTS : 1;
    for (i = 0; i < len; ++i) {
        unsigned char b = (unsigned char)data[i];
        for (c = 0; c < active; ++c) {
            stream_ctx_t *ctx = &stream->ctx[c];
            ctx->byte_count += 1;
            step_byte(ctx, b);
            stream->bytes_worked += 1;
        }
    }
    stream->bytes_accepted += len;
    return 0;
}

injection_result_t libinjection_xss_stream_finish(
    libinjection_xss_stream *stream) {
    int active;
    int i;

    if (stream == NULL) {
        return LIBINJECTION_RESULT_ERROR;
    }
    if (stream->finished) {
        return stream->stored;
    }
    stream->finished = 1;

    active = stream->all_contexts ? STREAM_CONTEXTS : 1;
    for (i = 0; i < active; ++i) {
        stream_ctx_t *ctx = &stream->ctx[i];
        stream_eof(ctx);
        if (ctx->result != LIBINJECTION_RESULT_FALSE) {
            stream->stored = ctx->result;
            return stream->stored;
        }
    }
    stream->stored = LIBINJECTION_RESULT_FALSE;
    return stream->stored;
}

void libinjection_xss_stream_free(libinjection_xss_stream *stream) {
    if (stream != NULL) {
        /* everything lived inline in this single allocation */
        free(stream);
    }
}

/* ---- test introspection (not part of the detection contract) -------- */

size_t libinjection_xss_stream_bytes_accepted(
    const libinjection_xss_stream *stream) {
    return stream == NULL ? 0 : stream->bytes_accepted;
}

size_t libinjection_xss_stream_bytes_worked(
    const libinjection_xss_stream *stream) {
    return stream == NULL ? 0 : stream->bytes_worked;
}

size_t libinjection_xss_stream_alloc_peak(
    const libinjection_xss_stream *stream) {
    return stream == NULL ? 0 : stream->alloc_peak;
}

size_t libinjection_xss_stream_struct_size(void) {
    return sizeof(struct libinjection_xss_stream);
}
