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

#include "yajl_lex.h"
#include "yajl_buf.h"

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>

#ifdef YAJL_LEXER_DEBUG
static const char *
tokToStr(yajl_tok tok)
{
    switch (tok) {
        case yajl_tok_bool: return "bool";
        case yajl_tok_colon: return "colon";
        case yajl_tok_comma: return "comma";
        case yajl_tok_eof: return "eof";
        case yajl_tok_error: return "error";
        case yajl_tok_left_brace: return "brace";
        case yajl_tok_left_bracket: return "bracket";
        case yajl_tok_null: return "null";
        case yajl_tok_integer: return "integer";
        case yajl_tok_double: return "double";
        case yajl_tok_right_brace: return "brace";
        case yajl_tok_right_bracket: return "bracket";
        case yajl_tok_string: return "string";
        case yajl_tok_string_with_escapes: return "string_with_escapes";
    }
    return "unknown";
}
#endif

/* Impact of the stream parsing feature on the lexer:
 *
 * YAJL support stream parsing.  That is, the ability to parse the first
 * bits of a chunk of JSON before the last bits are available (still on
 * the network or disk).  This makes the lexer more complex.  The
 * responsibility of the lexer is to handle transparently the case where
 * a chunk boundary falls in the middle of a token.  This is
 * accomplished is via a buffer and a character reading abstraction.
 *
 * Overview of implementation
 *
 * When we lex to end of input string before end of token is hit, we
 * copy all of the input text composing the token into our lexBuf.
 *
 * Every time we read a character, we do so through the readChar function.
 * readChar's responsibility is to handle pulling all chars from the buffer
 * before pulling chars from input text
 */

struct yajl_lexer_t {
    /* the overal line and char offset into the data */
    size_t lineOff;
    size_t charOff;

    /* error */
    yajl_lex_error error;

    /* a input buffer to handle the case where a token is spread over
     * multiple chunks */
    yajl_buf buf;

    /* in the case where we have data in the lexBuf, bufOff holds
     * the current offset into the lexBuf. */
    size_t bufOff;

    /* are we using the lex buf? */
    unsigned int bufInUse;

    /* shall we allow comments? */
    unsigned int allowComments;

    /* shall we validate utf8 inside strings? */
    unsigned int validateUTF8;

    /* state for streamed string values (see yajl_lex_string_stream_next).
     * Only a bounded amount of state is kept across chunks: a partial
     * escape sequence, a partial utf8 sequence or one pending high
     * surrogate.  Decoded content is accumulated in streamBuf and handed
     * to the parser whenever it reaches the delivery threshold. */
    unsigned int streamEsc;         /* 0: none, 1: backslash seen, 2: in \u */
    unsigned int streamHexCount;    /* hex digits read for the current \u */
    unsigned int streamHexValue;    /* value of the current \u escape */
    unsigned int streamSurrogate;   /* pending high surrogate, 0 if none */
    unsigned int streamClosePending;/* closing quote consumed, end owed */
    unsigned char streamUtf8[4];    /* bytes of an in-progress utf8 char */
    unsigned int streamUtf8Len;     /* bytes of it buffered so far */
    unsigned int streamUtf8Need;    /* continuation bytes still expected */
    unsigned char streamUtf8Min;    /* allowed range of the second byte */
    unsigned char streamUtf8Max;
    yajl_buf streamBuf;             /* decoded bytes pending delivery */

    yajl_alloc_funcs * alloc;
};

#define readChar(lxr, txt, off)                      \
    (((lxr)->bufInUse && yajl_buf_len((lxr)->buf) && lxr->bufOff < yajl_buf_len((lxr)->buf)) ? \
     (*((const unsigned char *) yajl_buf_data((lxr)->buf) + ((lxr)->bufOff)++)) : \
     ((txt)[(*(off))++]))

#define unreadChar(lxr, off) ((*(off) > 0) ? (*(off))-- : ((lxr)->bufOff--))

yajl_lexer
yajl_lex_alloc(yajl_alloc_funcs * alloc,
               unsigned int allowComments, unsigned int validateUTF8)
{
    yajl_lexer lxr = (yajl_lexer) YA_MALLOC(alloc, sizeof(struct yajl_lexer_t));
    memset((void *) lxr, 0, sizeof(struct yajl_lexer_t));
    lxr->buf = yajl_buf_alloc(alloc);
    lxr->streamBuf = yajl_buf_alloc(alloc);
    lxr->allowComments = allowComments;
    lxr->validateUTF8 = validateUTF8;
    lxr->alloc = alloc;
    return lxr;
}

void
yajl_lex_free(yajl_lexer lxr)
{
    yajl_buf_free(lxr->buf);
    yajl_buf_free(lxr->streamBuf);
    YA_FREE(lxr->alloc, lxr);
    return;
}

/* a lookup table which lets us quickly determine three things:
 * VEC - valid escaped control char
 * note.  the solidus '/' may be escaped or not.
 * IJC - invalid json char
 * VHC - valid hex char
 * NFP - needs further processing (from a string scanning perspective)
 * NUC - needs utf8 checking when enabled (from a string scanning perspective)
 */
#define VEC 0x01
#define IJC 0x02
#define VHC 0x04
#define NFP 0x08
#define NUC 0x10

static const char charLookupTable[256] =
{
/*00*/ IJC    , IJC    , IJC    , IJC    , IJC    , IJC    , IJC    , IJC    ,
/*08*/ IJC    , IJC    , IJC    , IJC    , IJC    , IJC    , IJC    , IJC    ,
/*10*/ IJC    , IJC    , IJC    , IJC    , IJC    , IJC    , IJC    , IJC    ,
/*18*/ IJC    , IJC    , IJC    , IJC    , IJC    , IJC    , IJC    , IJC    ,

/*20*/ 0      , 0      , NFP|VEC|IJC, 0      , 0      , 0      , 0      , 0      ,
/*28*/ 0      , 0      , 0      , 0      , 0      , 0      , 0      , VEC    ,
/*30*/ VHC    , VHC    , VHC    , VHC    , VHC    , VHC    , VHC    , VHC    ,
/*38*/ VHC    , VHC    , 0      , 0      , 0      , 0      , 0      , 0      ,

/*40*/ 0      , VHC    , VHC    , VHC    , VHC    , VHC    , VHC    , 0      ,
/*48*/ 0      , 0      , 0      , 0      , 0      , 0      , 0      , 0      ,
/*50*/ 0      , 0      , 0      , 0      , 0      , 0      , 0      , 0      ,
/*58*/ 0      , 0      , 0      , 0      , NFP|VEC|IJC, 0      , 0      , 0      ,

/*60*/ 0      , VHC    , VEC|VHC, VHC    , VHC    , VHC    , VEC|VHC, 0      ,
/*68*/ 0      , 0      , 0      , 0      , 0      , 0      , VEC    , 0      ,
/*70*/ 0      , 0      , VEC    , 0      , VEC    , 0      , 0      , 0      ,
/*78*/ 0      , 0      , 0      , 0      , 0      , 0      , 0      , 0      ,

       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,
       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,
       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,
       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,

       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,
       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,
       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,
       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,

       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,
       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,
       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,
       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,

       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,
       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,
       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,
       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC
};

/** process a variable length utf8 encoded codepoint.
 *
 *  returns:
 *    yajl_tok_string - if valid utf8 char was parsed and offset was
 *                      advanced
 *    yajl_tok_eof - if end of input was hit before validation could
 *                   complete
 *    yajl_tok_error - if invalid utf8 was encountered
 *
 *  NOTE: on error the offset will point to the first char of the
 *  invalid utf8 */
#define UTF8_CHECK_EOF if (*offset >= jsonTextLen) { return yajl_tok_eof; }

static yajl_tok
yajl_lex_utf8_char(yajl_lexer lexer, const unsigned char * jsonText,
                   size_t jsonTextLen, size_t * offset,
                   unsigned char curChar)
{
    if (curChar <= 0x7f) {
        /* single byte */
        return yajl_tok_string;
    } else if ((curChar >> 5) == 0x6) {
        /* two byte */
        UTF8_CHECK_EOF;
        curChar = readChar(lexer, jsonText, offset);
        if ((curChar >> 6) == 0x2) return yajl_tok_string;
    } else if ((curChar >> 4) == 0x0e) {
        /* three byte */
        UTF8_CHECK_EOF;
        curChar = readChar(lexer, jsonText, offset);
        if ((curChar >> 6) == 0x2) {
            UTF8_CHECK_EOF;
            curChar = readChar(lexer, jsonText, offset);
            if ((curChar >> 6) == 0x2) return yajl_tok_string;
        }
    } else if ((curChar >> 3) == 0x1e) {
        /* four byte */
        UTF8_CHECK_EOF;
        curChar = readChar(lexer, jsonText, offset);
        if ((curChar >> 6) == 0x2) {
            UTF8_CHECK_EOF;
            curChar = readChar(lexer, jsonText, offset);
            if ((curChar >> 6) == 0x2) {
                UTF8_CHECK_EOF;
                curChar = readChar(lexer, jsonText, offset);
                if ((curChar >> 6) == 0x2) return yajl_tok_string;
            }
        }
    }

    return yajl_tok_error;
}

/* lex a string.  input is the lexer, pointer to beginning of
 * json text, and start of string (offset).
 * a token is returned which has the following meanings:
 * yajl_tok_string: lex of string was successful.  offset points to
 *                  terminating '"'.
 * yajl_tok_eof: end of text was encountered before we could complete
 *               the lex.
 * yajl_tok_error: embedded in the string were unallowable chars.  offset
 *               points to the offending char
 */
#define STR_CHECK_EOF \
if (*offset >= jsonTextLen) { \
   tok = yajl_tok_eof; \
   goto finish_string_lex; \
}

/** scan a string for interesting characters that might need further
 *  review.  return the number of chars that are uninteresting and can
 *  be skipped.
 * (lth) hi world, any thoughts on how to make this routine faster? */
static size_t
yajl_string_scan(const unsigned char * buf, size_t len, int utf8check)
{
    unsigned char mask = IJC|NFP|(utf8check ? NUC : 0);
    size_t skip = 0;
    while (skip < len && !(charLookupTable[*buf] & mask))
    {
        skip++;
        buf++;
    }
    return skip;
}

static yajl_tok
yajl_lex_string(yajl_lexer lexer, const unsigned char * jsonText,
                size_t jsonTextLen, size_t * offset)
{
    yajl_tok tok = yajl_tok_error;
    int hasEscapes = 0;

    for (;;) {
        unsigned char curChar;

        /* now jump into a faster scanning routine to skip as much
         * of the buffers as possible */
        {
            const unsigned char * p;
            size_t len;

            if ((lexer->bufInUse && yajl_buf_len(lexer->buf) &&
                 lexer->bufOff < yajl_buf_len(lexer->buf)))
            {
                p = ((const unsigned char *) yajl_buf_data(lexer->buf) +
                     (lexer->bufOff));
                len = yajl_buf_len(lexer->buf) - lexer->bufOff;
                lexer->bufOff += yajl_string_scan(p, len, lexer->validateUTF8);
            }
            else if (*offset < jsonTextLen)
            {
                p = jsonText + *offset;
                len = jsonTextLen - *offset;
                *offset += yajl_string_scan(p, len, lexer->validateUTF8);
            }
        }

        STR_CHECK_EOF;

        curChar = readChar(lexer, jsonText, offset);

        /* quote terminates */
        if (curChar == '"') {
            tok = yajl_tok_string;
            break;
        }
        /* backslash escapes a set of control chars, */
        else if (curChar == '\\') {
            hasEscapes = 1;
            STR_CHECK_EOF;

            /* special case \u */
            curChar = readChar(lexer, jsonText, offset);
            if (curChar == 'u') {
                unsigned int i = 0;

                for (i=0;i<4;i++) {
                    STR_CHECK_EOF;
                    curChar = readChar(lexer, jsonText, offset);
                    if (!(charLookupTable[curChar] & VHC)) {
                        /* back up to offending char */
                        unreadChar(lexer, offset);
                        lexer->error = yajl_lex_string_invalid_hex_char;
                        goto finish_string_lex;
                    }
                }
            } else if (!(charLookupTable[curChar] & VEC)) {
                /* back up to offending char */
                unreadChar(lexer, offset);
                lexer->error = yajl_lex_string_invalid_escaped_char;
                goto finish_string_lex;
            }
        }
        /* when not validating UTF8 it's a simple table lookup to determine
         * if the present character is invalid */
        else if(charLookupTable[curChar] & IJC) {
            /* back up to offending char */
            unreadChar(lexer, offset);
            lexer->error = yajl_lex_string_invalid_json_char;
            goto finish_string_lex;
        }
        /* when in validate UTF8 mode we need to do some extra work */
        else if (lexer->validateUTF8) {
            yajl_tok t = yajl_lex_utf8_char(lexer, jsonText, jsonTextLen,
                                            offset, curChar);

            if (t == yajl_tok_eof) {
                tok = yajl_tok_eof;
                goto finish_string_lex;
            } else if (t == yajl_tok_error) {
                lexer->error = yajl_lex_string_invalid_utf8;
                goto finish_string_lex;
            }
        }
        /* accept it, and move on */
    }
  finish_string_lex:
    /* tell our buddy, the parser, wether he needs to process this string
     * again */
    if (hasEscapes && tok == yajl_tok_string) {
        tok = yajl_tok_string_with_escapes;
    }

    return tok;
}

/* --- streamed string values -----------------------------------------
 *
 * The routines below implement incremental lexing of a string in value
 * position (see yajl_set_string_stream).  Unlike yajl_lex_string, the
 * content is decoded and handed to the parser in fragments as it
 * arrives, so the memory footprint does not depend on the length of the
 * string.  Streamed strings are always validated (utf8, escapes,
 * control characters and surrogates), independent of the
 * validateUTF8 lexer setting.
 */

/* deliver a fragment once this many decoded bytes were gathered.  The
 * delivery buffer never grows past this plus a single character. */
#define YAJL_STREAM_FRAG_THRESHOLD 4096

/* classify a utf8 lead byte.  returns 1 and sets *need (continuation
 * bytes expected) and the valid range for the second byte when the lead
 * byte may start a well formed, non-overlong utf8 sequence encoding a
 * unicode scalar value, 0 otherwise. */
static int
yajl_stream_utf8_lead(unsigned char c, unsigned int * need,
                      unsigned char * min, unsigned char * max)
{
    *min = 0x80;
    *max = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) { *need = 1; return 1; }
    if (c == 0xE0) { *need = 2; *min = 0xA0; return 1; }
    if (c == 0xED) { *need = 2; *max = 0x9F; return 1; }
    if (c >= 0xE1 && c <= 0xEF) { *need = 2; return 1; }
    if (c == 0xF0) { *need = 3; *min = 0x90; return 1; }
    if (c >= 0xF1 && c <= 0xF3) { *need = 3; return 1; }
    if (c == 0xF4) { *need = 3; *max = 0x8F; return 1; }
    return 0;
}

/* encode a unicode scalar value as utf8, returns the number of bytes */
static unsigned int
yajl_stream_utf8_encode(unsigned int cp, unsigned char * out)
{
    if (cp < 0x80) {
        out[0] = (unsigned char) cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (unsigned char) (0xC0 | (cp >> 6));
        out[1] = (unsigned char) (0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (unsigned char) (0xE0 | (cp >> 12));
        out[1] = (unsigned char) (0x80 | ((cp >> 6) & 0x3F));
        out[2] = (unsigned char) (0x80 | (cp & 0x3F));
        return 3;
    } else {
        out[0] = (unsigned char) (0xF0 | (cp >> 18));
        out[1] = (unsigned char) (0x80 | ((cp >> 12) & 0x3F));
        out[2] = (unsigned char) (0x80 | ((cp >> 6) & 0x3F));
        out[3] = (unsigned char) (0x80 | (cp & 0x3F));
        return 4;
    }
}

yajl_tok
yajl_lex_string_stream_next(yajl_lexer lexer,
                            const unsigned char * jsonText,
                            size_t jsonTextLen, size_t * offset,
                            const unsigned char ** outBuf, size_t * outLen)
{
    unsigned char c;

    *outBuf = NULL;
    *outLen = 0;

    /* the closing quote was consumed during a previous call but the
     * final fragment had not been delivered yet */
    if (lexer->streamClosePending) {
        lexer->streamClosePending = 0;
        return yajl_tok_string_stream_end;
    }

    for (;;) {
        /* complete a pending \uXXXX escape */
        while (lexer->streamEsc == 2) {
            unsigned int v;
            if (*offset >= jsonTextLen) goto need_more_input;
            c = jsonText[(*offset)++];
            if (!(charLookupTable[c] & VHC)) {
                (*offset)--;
                lexer->error = yajl_lex_string_invalid_hex_char;
                return yajl_tok_error;
            }
            v = c - '0';
            if (c >= 'a') v = c - 'a' + 10;
            else if (c >= 'A') v = c - 'A' + 10;
            lexer->streamHexValue = (lexer->streamHexValue << 4) | v;
            if (++lexer->streamHexCount < 4) continue;
            lexer->streamEsc = 0;
            /* a full \uXXXX value was read */
            {
                unsigned int cp = lexer->streamHexValue;
                unsigned char utf8[4];
                unsigned int n;
                if (lexer->streamSurrogate) {
                    /* a high surrogate must be followed by a low one */
                    if (cp < 0xDC00 || cp > 0xDFFF) {
                        lexer->error = yajl_lex_string_isolated_surrogate;
                        return yajl_tok_error;
                    }
                    cp = 0x10000 + ((lexer->streamSurrogate - 0xD800) << 10)
                         + (cp - 0xDC00);
                    lexer->streamSurrogate = 0;
                } else if (cp >= 0xD800 && cp <= 0xDBFF) {
                    /* high surrogate: wait for the low one */
                    lexer->streamSurrogate = cp;
                    continue;
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    lexer->error = yajl_lex_string_isolated_surrogate;
                    return yajl_tok_error;
                }
                n = yajl_stream_utf8_encode(cp, utf8);
                yajl_buf_append(lexer->streamBuf, utf8, n);
                if (yajl_buf_len(lexer->streamBuf) >=
                    YAJL_STREAM_FRAG_THRESHOLD) goto deliver;
            }
        }

        /* complete a pending simple escape */
        if (lexer->streamEsc == 1) {
            unsigned char d;
            if (*offset >= jsonTextLen) goto need_more_input;
            c = jsonText[(*offset)++];
            if (c == 'u') {
                lexer->streamEsc = 2;
                lexer->streamHexCount = 0;
                lexer->streamHexValue = 0;
                continue;
            }
            if (lexer->streamSurrogate) {
                /* a high surrogate must be followed by \uDC00-\uDFFF */
                (*offset)--;
                lexer->error = yajl_lex_string_isolated_surrogate;
                return yajl_tok_error;
            }
            switch (c) {
                case '"':  d = '"';  break;
                case '\\': d = '\\'; break;
                case '/':  d = '/';  break;
                case 'b':  d = '\b'; break;
                case 'f':  d = '\f'; break;
                case 'n':  d = '\n'; break;
                case 'r':  d = '\r'; break;
                case 't':  d = '\t'; break;
                default:
                    (*offset)--;
                    lexer->error = yajl_lex_string_invalid_escaped_char;
                    return yajl_tok_error;
            }
            lexer->streamEsc = 0;
            yajl_buf_append(lexer->streamBuf, &d, 1);
            if (yajl_buf_len(lexer->streamBuf) >=
                YAJL_STREAM_FRAG_THRESHOLD) goto deliver;
        }

        /* complete a pending multi-byte utf8 sequence */
        while (lexer->streamUtf8Need) {
            unsigned char lo = 0x80, hi = 0xBF;
            if (*offset >= jsonTextLen) goto need_more_input;
            if (lexer->streamUtf8Len == 1) {
                lo = lexer->streamUtf8Min;
                hi = lexer->streamUtf8Max;
            }
            c = jsonText[(*offset)++];
            if (c < lo || c > hi) {
                (*offset)--;
                lexer->error = yajl_lex_string_invalid_utf8;
                return yajl_tok_error;
            }
            lexer->streamUtf8[lexer->streamUtf8Len++] = c;
            lexer->streamUtf8Need--;
        }
        if (lexer->streamUtf8Len) {
            yajl_buf_append(lexer->streamBuf, lexer->streamUtf8,
                            lexer->streamUtf8Len);
            lexer->streamUtf8Len = 0;
            if (yajl_buf_len(lexer->streamBuf) >=
                YAJL_STREAM_FRAG_THRESHOLD) goto deliver;
        }

        /* fast path: copy a run of ordinary single-byte characters */
        if (*offset < jsonTextLen) {
            size_t run = yajl_string_scan(jsonText + *offset,
                                          jsonTextLen - *offset, 1);
            if (run) {
                if (lexer->streamSurrogate) {
                    lexer->error = yajl_lex_string_isolated_surrogate;
                    return yajl_tok_error;
                }
                /* fill the delivery buffer, handing off full buffers as
                 * we go so memory stays bounded */
                while (run) {
                    size_t take =
                        YAJL_STREAM_FRAG_THRESHOLD -
                        yajl_buf_len(lexer->streamBuf);
                    if (take > run) take = run;
                    yajl_buf_append(lexer->streamBuf, jsonText + *offset,
                                    take);
                    *offset += take;
                    run -= take;
                    if (yajl_buf_len(lexer->streamBuf) >=
                        YAJL_STREAM_FRAG_THRESHOLD) goto deliver;
                }
                continue;
            }
        }

        if (*offset >= jsonTextLen) goto need_more_input;

        c = jsonText[(*offset)++];

        if (c == '"') {
            if (lexer->streamSurrogate) {
                (*offset)--;
                lexer->error = yajl_lex_string_isolated_surrogate;
                return yajl_tok_error;
            }
            if (yajl_buf_len(lexer->streamBuf)) {
                /* deliver the final fragment first, end comes next */
                lexer->streamClosePending = 1;
                goto deliver;
            }
            return yajl_tok_string_stream_end;
        }
        if (c == '\\') {
            lexer->streamEsc = 1;
            continue;
        }
        if (charLookupTable[c] & IJC) {
            (*offset)--;
            lexer->error = yajl_lex_string_invalid_json_char;
            return yajl_tok_error;
        }
        /* a multi-byte utf8 character begins here */
        if (lexer->streamSurrogate) {
            (*offset)--;
            lexer->error = yajl_lex_string_isolated_surrogate;
            return yajl_tok_error;
        }
        {
            unsigned int need;
            unsigned char mn, mx;
            if (!yajl_stream_utf8_lead(c, &need, &mn, &mx)) {
                (*offset)--;
                lexer->error = yajl_lex_string_invalid_utf8;
                return yajl_tok_error;
            }
            lexer->streamUtf8[0] = c;
            lexer->streamUtf8Len = 1;
            lexer->streamUtf8Need = need;
            lexer->streamUtf8Min = mn;
            lexer->streamUtf8Max = mx;
        }
    }

  need_more_input:
    /* hand off whatever was fully decoded; incomplete escapes, utf8
     * sequences and surrogates stay behind in fixed size state */
    if (yajl_buf_len(lexer->streamBuf)) goto deliver;
    return yajl_tok_eof;

  deliver:
    *outBuf = yajl_buf_data(lexer->streamBuf);
    *outLen = yajl_buf_len(lexer->streamBuf);
    /* the parser delivers the fragment synchronously, so the buffer may
     * be reused starting with the next call.  Note: only the length is
     * reset here (yajl_buf_clear would clobber the first byte) */
    yajl_buf_truncate(lexer->streamBuf, 0);
    return yajl_tok_string_stream_chunk;
}

#define RETURN_IF_EOF if (*offset >= jsonTextLen) return yajl_tok_eof;

static yajl_tok
yajl_lex_number(yajl_lexer lexer, const unsigned char * jsonText,
                size_t jsonTextLen, size_t * offset)
{
    /** XXX: numbers are the only entities in json that we must lex
     *       _beyond_ in order to know that they are complete.  There
     *       is an ambiguous case for integers at EOF. */

    unsigned char c;

    yajl_tok tok = yajl_tok_integer;

    RETURN_IF_EOF;
    c = readChar(lexer, jsonText, offset);

    /* optional leading minus */
    if (c == '-') {
        RETURN_IF_EOF;
        c = readChar(lexer, jsonText, offset);
    }

    /* a single zero, or a series of integers */
    if (c == '0') {
        RETURN_IF_EOF;
        c = readChar(lexer, jsonText, offset);
    } else if (c >= '1' && c <= '9') {
        do {
            RETURN_IF_EOF;
            c = readChar(lexer, jsonText, offset);
        } while (c >= '0' && c <= '9');
    } else {
        unreadChar(lexer, offset);
        lexer->error = yajl_lex_missing_integer_after_minus;
        return yajl_tok_error;
    }

    /* optional fraction (indicates this is floating point) */
    if (c == '.') {
        int numRd = 0;

        RETURN_IF_EOF;
        c = readChar(lexer, jsonText, offset);

        while (c >= '0' && c <= '9') {
            numRd++;
            RETURN_IF_EOF;
            c = readChar(lexer, jsonText, offset);
        }

        if (!numRd) {
            unreadChar(lexer, offset);
            lexer->error = yajl_lex_missing_integer_after_decimal;
            return yajl_tok_error;
        }
        tok = yajl_tok_double;
    }

    /* optional exponent (indicates this is floating point) */
    if (c == 'e' || c == 'E') {
        RETURN_IF_EOF;
        c = readChar(lexer, jsonText, offset);

        /* optional sign */
        if (c == '+' || c == '-') {
            RETURN_IF_EOF;
            c = readChar(lexer, jsonText, offset);
        }

        if (c >= '0' && c <= '9') {
            do {
                RETURN_IF_EOF;
                c = readChar(lexer, jsonText, offset);
            } while (c >= '0' && c <= '9');
        } else {
            unreadChar(lexer, offset);
            lexer->error = yajl_lex_missing_integer_after_exponent;
            return yajl_tok_error;
        }
        tok = yajl_tok_double;
    }

    /* we always go "one too far" */
    unreadChar(lexer, offset);

    return tok;
}

static yajl_tok
yajl_lex_comment(yajl_lexer lexer, const unsigned char * jsonText,
                 size_t jsonTextLen, size_t * offset)
{
    unsigned char c;

    yajl_tok tok = yajl_tok_comment;

    RETURN_IF_EOF;
    c = readChar(lexer, jsonText, offset);

    /* either slash or star expected */
    if (c == '/') {
        /* now we throw away until end of line */
        do {
            RETURN_IF_EOF;
            c = readChar(lexer, jsonText, offset);
        } while (c != '\n');
    } else if (c == '*') {
        /* now we throw away until end of comment */
        for (;;) {
            RETURN_IF_EOF;
            c = readChar(lexer, jsonText, offset);
            if (c == '*') {
                RETURN_IF_EOF;
                c = readChar(lexer, jsonText, offset);
                if (c == '/') {
                    break;
                } else {
                    unreadChar(lexer, offset);
                }
            }
        }
    } else {
        lexer->error = yajl_lex_invalid_char;
        tok = yajl_tok_error;
    }

    return tok;
}

static yajl_tok
yajl_lex_lex_impl(yajl_lexer lexer, const unsigned char * jsonText,
                  size_t jsonTextLen, size_t * offset,
                  const unsigned char ** outBuf, size_t * outLen,
                  int streamValueStrings)
{
    yajl_tok tok = yajl_tok_error;
    unsigned char c;
    size_t startOffset = *offset;

    *outBuf = NULL;
    *outLen = 0;

    for (;;) {
        assert(*offset <= jsonTextLen);

        if (*offset >= jsonTextLen) {
            tok = yajl_tok_eof;
            goto lexed;
        }

        c = readChar(lexer, jsonText, offset);

        switch (c) {
            case '{':
                tok = yajl_tok_left_bracket;
                goto lexed;
            case '}':
                tok = yajl_tok_right_bracket;
                goto lexed;
            case '[':
                tok = yajl_tok_left_brace;
                goto lexed;
            case ']':
                tok = yajl_tok_right_brace;
                goto lexed;
            case ',':
                tok = yajl_tok_comma;
                goto lexed;
            case ':':
                tok = yajl_tok_colon;
                goto lexed;
            case '\t': case '\n': case '\v': case '\f': case '\r': case ' ':
                startOffset++;
                break;
            case 't': {
                const char * want = "rue";
                do {
                    if (*offset >= jsonTextLen) {
                        tok = yajl_tok_eof;
                        goto lexed;
                    }
                    c = readChar(lexer, jsonText, offset);
                    if (c != *want) {
                        unreadChar(lexer, offset);
                        lexer->error = yajl_lex_invalid_string;
                        tok = yajl_tok_error;
                        goto lexed;
                    }
                } while (*(++want));
                tok = yajl_tok_bool;
                goto lexed;
            }
            case 'f': {
                const char * want = "alse";
                do {
                    if (*offset >= jsonTextLen) {
                        tok = yajl_tok_eof;
                        goto lexed;
                    }
                    c = readChar(lexer, jsonText, offset);
                    if (c != *want) {
                        unreadChar(lexer, offset);
                        lexer->error = yajl_lex_invalid_string;
                        tok = yajl_tok_error;
                        goto lexed;
                    }
                } while (*(++want));
                tok = yajl_tok_bool;
                goto lexed;
            }
            case 'n': {
                const char * want = "ull";
                do {
                    if (*offset >= jsonTextLen) {
                        tok = yajl_tok_eof;
                        goto lexed;
                    }
                    c = readChar(lexer, jsonText, offset);
                    if (c != *want) {
                        unreadChar(lexer, offset);
                        lexer->error = yajl_lex_invalid_string;
                        tok = yajl_tok_error;
                        goto lexed;
                    }
                } while (*(++want));
                tok = yajl_tok_null;
                goto lexed;
            }
            case '"': {
                if (streamValueStrings) {
                    /* the parser wants strings in value position
                     * streamed: report the opening quote only, the
                     * content is pulled incrementally with
                     * yajl_lex_string_stream_next.  Reset the bounded
                     * carry-over state of the streaming scanner. */
                    lexer->streamEsc = 0;
                    lexer->streamHexCount = 0;
                    lexer->streamHexValue = 0;
                    lexer->streamSurrogate = 0;
                    lexer->streamClosePending = 0;
                    lexer->streamUtf8Len = 0;
                    lexer->streamUtf8Need = 0;
                    yajl_buf_clear(lexer->streamBuf);
                    tok = yajl_tok_string_stream_begin;
                    goto lexed;
                }
                tok = yajl_lex_string(lexer, (const unsigned char *) jsonText,
                                      jsonTextLen, offset);
                goto lexed;
            }
            case '-':
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9': {
                /* integer parsing wants to start from the beginning */
                unreadChar(lexer, offset);
                tok = yajl_lex_number(lexer, (const unsigned char *) jsonText,
                                      jsonTextLen, offset);
                goto lexed;
            }
            case '/':
                /* hey, look, a probable comment!  If comments are disabled
                 * it's an error. */
                if (!lexer->allowComments) {
                    unreadChar(lexer, offset);
                    lexer->error = yajl_lex_unallowed_comment;
                    tok = yajl_tok_error;
                    goto lexed;
                }
                /* if comments are enabled, then we should try to lex
                 * the thing.  possible outcomes are
                 * - successful lex (tok_comment, which means continue),
                 * - malformed comment opening (slash not followed by
                 *   '*' or '/') (tok_error)
                 * - eof hit. (tok_eof) */
                tok = yajl_lex_comment(lexer, (const unsigned char *) jsonText,
                                       jsonTextLen, offset);
                if (tok == yajl_tok_comment) {
                    /* "error" is silly, but that's the initial
                     * state of tok.  guilty until proven innocent. */
                    tok = yajl_tok_error;
                    yajl_buf_clear(lexer->buf);
                    lexer->bufInUse = 0;
                    startOffset = *offset;
                    break;
                }
                /* hit error or eof, bail */
                goto lexed;
            default:
                lexer->error = yajl_lex_invalid_char;
                tok = yajl_tok_error;
                goto lexed;
        }
    }


  lexed:
    /* need to append to buffer if the buffer is in use or
     * if it's an EOF token */
    if (tok == yajl_tok_eof || lexer->bufInUse) {
        if (!lexer->bufInUse) yajl_buf_clear(lexer->buf);
        lexer->bufInUse = 1;
        yajl_buf_append(lexer->buf, jsonText + startOffset, *offset - startOffset);
        lexer->bufOff = 0;

        if (tok != yajl_tok_eof) {
            *outBuf = yajl_buf_data(lexer->buf);
            *outLen = yajl_buf_len(lexer->buf);
            lexer->bufInUse = 0;
        }
    } else if (tok != yajl_tok_error) {
        *outBuf = jsonText + startOffset;
        *outLen = *offset - startOffset;
    }

    /* special case for strings. skip the quotes. */
    if (tok == yajl_tok_string || tok == yajl_tok_string_with_escapes)
    {
        assert(*outLen >= 2);
        (*outBuf)++;
        *outLen -= 2;
    }


#ifdef YAJL_LEXER_DEBUG
    if (tok == yajl_tok_error) {
        printf("lexical error: %s\n",
               yajl_lex_error_to_string(yajl_lex_get_error(lexer)));
    } else if (tok == yajl_tok_eof) {
        printf("EOF hit\n");
    } else {
        printf("lexed %s: '", tokToStr(tok));
        fwrite(*outBuf, 1, *outLen, stdout);
        printf("'\n");
    }
#endif

    return tok;
}

yajl_tok
yajl_lex_lex(yajl_lexer lexer, const unsigned char * jsonText,
             size_t jsonTextLen, size_t * offset,
             const unsigned char ** outBuf, size_t * outLen)
{
    return yajl_lex_lex_impl(lexer, jsonText, jsonTextLen, offset,
                             outBuf, outLen, 0);
}

yajl_tok
yajl_lex_lex_value(yajl_lexer lexer, const unsigned char * jsonText,
                   size_t jsonTextLen, size_t * offset,
                   const unsigned char ** outBuf, size_t * outLen)
{
    return yajl_lex_lex_impl(lexer, jsonText, jsonTextLen, offset,
                             outBuf, outLen, 1);
}

const char *
yajl_lex_error_to_string(yajl_lex_error error)
{
    switch (error) {
        case yajl_lex_e_ok:
            return "ok, no error";
        case yajl_lex_string_invalid_utf8:
            return "invalid bytes in UTF8 string.";
        case yajl_lex_string_invalid_escaped_char:
            return "inside a string, '\\' occurs before a character "
                   "which it may not.";
        case yajl_lex_string_invalid_json_char:
            return "invalid character inside string.";
        case yajl_lex_string_invalid_hex_char:
            return "invalid (non-hex) character occurs after '\\u' inside "
                   "string.";
        case yajl_lex_invalid_char:
            return "invalid char in json text.";
        case yajl_lex_invalid_string:
            return "invalid string in json text.";
        case yajl_lex_missing_integer_after_exponent:
            return "malformed number, a digit is required after the exponent.";
        case yajl_lex_missing_integer_after_decimal:
            return "malformed number, a digit is required after the "
                   "decimal point.";
        case yajl_lex_missing_integer_after_minus:
            return "malformed number, a digit is required after the "
                   "minus sign.";
        case yajl_lex_unallowed_comment:
            return "probable comment found in input text, comments are "
                   "not enabled.";
        case yajl_lex_string_isolated_surrogate:
            return "isolated surrogate in string.";
    }
    return "unknown error code";
}


/** allows access to more specific information about the lexical
 *  error when yajl_lex_lex returns yajl_tok_error. */
yajl_lex_error
yajl_lex_get_error(yajl_lexer lexer)
{
    if (lexer == NULL) return (yajl_lex_error) -1;
    return lexer->error;
}

size_t yajl_lex_current_line(yajl_lexer lexer)
{
    return lexer->lineOff;
}

size_t yajl_lex_current_char(yajl_lexer lexer)
{
    return lexer->charOff;
}

yajl_tok yajl_lex_peek(yajl_lexer lexer, const unsigned char * jsonText,
                       size_t jsonTextLen, size_t offset)
{
    const unsigned char * outBuf;
    size_t outLen;
    size_t bufLen = yajl_buf_len(lexer->buf);
    size_t bufOff = lexer->bufOff;
    unsigned int bufInUse = lexer->bufInUse;
    yajl_tok tok;

    tok = yajl_lex_lex(lexer, jsonText, jsonTextLen, &offset,
                       &outBuf, &outLen);

    lexer->bufOff = bufOff;
    lexer->bufInUse = bufInUse;
    yajl_buf_truncate(lexer->buf, bufLen);

    return tok;
}
