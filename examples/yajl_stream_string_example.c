/*
 * Copyright (c) 2007-2014, Lloyd Hilaiel <me@lloyd.io>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * example: stream a large string value out of an in-memory JSON
 * document using yajl_set_string_stream.
 *
 * Normally yajl accumulates every string internally and delivers it
 * with a single yajl_string callback once the closing quote is seen.
 * When a field may be megabytes of log text or a document body, that
 * buffering is exactly what a consumer wants to avoid.  With streaming
 * string callbacks the decoded content is delivered in fragments as it
 * arrives, so the parser's memory usage stays bounded.
 */

#include <yajl/yajl_parse.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t values;     /* streamed string values seen            */
    size_t fragments;  /* fragments delivered                    */
    size_t bytes;      /* total decoded bytes                    */
} summary;

/* called once when the opening quote of a string value is seen */
static int on_begin(void * ctx)
{
    summary * s = (summary *) ctx;
    s->values++;
    return 1; /* nonzero: keep parsing */
}

/* called one or more times with the next chunk of decoded UTF-8
 * content.  The bytes are only borrowed: consume them here (write them
 * to a file, feed a hash, ...) and do not keep the pointer. */
static int on_fragment(void * ctx, const unsigned char * bytes,
                       size_t length)
{
    summary * s = (summary *) ctx;
    s->fragments++;
    s->bytes += length;
    /* a real consumer would, for example, fwrite(bytes, 1, length, f) */
    (void) bytes;
    return 1;
}

/* called once when the closing quote of the string value is seen */
static int on_end(void * ctx)
{
    (void) ctx;
    return 1;
}

int main(void)
{
    /* a JSON document held fully in memory; imagine the "log" field
     * being megabytes long */
    static const char json[] =
        "{\"log\":\"line one\\nline two\\nline three\"}";
    /* feed it to the parser in small pieces, as if it were arriving
     * over the network */
    const size_t chunkSize = 7;

    summary s;
    yajl_string_stream_callbacks streamCallbacks;
    yajl_handle hand;
    yajl_status stat;
    size_t offset;

    memset(&s, 0, sizeof(s));

    streamCallbacks.begin = on_begin;
    streamCallbacks.fragment = on_fragment;
    streamCallbacks.end = on_end;

    /* no classic yajl_callbacks needed for this example: NULL is fine */
    hand = yajl_alloc(NULL, NULL, &s);
    if (hand == NULL) {
        fprintf(stderr, "yajl_alloc failed\n");
        return 1;
    }

    /* streaming must be enabled before the first yajl_parse */
    if (!yajl_set_string_stream(hand, &streamCallbacks)) {
        fprintf(stderr, "could not enable string streaming\n");
        yajl_free(hand);
        return 1;
    }

    stat = yajl_status_ok;
    for (offset = 0; offset < sizeof(json) - 1; offset += chunkSize) {
        size_t n = sizeof(json) - 1 - offset;
        if (n > chunkSize) n = chunkSize;
        stat = yajl_parse(hand, (const unsigned char *) json + offset, n);
        if (stat != yajl_status_ok) break;
    }
    if (stat == yajl_status_ok) stat = yajl_complete_parse(hand);

    if (stat != yajl_status_ok) {
        unsigned char * err = yajl_get_error(hand, 1,
                                             (const unsigned char *) json,
                                             sizeof(json) - 1);
        fprintf(stderr, "%s", (const char *) err);
        yajl_free_error(hand, err);
        yajl_free(hand);
        return 1;
    }

    printf("streamed %zu value(s) in %zu fragment(s), %zu byte(s) total\n",
           s.values, s.fragments, s.bytes);

    yajl_free(hand);
    return 0;
}
