#ifndef LIBINJECTION_XSS
#define LIBINJECTION_XSS

#ifdef __cplusplus
extern "C" {
#endif

/**
 * HEY THIS ISN'T DONE
 */

/* pull in size_t */

#include "libinjection.h"
#include "libinjection_html5.h"
#include <string.h>

injection_result_t libinjection_is_xss(const char *s, size_t len, int flags);

/*
 * Incremental, block-at-a-time XSS detection.
 *
 * This is the same heuristic as libinjection_xss(), but the input does
 * not have to exist as one contiguous buffer: feed it any number of
 * byte blocks and only hand it the final verdict at finish().  Block
 * boundaries are not input boundaries -- names, quoted values, numeric
 * character references, comment terminators and declaration prefixes
 * may span blocks, and EOF handling from the original HTML lexer runs
 * only at finish().
 *
 * The type is opaque on purpose; a stream plus everything it allocates
 * stays under 32 KiB regardless of how many bytes are fed to it.
 */
typedef struct libinjection_xss_stream libinjection_xss_stream;

/*
 * Create a stream pinned to one parser context.
 *
 * flags is one of the five html5_flags values, or -1 to run all five
 * contexts in exactly the order libinjection_xss() tries them.  Any
 * other value returns NULL.
 */
libinjection_xss_stream *libinjection_xss_stream_new(int flags);

/*
 * Append one block.  The stream copies what it needs before returning;
 * the caller may overwrite or free "data" immediately afterwards.
 *
 * Returns 0 when the bytes were accepted (0 is an acknowledgement, not
 * a safety verdict), -1 on a call error (NULL stream/finished stream,
 * or a non-NULL length with a NULL pointer).  Zero-length blocks do not
 * change parser state.
 */
int libinjection_xss_stream_feed(libinjection_xss_stream *stream,
                                 const char *data, size_t len);

/*
 * Apply the original lexer's EOF handling and return the verdict.  The
 * result is identical to running the one-shot API on the same byte
 * sequence with the same context (or, with -1, the same context order
 * and error precedence as libinjection_xss).  Repeated calls return the
 * stored result; feeding after finish() is an error.
 */
injection_result_t libinjection_xss_stream_finish(
    libinjection_xss_stream *stream);

/* Release the stream; NULL is accepted. */
void libinjection_xss_stream_free(libinjection_xss_stream *stream);

#ifdef __cplusplus
}
#endif
#endif
