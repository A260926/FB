/*
Copyright (c) 2015, Cisco Systems
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice, this
  list of conditions and the following disclaimer in the documentation and/or
  other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "global.h"
#include "mainenc.h"
#include "input_stream.h"

#define STREAM_MAGIC "YUV4MPEG2"
#define STREAM_MAGIC_LEN 9

struct stream_input_s {
  FILE *f;
  int owns_file;
  stream_input_format_t format;

  int width;
  int height;
  int subsample;
  int input_bitdepth;
  int frame_bitdepth;

  int skip;
  int num_frames;
  int consumed;   /* Complete input pictures seen, in display order */
  int delivered;  /* Pictures handed to the encoder, -skip excluded */
  int eof;        /* A clean producer EOF has been observed */

  /*
   * Probe bytes that have been read from the FILE but not yet consumed by
   * the picture reader.  Keeping the pushback here (instead of ungetc())
   * guarantees that probing a raw stream never drops its first pixels.
   */
  uint8_t prefix[STREAM_MAGIC_LEN];
  int prefix_len;

  /* The only two line-sized parsing buffers. */
  uint8_t hdr_buf[STREAM_PARSE_BUF_SIZE];
  uint8_t line_buf[STREAM_PARSE_BUF_SIZE];
};

static int stream_fgetc(stream_input_t *st)
{
  if (st->prefix_len > 0) {
    /* Prefix bytes are kept in natural stream order. */
    uint8_t b = st->prefix[0];
    st->prefix_len--;
    memmove(st->prefix, st->prefix + 1, st->prefix_len);
    return b;
  }
  return fgetc(st->f);
}

static size_t stream_fread(stream_input_t *st, void *dst, size_t n)
{
  uint8_t *out = (uint8_t *)dst;
  size_t got = 0;

  if (st->prefix_len > 0) {
    size_t take = min((size_t)st->prefix_len, n);
    memcpy(out, st->prefix, take);
    st->prefix_len -= (int)take;
    memmove(st->prefix, st->prefix + take, st->prefix_len);
    got += take;
  }
  while (got < n) {
    size_t r = fread(out + got, 1, n - got, st->f);
    if (r == 0)
      break;
    got += r;
  }
  return got;
}

/*
 * Read one newline-terminated line.  The terminating newline is replaced by
 * a zero byte.  Returns 1 on success, 0 on clean EOF before any byte, -1 on
 * truncation, overflow or read error.
 */
static int read_line(stream_input_t *st, uint8_t *buf)
{
  size_t len = 0;
  int c;

  for (;;) {
    c = stream_fgetc(st);
    if (c == EOF) {
      if (len == 0)
        return 0;
      fprintf(stderr, "Stream input: truncated line (no newline)\n");
      return -1;
    }
    if (len >= STREAM_PARSE_MAX_LINE - 1) {
      fprintf(stderr, "Stream input: header or frame marker longer than %d bytes\n",
              STREAM_PARSE_MAX_LINE);
      return -1;
    }
    if (c == '\n') {
      buf[len] = 0;
      return 1;
    }
    buf[len++] = (uint8_t)c;
  }
}

static int parse_y4m_header(stream_input_t *st, enc_params *params)
{
  uint8_t *line = st->hdr_buf;
  char *tok;
  int have_w = 0, have_h = 0;
  int rate_num = 0, rate_den = 0;
  int bitdepth = 8;

  /* hdr_buf already holds the magic; complete the header line. */
  size_t len = STREAM_MAGIC_LEN;
  int c;
  for (;;) {
    c = stream_fgetc(st);
    if (c == EOF) {
      fprintf(stderr, "Stream input: truncated Y4M header\n");
      return -1;
    }
    if (len >= STREAM_PARSE_MAX_LINE - 1) {
      fprintf(stderr, "Stream input: Y4M header longer than %d bytes\n",
              STREAM_PARSE_MAX_LINE);
      return -1;
    }
    if (c == '\n')
      break;
    line[len++] = (uint8_t)c;
  }
  line[len] = 0;

  if (line[STREAM_MAGIC_LEN] != ' ' && line[STREAM_MAGIC_LEN] != 0) {
    fprintf(stderr, "Stream input: invalid YUV4MPEG2 header\n");
    return -1;
  }

  tok = strtok((char *)line, " ");
  if (tok == NULL || strcmp(tok, STREAM_MAGIC) != 0) {
    fprintf(stderr, "Stream input: invalid YUV4MPEG2 header\n");
    return -1;
  }

  while ((tok = strtok(NULL, " ")) != NULL) {
    switch (tok[0]) {
    case 'W':
      st->width = (int)strtol(tok + 1, NULL, 10);
      have_w = 1;
      break;
    case 'H':
      st->height = (int)strtol(tok + 1, NULL, 10);
      have_h = 1;
      break;
    case 'F': {
      char *colon = strchr(tok + 1, ':');
      if (colon == NULL) {
        fprintf(stderr, "Stream input: malformed Y4M frame rate tag\n");
        return -1;
      }
      rate_num = (int)strtol(tok + 1, NULL, 10);
      rate_den = (int)strtol(colon + 1, NULL, 10);
      break;
    }
    case 'I':
      if (tok[1] != 'p') {
        fprintf(stderr, "Stream input: interlaced Y4M input is not supported\n");
        return -1;
      }
      break;
    case 'C':
      if (!strcmp(tok + 1, "420jpeg") || !strcmp(tok + 1, "420")) {
        bitdepth = 8;
      } else if (!strcmp(tok + 1, "420p10")) {
        bitdepth = 10;
      } else {
        fprintf(stderr, "Stream input: unsupported Y4M pixel format '%s'\n", tok + 1);
        return -1;
      }
      break;
    case 'A': {
      char *colon2 = strchr(tok + 1, ':');
      if (colon2) {
        params->aspectnum = (int)strtol(tok + 1, NULL, 10);
        params->aspectden = (int)strtol(colon2 + 1, NULL, 10);
      }
      break;
    }
    case 'X':
      /* Application specific extension tag: ignored. */
      break;
    default:
      /* Aspect ratio, colour space and remaining tags are ignored. */
      break;
    }
  }

  if (!have_w || !have_h || st->width <= 0 || st->height <= 0) {
    fprintf(stderr, "Stream input: Y4M header lacks frame dimensions\n");
    return -1;
  }
  if (rate_num > 0 && rate_den > 0)
    params->frame_rate = (float)rate_num / (float)rate_den;

  st->subsample = 420;
  st->input_bitdepth = bitdepth;
  st->frame_bitdepth = bitdepth > 8 ? 16 : 8;
  st->format = bitdepth > 8 ? STREAM_Y4M_420_10 : STREAM_Y4M_420_8;

  /* Finish configuration before any picture storage is allocated. */
  params->width = (unsigned int)st->width;
  params->height = (unsigned int)st->height;
  params->subsample = 420;
  params->input_bitdepth = bitdepth;
  params->frame_bitdepth = st->frame_bitdepth;
  params->file_headerlen = 0;
  params->frame_headerlen = 0;
  return 0;
}

stream_input_t *stream_input_open(enc_params *params)
{
  stream_input_t *st;
  int probe;

  if (params->infilestr == NULL) {
    fprintf(stderr, "Stream input: no input file specified\n");
    return NULL;
  }

  st = (stream_input_t *)calloc(1, sizeof(*st));
  if (st == NULL) {
    fprintf(stderr, "Stream input: memory allocation failed\n");
    return NULL;
  }
  st->width = (int)params->width;
  st->height = (int)params->height;
  st->subsample = (int)params->subsample;
  st->input_bitdepth = (int)params->input_bitdepth;
  st->frame_bitdepth = (int)params->frame_bitdepth;
  st->skip = params->skip;
  st->num_frames = params->num_frames;

  if (!strcmp(params->infilestr, "-")) {
    st->f = stdin;
    st->owns_file = 0;
  } else {
    st->f = fopen(params->infilestr, "rb");
    if (st->f == NULL) {
      fprintf(stderr, "Stream input: could not open '%s' for reading\n",
              params->infilestr);
      free(st);
      return NULL;
    }
    st->owns_file = 1;
  }

  /*
   * Probe for the Y4M magic without consuming raw pixels: every byte read
   * here stays in hdr_buf/prefix and remains part of the parsed stream.
   */
  probe = 0;
  while (probe < STREAM_MAGIC_LEN) {
    int c = fgetc(st->f);
    if (c == EOF)
      break;
    st->hdr_buf[probe++] = (uint8_t)c;
  }

  if (probe == STREAM_MAGIC_LEN &&
      !memcmp(st->hdr_buf, STREAM_MAGIC, STREAM_MAGIC_LEN)) {
    if (parse_y4m_header(st, params) != 0) {
      stream_input_close(st);
      return NULL;
    }
  } else {
    /* Raw 8-bit I420: geometry and frame rate come from the command line. */
    if (st->subsample != 420) {
      fprintf(stderr, "Stream input: raw streams must be I420 (subsample 420)\n");
      stream_input_close(st);
      return NULL;
    }
    if (st->input_bitdepth != 8) {
      fprintf(stderr, "Stream input: raw streams must be 8-bit I420\n");
      stream_input_close(st);
      return NULL;
    }
    st->format = STREAM_RAW_I420;
    /* Return the probe bytes, in natural order, to the reader. */
    memcpy(st->prefix, st->hdr_buf, probe);
    st->prefix_len = probe;
  }

  return st;
}

/*
 * Read exactly n bytes.  Returns 1 on success, 0 on a clean EOF before any
 * byte, -1 if the stream ends in the middle of the requested block or on a
 * read error.
 */
static int read_full(stream_input_t *st, uint8_t *dst, size_t n)
{
  size_t got = stream_fread(st, dst, n);
  if (got == n)
    return 1;
  if (got == 0 && feof(st->f))
    return 0;
  fprintf(stderr, "Stream input: truncated picture (got %u of %u bytes)\n",
          (unsigned)got, (unsigned)n);
  return -1;
}

/*
 * Discard n bytes (pictures removed by -skip); any partial block is an
 * error.  A clean EOF before the very first byte is propagated as 0 so a
 * stream that ends on a picture boundary during the -skip region finishes
 * normally.
 */
static int drain_full(stream_input_t *st, size_t n, int first_read)
{
  while (n > 0) {
    size_t chunk = min(n, sizeof(st->line_buf));
    int rc = read_full(st, st->line_buf, chunk);
    if (rc == 0)
      return first_read ? 0 : -1;
    if (rc < 0)
      return -1;
    first_read = 0;
    n -= chunk;
  }
  return 1;
}

static int read_frame_marker(stream_input_t *st)
{
  int rc = read_line(st, st->line_buf);
  if (rc <= 0)
    return rc;
  if (strncmp((char *)st->line_buf, "FRAME", 5) != 0 ||
      (st->line_buf[5] != 0 && st->line_buf[5] != ' ')) {
    fprintf(stderr, "Stream input: expected FRAME marker\n");
    return -1;
  }
  /* The variable-length tag after FRAME carries no timing information. */
  return 1;
}

/*
 * Fill one plane, replicating the byte layout and bit-depth conversion of
 * the templated read_yuv_frame() without depending on this translation
 * unit's own SAMPLE width: high-bitdepth frames use 16-bit storage even
 * though this file is compiled once, in the low-bitdepth configuration.
 * 10-bit input always implies 16-bit frame storage, so neither element
 * width needs a temporary row buffer.
 */
static int read_plane(stream_input_t *st, void *dst, int stride,
                      int width, int height, const yuv_frame_t *frame,
                      int boundary_eof_allowed)
{
  int in_bpp = frame->input_bitdepth > 8 ? 2 : 1;
  /* check_parameters() forces 16-bit frame storage whenever bitdepth > 8. */
  int wide_storage = frame->bitdepth > 8;
  int round = frame->bitdepth > frame->input_bitdepth - 1
                  ? 1 << (frame->bitdepth - frame->input_bitdepth - 1)
                  : 0;
  int i, j;

  if (!wide_storage) {
    /* 8-bit frame storage; supported inputs here are 8-bit, so read straight in. */
    uint8_t *base = (uint8_t *)dst;
    for (i = 0; i < height; ++i) {
      uint8_t *row = base + (size_t)i * stride;
      int rc = read_full(st, row, (size_t)width * in_bpp);
      if (rc == 0)
        return (boundary_eof_allowed && i == 0) ? 0 : -1;
      if (rc < 0)
        return -1;
      if (frame->input_bitdepth != frame->bitdepth) {
        for (j = width - 1; j >= 0; j--)
          row[j] = (uint8_t)(row[j] << (frame->bitdepth - frame->input_bitdepth));
      }
    }
  } else {
    uint16_t *base = (uint16_t *)dst;
    for (i = 0; i < height; ++i) {
      uint16_t *row = base + (size_t)i * stride;
      int rc = read_full(st, (uint8_t *)row, (size_t)width * in_bpp);
      if (rc == 0)
        return (boundary_eof_allowed && i == 0) ? 0 : -1;
      if (rc < 0)
        return -1;

      if (frame->input_bitdepth == frame->bitdepth)
        continue;

      if (in_bpp == 2) {
        for (j = width - 1; j >= 0; j--) {
          row[j] = frame->bitdepth > frame->input_bitdepth
                       ? (uint16_t)(row[j] << (frame->bitdepth - frame->input_bitdepth))
                       : (uint16_t)((row[j] + round) >>
                                    (frame->input_bitdepth - frame->bitdepth));
        }
      } else {
        /*
         * Packed 8-bit bytes land in the low half of the row; widen
         * backwards so the 16-bit writes never clobber unread bytes.
         */
        uint8_t *bytes = (uint8_t *)row;
        for (j = width - 1; j >= 0; j--)
          row[j] = (uint16_t)(bytes[j] << (frame->bitdepth - frame->input_bitdepth));
      }
    }
  }
  return 1;
}

static int drain_plane(stream_input_t *st, int width, int height, int bpp,
                       int boundary_eof_allowed)
{
  return drain_full(st, (size_t)width * height * bpp, boundary_eof_allowed);
}

static int read_or_drain_picture(stream_input_t *st, yuv_frame_t *frame,
                                 int deliver)
{
  int bpp = st->input_bitdepth > 8 ? 2 : 1;
  int cw = st->width >> 1;
  int ch = st->height >> 1;
  int rc;

  if (st->format != STREAM_RAW_I420) {
    rc = read_frame_marker(st);
    if (rc <= 0)
      return rc;
  }

  if (!deliver) {
    /* A clean EOF at the first byte of a skipped picture ends the stream. */
    if ((rc = drain_plane(st, st->width, st->height, bpp, 1)) <= 0)
      return rc;
    if ((rc = drain_plane(st, cw, ch, bpp, 0)) != 1)
      return rc;
    if ((rc = drain_plane(st, cw, ch, bpp, 0)) != 1)
      return rc;
    return 1;
  }

  if ((rc = read_plane(st, frame->y, frame->stride_y, st->width, st->height, frame, 1)) <= 0)
    return rc;
  if ((rc = read_plane(st, frame->u, frame->stride_c, cw, ch, frame, 0)) <= 0)
    return rc;
  if ((rc = read_plane(st, frame->v, frame->stride_c, cw, ch, frame, 0)) <= 0)
    return rc;
  return 1;
}

int stream_input_next_picture(stream_input_t *st, yuv_frame_t *frame)
{
  int rc;

  if (st == NULL)
    return -1;
  if (st->eof)
    return 0;
  if (st->delivered >= st->num_frames) {
    /* Requested count reached: do not wait for the producer's EOF. */
    return 0;
  }

  for (;;) {
    int deliver = st->consumed >= st->skip;
    rc = read_or_drain_picture(st, frame, deliver);
    if (rc <= 0) {
      if (rc == 0)
        st->eof = 1;
      return rc;
    }
    st->consumed++;
    if (deliver) {
      st->delivered++;
      frame->frame_num = st->delivered - 1;
      return 1;
    }
  }
}

int stream_input_pictures_consumed(const stream_input_t *st)
{
  return st->consumed;
}

int stream_input_pictures_delivered(const stream_input_t *st)
{
  return st->delivered;
}

int stream_input_at_eof(const stream_input_t *st)
{
  return st->eof;
}

int stream_input_width(const stream_input_t *st)
{
  return st->width;
}

int stream_input_height(const stream_input_t *st)
{
  return st->height;
}

int stream_input_input_bitdepth(const stream_input_t *st)
{
  return st->input_bitdepth;
}

int stream_input_subsample(const stream_input_t *st)
{
  return st->subsample;
}

stream_input_format_t stream_input_format(const stream_input_t *st)
{
  return st->format;
}

void stream_input_close(stream_input_t *st)
{
  if (st == NULL)
    return;
  if (st->owns_file && st->f != NULL)
    fclose(st->f);
  free(st);
}
