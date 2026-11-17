/*
 * Incremental XSS scanning example.
 *
 * libinjection_xss_stream_* is the block-at-a-time form of the
 * existing libinjection_xss() heuristic.  It applies the same HTML
 * lexer states and the same classification rules; the only difference
 * is that the input can arrive in pieces that are never reassembled.
 *
 * Important: a FALSE result means "this heuristic did not recognize an
 * injection".  It is not proof that the content is safe.  The detector
 * is a heuristic, not an HTML sanitizer, and that is unchanged by
 * feeding the input incrementally.
 */

#include "libinjection_xss.h"

#include <stdio.h>
#include <string.h>

/* Scan one logical input that is already split into blocks.  The
 * blocks stay independent: nothing here concatenates them. */
static int scan_blocks(const char *const *blocks, size_t nblocks,
                       int flags) {
    libinjection_xss_stream *stream =
        libinjection_xss_stream_new(flags);
    size_t i;
    injection_result_t result;

    if (stream == NULL) {
        /* only an invalid flags value reaches here */
        fprintf(stderr, "invalid parser context: %d\n", flags);
        return -1;
    }

    for (i = 0; i < nblocks; ++i) {
        size_t len = strlen(blocks[i]);
        /* 0 means the bytes were accepted; it is NOT a safety verdict.
         * The stream copies what it needs, so the block may be reused
         * or freed immediately afterwards. */
        if (libinjection_xss_stream_feed(stream, blocks[i], len) != 0) {
            libinjection_xss_stream_free(stream);
            return -1;
        }
    }

    /* Only finish() applies the lexer's end-of-input rules and yields
     * the verdict; repeating it just returns the same stored result. */
    result = libinjection_xss_stream_finish(stream);
    libinjection_xss_stream_free(stream);
    return (int)result;
}

int main(void) {
    /* The same payload split at different points; the split itself must
     * not change the verdict. */
    static const char *const split_danger[] = {"<a href='jav", "ascript:x'>"};
    static const char *const split_clean[] = {"<p>ordinary ", "text</p>"};

    /* -1: try every parser context in libinjection_xss()'s own order */
    int danger = scan_blocks(split_danger, 2, -1);
    int clean = scan_blocks(split_clean, 2, -1);

    printf("split dangerous payload: %s (1=heuristic matched an injection)\n",
           danger == LIBINJECTION_RESULT_TRUE ? "TRUE" : "FALSE");
    printf("split ordinary document: %s\n",
           clean == LIBINJECTION_RESULT_FALSE ? "FALSE" : "TRUE");

    if (danger != LIBINJECTION_RESULT_TRUE ||
        clean != LIBINJECTION_RESULT_FALSE) {
        return 1;
    }

    /* A FALSE result only means the heuristic found nothing; it never
     * certifies that untrusted input is safe to render. */
    printf("FALSE means 'no heuristic match', never 'proven safe'.\n");
    return 0;
}
