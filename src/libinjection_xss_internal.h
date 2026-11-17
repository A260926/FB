/*
 * Internal XSS classification primitives shared by the one-shot
 * detector (libinjection_xss.c) and the incremental, block-at-a-time
 * stream detector (libinjection_xss_stream.c).
 *
 * The blacklists live exactly once, in libinjection_xss.c; both entry
 * paths must keep using the same tables and the same rules.
 */

#ifndef LIBINJECTION_XSS_INTERNAL_H
#define LIBINJECTION_XSS_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "libinjection_html5.h"
#include "libinjection_xss.h"

/* attribute categories, kept in sync with the private enum in
 * libinjection_xss.c */
typedef enum xss_attr_type {
    XSS_ATTR_NONE,
    XSS_ATTR_BLACK,
    XSS_ATTR_URL,
    XSS_ATTR_STYLE,
    XSS_ATTR_INDIRECT
} xss_attr_type_t;

/* Names longer than this cannot match any blacklist entry: the longest
 * entry in the tables is 44 bytes.  Stream summaries only retain this
 * many bytes, so memory use is independent of input length. */
#define XSS_NAME_MAX 48

/*
 * Incremental, resumable matcher for tag names, attribute names and
 * indirect attribute values.  It accumulates the same folded byte
 * view (NULs skipped, ASCII lower-cased) that cstrcasecmp_with_null()
 * uses, plus the raw head needed for the few raw-position rules.  The
 * actual table lookups live in libinjection_xss.c next to the tables.
 */
typedef struct xss_name_matcher {
    char raw[XSS_NAME_MAX];
    char folded[XSS_NAME_MAX];
    size_t raw_len;      /* saturates at XSS_NAME_MAX + 1 */
    size_t folded_len;   /* bytes stored in folded */
    size_t folded_total; /* total non-NUL bytes seen, uncapped */
} xss_name_matcher_t;

void libinjection_xss_name_matcher_init(xss_name_matcher_t *m);
void libinjection_xss_name_matcher_byte(xss_name_matcher_t *m,
                                        unsigned char b);
int libinjection_xss_name_matcher_is_tag(xss_name_matcher_t *m);
xss_attr_type_t libinjection_xss_name_matcher_attr(xss_name_matcher_t *m);

/*
 * The TAG_COMMENT rule, factored out so both paths share it.  "head"
 * holds the first six bytes of the comment (every comment rule only
 * inspects those), "total_len" the full comment length and "backtick"
 * records whether a '`' occurs anywhere in the comment.
 */
injection_result_t libinjection_xss_classify_comment(const char *head,
                                                     size_t total_len,
                                                     int backtick);

/*
 * Classification of one fully emitted h5 token.
 *
 * Returns LIBINJECTION_RESULT_TRUE when the token fires a rule (the
 * caller must stop and report an injection), LIBINJECTION_RESULT_FALSE
 * to continue.  "attr" carries the pending attribute category between
 * the ATTR_NAME and the following ATTR_VALUE token.
 */
injection_result_t libinjection_xss_classify_token(enum html5_type token_type,
                                                    const char *token_start,
                                                    size_t token_len,
                                                    xss_attr_type_t *attr);

/* Table lookups used by the stream's incremental name matcher. */
xss_attr_type_t libinjection_xss_lookup_attr(const char *s, size_t len);
int libinjection_xss_lookup_tag(const char *s, size_t len);
/* BLACKTAG table only; the raw-position SVG/XSL rule is applied by the
 * matcher itself against the raw token head. */
int libinjection_xss_blacktag_table(const char *s, size_t len);

/* Test-only introspection of the stream implementation. */
size_t libinjection_xss_stream_bytes_accepted(
    const libinjection_xss_stream *stream);
size_t libinjection_xss_stream_bytes_worked(
    const libinjection_xss_stream *stream);
size_t libinjection_xss_stream_alloc_peak(
    const libinjection_xss_stream *stream);
size_t libinjection_xss_stream_struct_size(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBINJECTION_XSS_INTERNAL_H */
