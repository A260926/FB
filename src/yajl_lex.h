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

#ifndef __YAJL_LEX_H__
#define __YAJL_LEX_H__

#include "api/yajl_common.h"

typedef enum {
    yajl_tok_bool,
    yajl_tok_colon,
    yajl_tok_comma,
    yajl_tok_eof,
    yajl_tok_error,
    yajl_tok_left_brace,
    yajl_tok_left_bracket,
    yajl_tok_null,
    yajl_tok_right_brace,
    yajl_tok_right_bracket,

    /* we differentiate between integers and doubles to allow the
     * parser to interpret the number without re-scanning */
    yajl_tok_integer,
    yajl_tok_double,

    /* we differentiate between strings which require further processing,
     * and strings that do not */
    yajl_tok_string,
    yajl_tok_string_with_escapes,

    /* comment tokens are not currently returned to the parser, ever */
    yajl_tok_comment,

    /* streamed string values (see yajl_set_string_stream): begin is
     * returned by yajl_lex_lex_value when the opening quote of a string
     * in value position is seen, chunk and end are returned by
     * yajl_lex_string_stream_next while the string content is consumed */
    yajl_tok_string_stream_begin,
    yajl_tok_string_stream_chunk,
    yajl_tok_string_stream_end
} yajl_tok;

typedef struct yajl_lexer_t * yajl_lexer;

yajl_lexer yajl_lex_alloc(yajl_alloc_funcs * alloc,
                          unsigned int allowComments,
                          unsigned int validateUTF8);

void yajl_lex_free(yajl_lexer lexer);

/**
 * run/continue a lex. "offset" is an input/output parameter.
 * It should be initialized to zero for a
 * new chunk of target text, and upon subsetquent calls with the same
 * target text should passed with the value of the previous invocation.
 *
 * the client may be interested in the value of offset when an error is
 * returned from the lexer.  This allows the client to render useful
 * error messages.
 *
 * When you pass the next chunk of data, context should be reinitialized
 * to zero.
 *
 * Finally, the output buffer is usually just a pointer into the jsonText,
 * however in cases where the entity being lexed spans multiple chunks,
 * the lexer will buffer the entity and the data returned will be
 * a pointer into that buffer.
 *
 * This behavior is abstracted from client code except for the performance
 * implications which require that the client choose a reasonable chunk
 * size to get adequate performance.
 */
yajl_tok yajl_lex_lex(yajl_lexer lexer, const unsigned char * jsonText,
                      size_t jsonTextLen, size_t * offset,
                      const unsigned char ** outBuf, size_t * outLen);

/** variant of yajl_lex_lex used by the parser in value position when
 *  string streaming is enabled: instead of lexing a whole string token,
 *  the opening quote yields yajl_tok_string_stream_begin and the string
 *  content is then retrieved incrementally with
 *  yajl_lex_string_stream_next.  All other tokens behave exactly as in
 *  yajl_lex_lex. */
yajl_tok yajl_lex_lex_value(yajl_lexer lexer, const unsigned char * jsonText,
                            size_t jsonTextLen, size_t * offset,
                            const unsigned char ** outBuf, size_t * outLen);

/** continue lexing a streamed string value after
 *  yajl_tok_string_stream_begin.  Returns
 *  yajl_tok_string_stream_chunk with decoded UTF-8 bytes in
 *  outBuf/outLen (never empty, never ending inside a multi-byte
 *  character; the buffer is reused on the next call),
 *  yajl_tok_string_stream_end once the closing quote was consumed,
 *  yajl_tok_eof when more input is needed, or yajl_tok_error.  Only a
 *  small constant amount of state (a partial escape, utf8 sequence or
 *  pending surrogate) is retained across calls, so memory usage does
 *  not grow with the length of the string. */
yajl_tok yajl_lex_string_stream_next(yajl_lexer lexer,
                                     const unsigned char * jsonText,
                                     size_t jsonTextLen, size_t * offset,
                                     const unsigned char ** outBuf,
                                     size_t * outLen);

/** have a peek at the next token, but don't move the lexer forward */
yajl_tok yajl_lex_peek(yajl_lexer lexer, const unsigned char * jsonText,
                       size_t jsonTextLen, size_t offset);


typedef enum {
    yajl_lex_e_ok = 0,
    yajl_lex_string_invalid_utf8,
    yajl_lex_string_invalid_escaped_char,
    yajl_lex_string_invalid_json_char,
    yajl_lex_string_invalid_hex_char,
    yajl_lex_invalid_char,
    yajl_lex_invalid_string,
    yajl_lex_missing_integer_after_decimal,
    yajl_lex_missing_integer_after_exponent,
    yajl_lex_missing_integer_after_minus,
    yajl_lex_unallowed_comment,
    yajl_lex_string_isolated_surrogate
} yajl_lex_error;

const char * yajl_lex_error_to_string(yajl_lex_error error);

/** allows access to more specific information about the lexical
 *  error when yajl_lex_lex returns yajl_tok_error. */
yajl_lex_error yajl_lex_get_error(yajl_lexer lexer);

/** get the current offset into the most recently lexed json string. */
size_t yajl_lex_current_offset(yajl_lexer lexer);

/** get the number of lines lexed by this lexer instance */
size_t yajl_lex_current_line(yajl_lexer lexer);

/** get the number of chars lexed by this lexer instance since the last
 *  \n or \r */
size_t yajl_lex_current_char(yajl_lexer lexer);

#endif
