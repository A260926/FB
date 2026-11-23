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

/*
 * Content assertions for the non-seekable streaming input mode.
 *
 * StreamInput   : parser/reader behaviour, Y4M variants and error reporting
 * StreamReorder : bounded lookahead, coding-order preservation and the
 *                 parent/child handshake that proves progress is made while
 *                 the producer still holds the write end open
 * StreamProcess : end-to-end runs of the real encoder, tail/segment
 *                 handling, truncation and 10-bit pixel consistency
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <poll.h>

#include "../task_support/check.h"
#include "../enc/mainenc.h"
#include "../enc/input_stream.h"
#include "common_frame.h"

#define TEST_W 32
#define TEST_H 32
#define TEST_FRAME_SIZE (TEST_W * TEST_H * 3 / 2)
#define TEST_FRAMES 20

static char workdir[256];

static void __attribute__((constructor)) init_workdir(void)
{
  const char *wd = getenv("TASK_WORKDIR");
  if (!wd || !*wd)
    wd = ".task-build/stream-cases";
  snprintf(workdir, sizeof(workdir), "%s", wd);
  mkdir(workdir, 0755);
}

static const char *encoder_bin(void)
{
  const char *p = getenv("THORENC_BIN");
  return p ? p : ".task-build/Thorenc";
}

static const char *decoder_bin(void)
{
  const char *p = getenv("THORDEC_BIN");
  return p ? p : ".task-build/Thordec";
}

static char *path_join(char *dst, size_t n, const char *a, const char *b)
{
  snprintf(dst, n, "%s/%s", a, b);
  return dst;
}

static void write_full_fd(int fd, const uint8_t *buf, size_t n)
{
  size_t off = 0;
  while (off < n) {
    ssize_t w = write(fd, buf + off, n - off);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      return;
    }
    off += (size_t)w;
  }
}

static int write_file(const char *path, const uint8_t *buf, size_t n)
{
  FILE *f = fopen(path, "wb");
  if (!f)
    return -1;
  if (n && fwrite(buf, 1, n, f) != n) {
    fclose(f);
    return -1;
  }
  return fclose(f);
}

static int read_file(const char *path, uint8_t **buf, size_t *len)
{
  FILE *f = fopen(path, "rb");
  long sz;
  if (!f)
    return -1;
  fseek(f, 0, SEEK_END);
  sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  *buf = malloc((size_t)sz + 1);
  if (!*buf) {
    fclose(f);
    return -1;
  }
  *len = (size_t)fread(*buf, 1, (size_t)sz, f);
  (*buf)[*len] = 0;
  fclose(f);
  return 0;
}

static int files_equal(const char *a, const char *b)
{
  uint8_t *ba, *bb;
  size_t na, nb;
  int eq;
  if (read_file(a, &ba, &na) || read_file(b, &bb, &nb))
    return 0;
  eq = na == nb && !memcmp(ba, bb, na);
  free(ba);
  free(bb);
  return eq;
}

/* Deterministic raw I420 content: every byte is a function of position. */
static void make_raw(uint8_t *dst, int frames)
{
  int n, y, x;
  for (n = 0; n < frames; n++) {
    uint8_t *f = dst + (size_t)n * TEST_FRAME_SIZE;
    for (y = 0; y < TEST_H; y++)
      for (x = 0; x < TEST_W; x++)
        f[y * TEST_W + x] = (uint8_t)(x * 3 + y * 5 + n * 17);
    for (x = 0; x < TEST_W * TEST_H / 2; x++)
      f[TEST_W * TEST_H + x] = (uint8_t)(96 + (x & 31) + n * 7);
  }
}

/* Build a Y4M byte stream over the given raw content. */
static size_t make_y4m(uint8_t *dst, size_t cap, const uint8_t *raw, int frames,
                       const char *ctag, int tagged_markers)
{
  size_t off = 0;
  int n;
  int r;
  r = snprintf((char *)dst + off, cap - off,
               "YUV4MPEG2 W%d H%d F30:1 Ip A1:1 %s Xapp=stream-test Xz=9\n",
               TEST_W, TEST_H, ctag);
  off += (size_t)r;
  for (n = 0; n < frames; n++) {
    if (tagged_markers)
      r = snprintf((char *)dst + off, cap - off, "FRAME tag=%03d variable length\n", n);
    else
      r = snprintf((char *)dst + off, cap - off, "FRAME\n");
    off += (size_t)r;
    memcpy(dst + off, raw + (size_t)n * TEST_FRAME_SIZE, TEST_FRAME_SIZE);
    off += TEST_FRAME_SIZE;
  }
  return off;
}

/* Little-endian 10-bit content from the 8-bit raw source. */
static size_t make_y4m10(uint8_t *dst, size_t cap, const uint8_t *raw, int frames)
{
  size_t off = 0;
  int n, i;
  int r;
  r = snprintf((char *)dst + off, cap - off,
               "YUV4MPEG2 W%d H%d F25:1 Ip C420p10 Xignored=1\n", TEST_W, TEST_H);
  off += (size_t)r;
  for (n = 0; n < frames; n++) {
    r = snprintf((char *)dst + off, cap - off, "FRAME\n");
    off += (size_t)r;
    for (i = 0; i < TEST_FRAME_SIZE; i++) {
      uint16_t v = (uint16_t)((raw[(size_t)n * TEST_FRAME_SIZE + i] << 2) | (n & 3));
      dst[off++] = (uint8_t)(v & 255);
      dst[off++] = (uint8_t)(v >> 8);
    }
  }
  return off;
}

static enc_params *default_params(const char *infile)
{
  enc_params *p = calloc(1, sizeof(*p));
  p->infilestr = (char *)infile;
  p->stream_input = 1;
  p->width = TEST_W;
  p->height = TEST_H;
  p->subsample = 420;
  p->input_bitdepth = 8;
  p->frame_bitdepth = 8;
  p->bitdepth = 8;
  p->frame_rate = 30.0f;
  p->skip = 0;
  p->num_frames = 600;
  p->aspectnum = p->aspectden = 1;
  return p;
}

/* Run the encoder.  When input is non-NULL it is piped to stdin in chunks. */
static int run_encoder(char *const argv[], const uint8_t *input, size_t input_len,
                       size_t chunk, const char *logpath)
{
  int in_pipe[2] = {-1, -1};
  int logfd;
  pid_t pid;
  int status = -1;

  logfd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (logfd < 0)
    return -1;
  if (input) {
    if (pipe(in_pipe))
      return -1;
  }
  pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    if (input) {
      dup2(in_pipe[0], STDIN_FILENO);
      close(in_pipe[0]);
      close(in_pipe[1]);
    }
    dup2(logfd, STDOUT_FILENO);
    dup2(logfd, STDERR_FILENO);
    execv(encoder_bin(), argv);
    _exit(127);
  }
  close(logfd);
  if (input) {
    size_t off = 0;
    close(in_pipe[0]);
    while (off < input_len) {
      size_t c = chunk && chunk < input_len - off ? chunk : input_len - off;
      write_full_fd(in_pipe[1], input + off, c);
      off += c;
    }
    close(in_pipe[1]);
  }
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    ;
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  return -1;
}

/* Count per-frame "N I/P/B ..." records and collect display numbers/types. */
static int parse_frame_log(const char *path, int *numbers, char *types, int maxn)
{
  FILE *f = fopen(path, "rb");
  char line[256];
  int count = 0;
  if (!f)
    return -1;
  while (fgets(line, sizeof(line), f) && count < maxn) {
    int num;
    char type;
    if (sscanf(line, "%d %c", &num, &type) == 2 &&
        (type == 'I' || type == 'P' || type == 'B')) {
      numbers[count] = num;
      types[count] = type;
      count++;
    }
  }
  fclose(f);
  return count;
}

static void argv_base(char buf[][128], int *argc, const char *out, const char *recon,
                      int n, int skip, int nrp, int dyadic, int hbd)
{
  int i = 0;
  snprintf(buf[i], 128, "%s", encoder_bin()); i++;
  snprintf(buf[i], 128, "-stream_input"); i++;
  snprintf(buf[i], 128, "1"); i++;
  snprintf(buf[i], 128, "-if"); i++;
  snprintf(buf[i], 128, "-"); i++;
  snprintf(buf[i], 128, "-of"); i++;
  snprintf(buf[i], 128, "%s", out); i++;
  if (recon) { snprintf(buf[i], 128, "-rf"); i++; snprintf(buf[i], 128, "%s", recon); i++; }
  snprintf(buf[i], 128, "-width"); i++; snprintf(buf[i], 128, "%d", TEST_W); i++;
  snprintf(buf[i], 128, "-height"); i++; snprintf(buf[i], 128, "%d", TEST_H); i++;
  snprintf(buf[i], 128, "-n"); i++; snprintf(buf[i], 128, "%d", n); i++;
  snprintf(buf[i], 128, "-skip"); i++; snprintf(buf[i], 128, "%d", skip); i++;
  snprintf(buf[i], 128, "-qp"); i++; snprintf(buf[i], 128, "30"); i++;
  snprintf(buf[i], 128, "-log2_sb_size"); i++; snprintf(buf[i], 128, "6"); i++;
  snprintf(buf[i], 128, "-max_num_ref"); i++; snprintf(buf[i], 128, "2"); i++;
  snprintf(buf[i], 128, "-num_reorder_pics"); i++; snprintf(buf[i], 128, "%d", nrp); i++;
  snprintf(buf[i], 128, "-dyadic_coding"); i++; snprintf(buf[i], 128, "%d", dyadic); i++;
  snprintf(buf[i], 128, "-snrcalc"); i++; snprintf(buf[i], 128, "0"); i++;
  snprintf(buf[i], 128, "-cdef"); i++; snprintf(buf[i], 128, "0"); i++;
  if (hbd) {
    snprintf(buf[i], 128, "-bitdepth"); i++; snprintf(buf[i], 128, "10"); i++;
    snprintf(buf[i], 128, "-input_bitdepth"); i++; snprintf(buf[i], 128, "10"); i++;
    snprintf(buf[i], 128, "-frame_bitdepth"); i++; snprintf(buf[i], 128, "16"); i++;
  }
  *argc = i;
}

/* ------------------------------ StreamInput ------------------------------ */

static int case_raw_pixels_preserved(void)
{
  char rawp[256];
  uint8_t *raw = malloc((size_t)TEST_FRAMES * TEST_FRAME_SIZE);
  enc_params *p;
  stream_input_t *st;
  yuv_frame_t frame;
  int f;
  make_raw(raw, TEST_FRAMES);
  path_join(rawp, sizeof(rawp), workdir, "si_raw.yuv");
  TASK_CHECK(write_file(rawp, raw, (size_t)TEST_FRAMES * TEST_FRAME_SIZE) == 0);

  p = default_params(rawp);
  st = stream_input_open(p);
  TASK_CHECK(st != NULL);
  TASK_CHECK(stream_input_format(st) == STREAM_RAW_I420);
  TASK_CHECK(stream_input_width(st) == TEST_W && stream_input_height(st) == TEST_H);

  create_yuv_frame_lbd(&frame, TEST_W, TEST_H, 420, 0, 0, 8, 8);
  for (f = 0; f < TEST_FRAMES; f++) {
    int rc = stream_input_next_picture(st, &frame);
    TASK_CHECK(rc == 1);
    TASK_CHECK(!memcmp(frame.y, raw + (size_t)f * TEST_FRAME_SIZE, TEST_W * TEST_H));
  }
  TASK_CHECK(stream_input_next_picture(st, &frame) == 0);
  TASK_CHECK(stream_input_at_eof(st));
  TASK_CHECK(stream_input_pictures_consumed(st) == TEST_FRAMES);
  TASK_CHECK(stream_input_pictures_delivered(st) == TEST_FRAMES);
  close_yuv_frame_lbd(&frame);
  stream_input_close(st);
  free(p);
  free(raw);
  return 0;
}

static int case_y4m_tags_geometry_and_order(void)
{
  char y4mp[256];
  uint8_t *raw = malloc((size_t)TEST_FRAMES * TEST_FRAME_SIZE);
  size_t cap = (size_t)TEST_FRAMES * (TEST_FRAME_SIZE + 64) + 256;
  uint8_t *y4m = malloc(cap);
  size_t ylen;
  enc_params *p;
  stream_input_t *st;
  yuv_frame_t frame;
  int f;
  make_raw(raw, TEST_FRAMES);
  ylen = make_y4m(y4m, cap, raw, TEST_FRAMES, "C420jpeg", 1);
  path_join(y4mp, sizeof(y4mp), workdir, "si_tag.y4m");
  TASK_CHECK(write_file(y4mp, y4m, ylen) == 0);

  p = default_params(y4mp);
  st = stream_input_open(p);
  TASK_CHECK(st != NULL);
  TASK_CHECK(stream_input_format(st) == STREAM_Y4M_420_8);
  TASK_CHECK(p->width == TEST_W && p->height == TEST_H);
  TASK_CHECK(p->subsample == 420 && p->input_bitdepth == 8);
  TASK_CHECK((int)(p->frame_rate + 0.5f) == 30);

  create_yuv_frame_lbd(&frame, TEST_W, TEST_H, 420, 0, 0, 8, 8);
  for (f = 0; f < TEST_FRAMES; f++) {
    TASK_CHECK(stream_input_next_picture(st, &frame) == 1);
    /* Tags must not change content or display ordering. */
    TASK_CHECK(!memcmp(frame.y, raw + (size_t)f * TEST_FRAME_SIZE, TEST_W * TEST_H));
    TASK_CHECK(frame.frame_num == f);
  }
  TASK_CHECK(stream_input_next_picture(st, &frame) == 0);
  close_yuv_frame_lbd(&frame);
  stream_input_close(st);
  free(p);
  free(raw);
  free(y4m);
  return 0;
}

static int case_y4m_x_extension_long_header(void)
{
  char y4mp[256];
  uint8_t *raw = malloc((size_t)4 * TEST_FRAME_SIZE);
  FILE *f;
  enc_params *p;
  stream_input_t *st;
  yuv_frame_t frame;
  int i, got = 0;
  make_raw(raw, 4);
  path_join(y4mp, sizeof(y4mp), workdir, "si_long.y4m");
  f = fopen(y4mp, "wb");
  TASK_CHECK(f != NULL);
  fputs("YUV4MPEG2 W32 H32 F30:1 Ip C420 ", f);
  for (i = 0; i < 60; i++)   /* > 256 bytes, still under 4096 */
    fputs("Xpadding=ignored ", f);
  fputc('\n', f);
  for (i = 0; i < 4; i++) {
    fputs("FRAME\n", f);
    fwrite(raw + (size_t)i * TEST_FRAME_SIZE, 1, TEST_FRAME_SIZE, f);
  }
  fclose(f);

  p = default_params(y4mp);
  st = stream_input_open(p);
  TASK_CHECK(st != NULL);
  TASK_CHECK(stream_input_format(st) == STREAM_Y4M_420_8);
  create_yuv_frame_lbd(&frame, TEST_W, TEST_H, 420, 0, 0, 8, 8);
  while (stream_input_next_picture(st, &frame) == 1)
    got++;
  TASK_CHECK(got == 4);
  close_yuv_frame_lbd(&frame);
  stream_input_close(st);
  free(p);
  free(raw);
  return 0;
}

static int case_y4m10_little_endian(void)
{
  char y4mp[256];
  uint8_t *raw = malloc((size_t)6 * TEST_FRAME_SIZE);
  size_t cap = 256 + (size_t)6 * TEST_FRAME_SIZE * 2 + 64;
  uint8_t *y4m = malloc(cap);
  size_t ylen;
  enc_params *p;
  stream_input_t *st;
  yuv_frame_t frame;
  int f;
  make_raw(raw, 6);
  ylen = make_y4m10(y4m, cap, raw, 6);
  path_join(y4mp, sizeof(y4mp), workdir, "si_10.y4m");
  TASK_CHECK(write_file(y4mp, y4m, ylen) == 0);

  p = default_params(y4mp);
  st = stream_input_open(p);
  TASK_CHECK(st != NULL);
  TASK_CHECK(stream_input_format(st) == STREAM_Y4M_420_10);
  TASK_CHECK(stream_input_input_bitdepth(st) == 10);
  /* The internal coding bitdepth stays command-line driven, as in the
     seekable Y4M path; only storage/input bitdepth come from the header. */
  TASK_CHECK(p->frame_bitdepth == 16 && p->input_bitdepth == 10);

  create_yuv_frame_hbd(&frame, TEST_W, TEST_H, 420, 0, 0, 10, 10);
  for (f = 0; f < 6; f++) {
    const uint16_t *y16 = (const uint16_t *)frame.y;
    uint16_t expect = (uint16_t)((raw[(size_t)f * TEST_FRAME_SIZE] << 2) | (f & 3));
    TASK_CHECK(stream_input_next_picture(st, &frame) == 1);
    TASK_CHECK(y16[0] == expect);
    TASK_CHECK(y16[TEST_W - 1] ==
               (uint16_t)((raw[(size_t)f * TEST_FRAME_SIZE + TEST_W - 1] << 2) | (f & 3)));
  }
  close_yuv_frame_hbd(&frame);
  stream_input_close(st);
  free(p);
  free(raw);
  free(y4m);
  return 0;
}

static int case_skip_and_n(void)
{
  char rawp[256];
  uint8_t *raw = malloc((size_t)TEST_FRAMES * TEST_FRAME_SIZE);
  enc_params *p;
  stream_input_t *st;
  yuv_frame_t frame;
  int f;
  make_raw(raw, TEST_FRAMES);
  path_join(rawp, sizeof(rawp), workdir, "si_skip.yuv");
  TASK_CHECK(write_file(rawp, raw, (size_t)TEST_FRAMES * TEST_FRAME_SIZE) == 0);

  p = default_params(rawp);
  p->skip = 2;
  p->num_frames = 3;
  st = stream_input_open(p);
  TASK_CHECK(st != NULL);
  create_yuv_frame_lbd(&frame, TEST_W, TEST_H, 420, 0, 0, 8, 8);
  for (f = 0; f < 3; f++) {
    TASK_CHECK(stream_input_next_picture(st, &frame) == 1);
    TASK_CHECK(!memcmp(frame.y,
                       raw + (size_t)(2 + f) * TEST_FRAME_SIZE, TEST_W * TEST_H));
  }
  /* -n reached: returns 0 without waiting for a producer EOF. */
  TASK_CHECK(stream_input_next_picture(st, &frame) == 0);
  TASK_CHECK(stream_input_pictures_delivered(st) == 3);
  TASK_CHECK(stream_input_pictures_consumed(st) == 5);
  close_yuv_frame_lbd(&frame);
  stream_input_close(st);
  free(p);
  free(raw);
  return 0;
}

static int case_fragmented_fifo(void)
{
  char fifop[256], rawp[256];
  uint8_t *raw = malloc((size_t)5 * TEST_FRAME_SIZE);
  size_t cap = 256 + (size_t)5 * (TEST_FRAME_SIZE + 64);
  uint8_t *y4m = malloc(cap);
  size_t ylen;
  enc_params *p;
  stream_input_t *st;
  yuv_frame_t frame;
  pid_t pid;
  int got = 0;
  make_raw(raw, 5);
  ylen = make_y4m(y4m, cap, raw, 5, "C420", 1);
  path_join(fifop, sizeof(fifop), workdir, "si.fifo");
  path_join(rawp, sizeof(rawp), workdir, "si_fifo_ref.yuv");
  unlink(fifop);
  TASK_CHECK(mkfifo(fifop, 0600) == 0);
  TASK_CHECK(write_file(rawp, raw, (size_t)5 * TEST_FRAME_SIZE) == 0);

  pid = fork();
  TASK_CHECK(pid >= 0);
  if (pid == 0) {
    size_t off = 0;
    int wfd = open(fifop, O_WRONLY);
    if (wfd < 0)
      _exit(2);
    while (off < ylen) {                 /* 7-byte fragmented writes */
      size_t c = ylen - off < 7 ? ylen - off : 7;
      write_full_fd(wfd, y4m + off, c);
      off += c;
    }
    close(wfd);
    _exit(0);
  }
  p = default_params(fifop);
  st = stream_input_open(p);
  TASK_CHECK(st != NULL);
  create_yuv_frame_lbd(&frame, TEST_W, TEST_H, 420, 0, 0, 8, 8);
  while (stream_input_next_picture(st, &frame) == 1) {
    TASK_CHECK(!memcmp(frame.y,
                       raw + (size_t)got * TEST_FRAME_SIZE, TEST_W * TEST_H));
    got++;
  }
  TASK_CHECK(got == 5);
  TASK_CHECK(stream_input_at_eof(st));
  waitpid(pid, NULL, 0);
  close_yuv_frame_lbd(&frame);
  stream_input_close(st);
  unlink(fifop);
  free(p);
  free(raw);
  free(y4m);
  return 0;
}

static int expect_open_failure(const uint8_t *bytes, size_t len)
{
  char p[256];
  enc_params *params;
  stream_input_t *st;
  path_join(p, sizeof(p), workdir, "si_bad.y4m");
  if (write_file(p, bytes, len) != 0)
    return 0;
  params = default_params(p);
  st = stream_input_open(params);
  free(params);
  return st == NULL;
}

static int expect_read_failure(const uint8_t *bytes, size_t len)
{
  char p[256];
  enc_params *params;
  stream_input_t *st;
  yuv_frame_t frame;
  int rc;
  path_join(p, sizeof(p), workdir, "si_bad.y4m");
  if (write_file(p, bytes, len) != 0)
    return 0;
  params = default_params(p);
  st = stream_input_open(params);
  if (!st) {
    free(params);
    return 0;
  }
  create_yuv_frame_lbd(&frame, TEST_W, TEST_H, 420, 0, 0, 8, 8);
  rc = stream_input_next_picture(st, &frame);
  close_yuv_frame_lbd(&frame);
  stream_input_close(st);
  free(params);
  return rc < 0;
}

static int case_malformed_inputs_rejected(void)
{
  static uint8_t big[4200];
  uint8_t raw[TEST_FRAME_SIZE * 2];
  uint8_t buf[64 + TEST_FRAME_SIZE];
  size_t n;
  make_raw(raw, 2);

  /* Header line longer than 4096 bytes. */
  memset(big, 'x', sizeof(big));
  memcpy(big, "YUV4MPEG2 ", 10);
  TASK_CHECK(expect_open_failure(big, sizeof(big)));

  /* Unknown pixel format. */
  n = (size_t)snprintf((char *)buf, sizeof(buf),
                       "YUV4MPEG2 W32 H32 F30:1 Ip C444\nFRAME\n");
  memcpy(buf + n, raw, TEST_FRAME_SIZE);
  TASK_CHECK(expect_open_failure(buf, n + TEST_FRAME_SIZE));

  /* Interlaced pictures. */
  n = (size_t)snprintf((char *)buf, sizeof(buf),
                       "YUV4MPEG2 W32 H32 F30:1 It C420\nFRAME\n");
  memcpy(buf + n, raw, TEST_FRAME_SIZE);
  TASK_CHECK(expect_open_failure(buf, n + TEST_FRAME_SIZE));

  /* Frame marker line longer than 4096 bytes. */
  n = (size_t)snprintf((char *)big, sizeof(big),
                       "YUV4MPEG2 W32 H32 F30:1 Ip C420\nFRAME ");
  memset(big + n, 'q', sizeof(big) - n - 1);
  big[sizeof(big) - 1] = '\n';
  TASK_CHECK(expect_read_failure(big, sizeof(big)));

  /* FRAME line cut off by EOF. */
  n = (size_t)snprintf((char *)buf, sizeof(buf),
                       "YUV4MPEG2 W32 H32 F30:1 Ip C420\nFRAM");
  TASK_CHECK(expect_read_failure(buf, n));

  /* Half a Y plane. */
  n = (size_t)snprintf((char *)buf, sizeof(buf),
                       "YUV4MPEG2 W32 H32 F30:1 Ip C420\nFRAME\n");
  memcpy(buf + n, raw, TEST_W * TEST_H / 2);
  TASK_CHECK(expect_read_failure(buf, n + TEST_W * TEST_H / 2));

  /* Raw stream ending inside a plane. */
  TASK_CHECK(expect_read_failure(raw, 123));
  return 0;
}

/* ----------------------------- StreamReorder ----------------------------- */

static int case_reorder_window_sizes(void)
{
  static const int nrps[] = {0, 1, 3, 7, 15};
  uint8_t *raw = malloc((size_t)TEST_FRAMES * TEST_FRAME_SIZE);
  char argvbuf[40][128];
  char *argv[41];
  char outp[256], logp[256], seeklog[256], seekout[256];
  int i, argc, rc, count, scount;
  int nums[64], snums[64];
  char types[64], stypes[64];
  make_raw(raw, TEST_FRAMES);

  for (i = 0; i < (int)(sizeof(nrps) / sizeof(nrps[0])); i++) {
    int nrp = nrps[i];
    path_join(outp, sizeof(outp), workdir, "ro.bit");
    path_join(logp, sizeof(logp), workdir, "ro.log");
    argv_base(argvbuf, &argc, outp, NULL, TEST_FRAMES, 0, nrp, 1, 0);
    for (int j = 0; j < argc; j++)
      argv[j] = argvbuf[j];
    argv[argc] = NULL;
    rc = run_encoder(argv, raw, (size_t)TEST_FRAMES * TEST_FRAME_SIZE, 4096, logp);
    TASK_CHECK(rc == 0);
    count = parse_frame_log(logp, nums, types, 64);
    TASK_CHECK(count == TEST_FRAMES);

    /* Same content through the seekable file entry. */
    path_join(seekout, sizeof(seekout), workdir, "ro_seek.bit");
    path_join(seeklog, sizeof(seeklog), workdir, "ro_seek.log");
    {
      char sarg[32][128];
      char *sargv[33];
      int k = 0, sargc;
      char rawp[256];
      path_join(rawp, sizeof(rawp), workdir, "ro_raw.yuv");
      write_file(rawp, raw, (size_t)TEST_FRAMES * TEST_FRAME_SIZE);
      snprintf(sarg[k], 128, "%s", encoder_bin()); k++;
      snprintf(sarg[k], 128, "-if"); k++; snprintf(sarg[k], 128, "%s", rawp); k++;
      snprintf(sarg[k], 128, "-of"); k++; snprintf(sarg[k], 128, "%s", seekout); k++;
      snprintf(sarg[k], 128, "-width"); k++; snprintf(sarg[k], 128, "%d", TEST_W); k++;
      snprintf(sarg[k], 128, "-height"); k++; snprintf(sarg[k], 128, "%d", TEST_H); k++;
      snprintf(sarg[k], 128, "-n"); k++; snprintf(sarg[k], 128, "%d", TEST_FRAMES); k++;
      snprintf(sarg[k], 128, "-qp"); k++; snprintf(sarg[k], 128, "30"); k++;
      snprintf(sarg[k], 128, "-log2_sb_size"); k++; snprintf(sarg[k], 128, "6"); k++;
      snprintf(sarg[k], 128, "-max_num_ref"); k++; snprintf(sarg[k], 128, "2"); k++;
      snprintf(sarg[k], 128, "-num_reorder_pics"); k++; snprintf(sarg[k], 128, "%d", nrp); k++;
      snprintf(sarg[k], 128, "-dyadic_coding"); k++; snprintf(sarg[k], 128, "1"); k++;
      snprintf(sarg[k], 128, "-snrcalc"); k++; snprintf(sarg[k], 128, "0"); k++;
      snprintf(sarg[k], 128, "-cdef"); k++; snprintf(sarg[k], 128, "0"); k++;
      sargc = k;
      for (k = 0; k < sargc; k++)
        sargv[k] = sarg[k];
      sargv[sargc] = NULL;
      TASK_CHECK(run_encoder(sargv, NULL, 0, 0, seeklog) == 0);
    }
    scount = parse_frame_log(seeklog, snums, stypes, 64);
    TASK_CHECK(scount == count);
    TASK_CHECK(!memcmp(nums, snums, (size_t)count * sizeof(int)));
    TASK_CHECK(!memcmp(types, stypes, (size_t)count));
    TASK_CHECK(files_equal(outp, seekout));
  }
  free(raw);
  return 0;
}

static int case_non_dyadic_reorder(void)
{
  uint8_t *raw = malloc((size_t)TEST_FRAMES * TEST_FRAME_SIZE);
  char argvbuf[40][128];
  char *argv[41];
  char outp[256], logp[256], seekout[256], seeklog[256], rawp[256];
  int argc, count, scount;
  int nums[64], snums[64];
  char types[64], stypes[64];
  make_raw(raw, TEST_FRAMES);
  path_join(rawp, sizeof(rawp), workdir, "nd_raw.yuv");
  TASK_CHECK(write_file(rawp, raw, (size_t)TEST_FRAMES * TEST_FRAME_SIZE) == 0);

  path_join(outp, sizeof(outp), workdir, "nd.bit");
  path_join(logp, sizeof(logp), workdir, "nd.log");
  argv_base(argvbuf, &argc, outp, NULL, TEST_FRAMES, 0, 3, 0, 0);
  for (int j = 0; j < argc; j++)
    argv[j] = argvbuf[j];
  argv[argc] = NULL;
  TASK_CHECK(run_encoder(argv, raw, (size_t)TEST_FRAMES * TEST_FRAME_SIZE, 123, logp) == 0);
  count = parse_frame_log(logp, nums, types, 64);
  TASK_CHECK(count == TEST_FRAMES);

  path_join(seekout, sizeof(seekout), workdir, "nd_seek.bit");
  path_join(seeklog, sizeof(seeklog), workdir, "nd_seek.log");
  {
    char sarg[32][128];
    char *sargv[33];
    int k = 0, sargc;
    snprintf(sarg[k], 128, "%s", encoder_bin()); k++;
    snprintf(sarg[k], 128, "-if"); k++; snprintf(sarg[k], 128, "%s", rawp); k++;
    snprintf(sarg[k], 128, "-of"); k++; snprintf(sarg[k], 128, "%s", seekout); k++;
    snprintf(sarg[k], 128, "-width"); k++; snprintf(sarg[k], 128, "%d", TEST_W); k++;
    snprintf(sarg[k], 128, "-height"); k++; snprintf(sarg[k], 128, "%d", TEST_H); k++;
    snprintf(sarg[k], 128, "-n"); k++; snprintf(sarg[k], 128, "%d", TEST_FRAMES); k++;
    snprintf(sarg[k], 128, "-qp"); k++; snprintf(sarg[k], 128, "30"); k++;
    snprintf(sarg[k], 128, "-log2_sb_size"); k++; snprintf(sarg[k], 128, "6"); k++;
    snprintf(sarg[k], 128, "-max_num_ref"); k++; snprintf(sarg[k], 128, "2"); k++;
    snprintf(sarg[k], 128, "-num_reorder_pics"); k++; snprintf(sarg[k], 128, "3"); k++;
    snprintf(sarg[k], 128, "-dyadic_coding"); k++; snprintf(sarg[k], 128, "0"); k++;
    snprintf(sarg[k], 128, "-snrcalc"); k++; snprintf(sarg[k], 128, "0"); k++;
    snprintf(sarg[k], 128, "-cdef"); k++; snprintf(sarg[k], 128, "0"); k++;
    sargc = k;
    for (k = 0; k < sargc; k++)
      sargv[k] = sarg[k];
    sargv[sargc] = NULL;
    TASK_CHECK(run_encoder(sargv, NULL, 0, 0, seeklog) == 0);
  }
  scount = parse_frame_log(seeklog, snums, stypes, 64);
  TASK_CHECK(scount == count);
  TASK_CHECK(!memcmp(nums, snums, (size_t)count * sizeof(int)));
  TASK_CHECK(!memcmp(types, stypes, (size_t)count));
  TASK_CHECK(files_equal(outp, seekout));
  free(raw);
  return 0;
}

/*
 * Parent/child handshake: after the producer has sent the first lookahead
 * window while keeping the write end open, the encoder must already have
 * emitted per-frame output before any later frame is sent.
 */
static int case_handshake_output_progress(void)
{
  enum { NRP = 3, GOP = NRP + 1, TOTAL = 12 };
  uint8_t *raw = malloc((size_t)TOTAL * TEST_FRAME_SIZE);
  int in_pipe[2], out_pipe[2];
  pid_t pid;
  char argvbuf[40][128];
  char *argv[41];
  int argc, flags, saw_progress = 0, count, status;
  int nums[64];
  char types[64];
  char logp[256], outp[256];
  struct pollfd pfd;
  make_raw(raw, TOTAL);

  TASK_CHECK(pipe(in_pipe) == 0 && pipe(out_pipe) == 0);
  pid = fork();
  TASK_CHECK(pid >= 0);
  if (pid == 0) {
    dup2(in_pipe[0], STDIN_FILENO);
    dup2(out_pipe[1], STDOUT_FILENO);
    dup2(out_pipe[1], STDERR_FILENO);
    close(in_pipe[0]); close(in_pipe[1]);
    close(out_pipe[0]); close(out_pipe[1]);
    path_join(outp, sizeof(outp), workdir, "hs.bit");
    argv_base(argvbuf, &argc, outp, NULL, TOTAL, 0, NRP, 1, 0);
    for (int j = 0; j < argc; j++)
      argv[j] = argvbuf[j];
    argv[argc] = NULL;
    execv(encoder_bin(), argv);
    _exit(127);
  }
  close(in_pipe[0]);
  close(out_pipe[1]);
  flags = fcntl(out_pipe[0], F_GETFL);
  fcntl(out_pipe[0], F_SETFL, flags | O_NONBLOCK);
  path_join(logp, sizeof(logp), workdir, "hs.log");
  {
    FILE *early = fopen(logp, "wb");
    TASK_CHECK(early != NULL);

    /* Send the first complete lookahead window (plus the initial I frame). */
    write_full_fd(in_pipe[1], raw, (size_t)GOP * TEST_FRAME_SIZE);

    /* With the write end still open, output progress must appear on its own. */
    pfd.fd = out_pipe[0];
    pfd.events = POLLIN;
    for (int tries = 0; tries < 200 && !saw_progress; tries++) {
      char probe[4096];
      if (poll(&pfd, 1, 50) > 0) {
        ssize_t r = read(out_pipe[0], probe, sizeof(probe));
        if (r > 0) {
          fwrite(probe, 1, (size_t)r, early);
          if (strstr(probe, " I ") != NULL)
            saw_progress = 1;
        }
      }
    }
    TASK_CHECK(saw_progress);

    /* Only now continue feeding the remaining pictures. */
    write_full_fd(in_pipe[1], raw + (size_t)GOP * TEST_FRAME_SIZE,
                  (size_t)(TOTAL - GOP) * TEST_FRAME_SIZE);
    close(in_pipe[1]);

    /* Drain the rest of stdout into the same log. */
    fcntl(out_pipe[0], F_SETFL, flags);
    for (;;) {
      char block[4096];
      ssize_t r = read(out_pipe[0], block, sizeof(block));
      if (r > 0)
        fwrite(block, 1, (size_t)r, early);
      else if (r == 0)
        break;
      else if (errno != EINTR)
        break;
    }
    fclose(early);
  }
  close(out_pipe[0]);
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    ;
  TASK_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  count = parse_frame_log(logp, nums, types, 64);
  TASK_CHECK(count == TOTAL);
  TASK_CHECK(types[0] == 'I' && nums[0] == 0);
  TASK_CHECK(types[1] == 'P' && nums[1] == GOP);
  free(raw);
  return 0;
}

/* ----------------------------- StreamProcess ----------------------------- */

static int case_tail_incomplete_gop(void)
{
  uint8_t *raw = malloc((size_t)7 * TEST_FRAME_SIZE);
  char argvbuf[40][128];
  char *argv[41];
  char outp[256], recp[256], logp[256], sout[256], srec[256], slog[256], rawp[256];
  int argc, count;
  int nums[64];
  char types[64];
  make_raw(raw, 7);
  path_join(rawp, sizeof(rawp), workdir, "tp_raw.yuv");
  TASK_CHECK(write_file(rawp, raw, (size_t)7 * TEST_FRAME_SIZE) == 0);

  path_join(outp, sizeof(outp), workdir, "tp.bit");
  path_join(recp, sizeof(recp), workdir, "tp.rec");
  path_join(logp, sizeof(logp), workdir, "tp.log");
  argv_base(argvbuf, &argc, outp, recp, 600, 0, 3, 1, 0);
  for (int j = 0; j < argc; j++)
    argv[j] = argvbuf[j];
  argv[argc] = NULL;
  TASK_CHECK(run_encoder(argv, raw, (size_t)7 * TEST_FRAME_SIZE, 257, logp) == 0);
  count = parse_frame_log(logp, nums, types, 64);
  TASK_CHECK(count == 7);
  /* First full GOP keeps reordering; the incomplete trailing GOP is turned
     into PPP, with no dropped or padded frames. */
  TASK_CHECK(types[0] == 'I' && nums[0] == 0);
  TASK_CHECK(types[1] == 'P' && nums[1] == 4);
  TASK_CHECK(types[2] == 'B' && nums[2] == 2);
  TASK_CHECK(types[4] == 'B' && nums[4] == 3);
  TASK_CHECK(types[5] == 'P' && nums[5] == 5);
  TASK_CHECK(types[6] == 'P' && nums[6] == 6);

  /* Identical to the seekable entry on the same five pictures. */
  path_join(sout, sizeof(sout), workdir, "tp_s.bit");
  path_join(srec, sizeof(srec), workdir, "tp_s.rec");
  path_join(slog, sizeof(slog), workdir, "tp_s.log");
  {
    char sarg[32][128];
    char *sargv[33];
    int k = 0, sargc;
    snprintf(sarg[k], 128, "%s", encoder_bin()); k++;
    snprintf(sarg[k], 128, "-if"); k++; snprintf(sarg[k], 128, "%s", rawp); k++;
    snprintf(sarg[k], 128, "-of"); k++; snprintf(sarg[k], 128, "%s", sout); k++;
    snprintf(sarg[k], 128, "-rf"); k++; snprintf(sarg[k], 128, "%s", srec); k++;
    snprintf(sarg[k], 128, "-width"); k++; snprintf(sarg[k], 128, "%d", TEST_W); k++;
    snprintf(sarg[k], 128, "-height"); k++; snprintf(sarg[k], 128, "%d", TEST_H); k++;
    snprintf(sarg[k], 128, "-n"); k++; snprintf(sarg[k], 128, "600"); k++;
    snprintf(sarg[k], 128, "-qp"); k++; snprintf(sarg[k], 128, "30"); k++;
    snprintf(sarg[k], 128, "-log2_sb_size"); k++; snprintf(sarg[k], 128, "6"); k++;
    snprintf(sarg[k], 128, "-max_num_ref"); k++; snprintf(sarg[k], 128, "2"); k++;
    snprintf(sarg[k], 128, "-num_reorder_pics"); k++; snprintf(sarg[k], 128, "3"); k++;
    snprintf(sarg[k], 128, "-dyadic_coding"); k++; snprintf(sarg[k], 128, "1"); k++;
    snprintf(sarg[k], 128, "-snrcalc"); k++; snprintf(sarg[k], 128, "0"); k++;
    snprintf(sarg[k], 128, "-cdef"); k++; snprintf(sarg[k], 128, "0"); k++;
    sargc = k;
    for (k = 0; k < sargc; k++)
      sargv[k] = sarg[k];
    sargv[sargc] = NULL;
    TASK_CHECK(run_encoder(sargv, NULL, 0, 0, slog) == 0);
  }
  TASK_CHECK(files_equal(outp, sout));
  TASK_CHECK(files_equal(recp, srec));
  free(raw);
  return 0;
}

static int case_skip_n_segment(void)
{
  uint8_t *raw = malloc((size_t)TEST_FRAMES * TEST_FRAME_SIZE);
  char argvbuf[40][128];
  char *argv[41];
  char outp[256], recp[256], logp[256], sout[256], srec[256], slog[256], rawp[256];
  int argc;
  make_raw(raw, TEST_FRAMES);
  path_join(rawp, sizeof(rawp), workdir, "sg_raw.yuv");
  TASK_CHECK(write_file(rawp, raw, (size_t)TEST_FRAMES * TEST_FRAME_SIZE) == 0);

  path_join(outp, sizeof(outp), workdir, "sg.bit");
  path_join(recp, sizeof(recp), workdir, "sg.rec");
  path_join(logp, sizeof(logp), workdir, "sg.log");
  argv_base(argvbuf, &argc, outp, recp, 6, 3, 3, 1, 0);
  for (int j = 0; j < argc; j++)
    argv[j] = argvbuf[j];
  argv[argc] = NULL;
  TASK_CHECK(run_encoder(argv, raw, (size_t)TEST_FRAMES * TEST_FRAME_SIZE, 512, logp) == 0);

  path_join(sout, sizeof(sout), workdir, "sg_s.bit");
  path_join(srec, sizeof(srec), workdir, "sg_s.rec");
  path_join(slog, sizeof(slog), workdir, "sg_s.log");
  {
    char sarg[32][128];
    char *sargv[33];
    int k = 0, sargc;
    snprintf(sarg[k], 128, "%s", encoder_bin()); k++;
    snprintf(sarg[k], 128, "-if"); k++; snprintf(sarg[k], 128, "%s", rawp); k++;
    snprintf(sarg[k], 128, "-of"); k++; snprintf(sarg[k], 128, "%s", sout); k++;
    snprintf(sarg[k], 128, "-rf"); k++; snprintf(sarg[k], 128, "%s", srec); k++;
    snprintf(sarg[k], 128, "-width"); k++; snprintf(sarg[k], 128, "%d", TEST_W); k++;
    snprintf(sarg[k], 128, "-height"); k++; snprintf(sarg[k], 128, "%d", TEST_H); k++;
    snprintf(sarg[k], 128, "-n"); k++; snprintf(sarg[k], 128, "6"); k++;
    snprintf(sarg[k], 128, "-skip"); k++; snprintf(sarg[k], 128, "3"); k++;
    snprintf(sarg[k], 128, "-qp"); k++; snprintf(sarg[k], 128, "30"); k++;
    snprintf(sarg[k], 128, "-log2_sb_size"); k++; snprintf(sarg[k], 128, "6"); k++;
    snprintf(sarg[k], 128, "-max_num_ref"); k++; snprintf(sarg[k], 128, "2"); k++;
    snprintf(sarg[k], 128, "-num_reorder_pics"); k++; snprintf(sarg[k], 128, "3"); k++;
    snprintf(sarg[k], 128, "-dyadic_coding"); k++; snprintf(sarg[k], 128, "1"); k++;
    snprintf(sarg[k], 128, "-snrcalc"); k++; snprintf(sarg[k], 128, "0"); k++;
    snprintf(sarg[k], 128, "-cdef"); k++; snprintf(sarg[k], 128, "0"); k++;
    sargc = k;
    for (k = 0; k < sargc; k++)
      sargv[k] = sarg[k];
    sargv[sargc] = NULL;
    TASK_CHECK(run_encoder(sargv, NULL, 0, 0, slog) == 0);
  }
  TASK_CHECK(files_equal(outp, sout));
  TASK_CHECK(files_equal(recp, srec));
  free(raw);
  return 0;
}

static int case_truncation_nonzero(void)
{
  uint8_t *raw = malloc((size_t)TEST_FRAMES * TEST_FRAME_SIZE);
  char argvbuf[40][128];
  char *argv[41];
  char outp[256], logp[256];
  int argc, rc;
  make_raw(raw, TEST_FRAMES);
  path_join(outp, sizeof(outp), workdir, "tr.bit");
  path_join(logp, sizeof(logp), workdir, "tr.log");
  argv_base(argvbuf, &argc, outp, NULL, TEST_FRAMES, 0, 3, 1, 0);
  for (int j = 0; j < argc; j++)
    argv[j] = argvbuf[j];
  argv[argc] = NULL;
  /* EOF in the middle of the third picture plane. */
  rc = run_encoder(argv, raw, (size_t)2 * TEST_FRAME_SIZE + 17, 64, logp);
  TASK_CHECK(rc != 0);
  free(raw);
  return 0;
}

static int case_hbd_y4m_vs_raw_pixels(void)
{
  uint8_t *raw8 = malloc((size_t)8 * TEST_FRAME_SIZE);
  size_t cap = 256 + (size_t)8 * TEST_FRAME_SIZE * 2 + 64;
  uint8_t *y4m = malloc(cap);
  uint8_t *raw10 = malloc((size_t)8 * TEST_FRAME_SIZE * 2);
  size_t ylen;
  char argvbuf[40][128];
  char *argv[41];
  char yp[256], rp[256], ybit[256], rbit[256], ylog[256], rlog[256];
  int argc, f, i;
  make_raw(raw8, 8);
  ylen = make_y4m10(y4m, cap, raw8, 8);
  /* The equivalent raw little-endian 10-bit stream (no Y4M framing). */
  for (f = 0; f < 8; f++)
    for (i = 0; i < TEST_FRAME_SIZE; i++) {
      uint16_t v = (uint16_t)((raw8[(size_t)f * TEST_FRAME_SIZE + i] << 2) | (f & 3));
      raw10[((size_t)f * TEST_FRAME_SIZE + i) * 2] = (uint8_t)(v & 255);
      raw10[((size_t)f * TEST_FRAME_SIZE + i) * 2 + 1] = (uint8_t)(v >> 8);
    }
  path_join(yp, sizeof(yp), workdir, "hbd.y4m");
  path_join(rp, sizeof(rp), workdir, "hbd_raw10.yuv");
  TASK_CHECK(write_file(yp, y4m, ylen) == 0);
  TASK_CHECK(write_file(rp, raw10, (size_t)8 * TEST_FRAME_SIZE * 2) == 0);

  path_join(ybit, sizeof(ybit), workdir, "hbd_y.bit");
  path_join(ylog, sizeof(ylog), workdir, "hbd_y.log");
  argv_base(argvbuf, &argc, ybit, NULL, 8, 0, 3, 1, 1);
  for (int j = 0; j < argc; j++)
    argv[j] = argvbuf[j];
  argv[argc] = NULL;
  TASK_CHECK(run_encoder(argv, y4m, ylen, 997, ylog) == 0);

  path_join(rbit, sizeof(rbit), workdir, "hbd_r.bit");
  path_join(rlog, sizeof(rlog), workdir, "hbd_r.log");
  {
    char sarg[32][128];
    char *sargv[33];
    int k = 0, sargc;
    snprintf(sarg[k], 128, "%s", encoder_bin()); k++;
    snprintf(sarg[k], 128, "-if"); k++; snprintf(sarg[k], 128, "%s", rp); k++;
    snprintf(sarg[k], 128, "-of"); k++; snprintf(sarg[k], 128, "%s", rbit); k++;
    snprintf(sarg[k], 128, "-width"); k++; snprintf(sarg[k], 128, "%d", TEST_W); k++;
    snprintf(sarg[k], 128, "-height"); k++; snprintf(sarg[k], 128, "%d", TEST_H); k++;
    snprintf(sarg[k], 128, "-n"); k++; snprintf(sarg[k], 128, "8"); k++;
    snprintf(sarg[k], 128, "-qp"); k++; snprintf(sarg[k], 128, "30"); k++;
    snprintf(sarg[k], 128, "-log2_sb_size"); k++; snprintf(sarg[k], 128, "6"); k++;
    snprintf(sarg[k], 128, "-max_num_ref"); k++; snprintf(sarg[k], 128, "2"); k++;
    snprintf(sarg[k], 128, "-num_reorder_pics"); k++; snprintf(sarg[k], 128, "3"); k++;
    snprintf(sarg[k], 128, "-dyadic_coding"); k++; snprintf(sarg[k], 128, "1"); k++;
    snprintf(sarg[k], 128, "-snrcalc"); k++; snprintf(sarg[k], 128, "0"); k++;
    snprintf(sarg[k], 128, "-cdef"); k++; snprintf(sarg[k], 128, "0"); k++;
    snprintf(sarg[k], 128, "-bitdepth"); k++; snprintf(sarg[k], 128, "10"); k++;
    snprintf(sarg[k], 128, "-input_bitdepth"); k++; snprintf(sarg[k], 128, "10"); k++;
    snprintf(sarg[k], 128, "-frame_bitdepth"); k++; snprintf(sarg[k], 128, "16"); k++;
    sargc = k;
    for (k = 0; k < sargc; k++)
      sargv[k] = sarg[k];
    sargv[sargc] = NULL;
    TASK_CHECK(run_encoder(sargv, NULL, 0, 0, rlog) == 0);
  }
  TASK_CHECK(files_equal(ybit, rbit));
  free(raw8);
  free(y4m);
  free(raw10);
  return 0;
}

static int case_decode_roundtrip(void)
{
  uint8_t *raw = malloc((size_t)9 * TEST_FRAME_SIZE);
  char argvbuf[40][128];
  char *argv[41];
  char bitp[256], recp[256], logp[256], decp[256], dlog[256];
  int argc;
  pid_t pid;
  int status;
  make_raw(raw, 9);
  path_join(bitp, sizeof(bitp), workdir, "rt.bit");
  path_join(recp, sizeof(recp), workdir, "rt.rec");
  path_join(logp, sizeof(logp), workdir, "rt.log");
  argv_base(argvbuf, &argc, bitp, recp, 9, 0, 3, 1, 0);
  for (int j = 0; j < argc; j++)
    argv[j] = argvbuf[j];
  argv[argc] = NULL;
  TASK_CHECK(run_encoder(argv, raw, (size_t)9 * TEST_FRAME_SIZE, 1024, logp) == 0);

  path_join(decp, sizeof(decp), workdir, "rt_dec.yuv");
  path_join(dlog, sizeof(dlog), workdir, "rt_dec.log");
  pid = fork();
  TASK_CHECK(pid >= 0);
  if (pid == 0) {
    int fd = open(dlog, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    execl(decoder_bin(), decoder_bin(), bitp, decp, (char *)NULL);
    _exit(127);
  }
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    ;
  TASK_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  TASK_CHECK(files_equal(recp, decp));
  free(raw);
  return 0;
}

static int case_fifo_end_to_end(void)
{
  uint8_t *raw = malloc((size_t)9 * TEST_FRAME_SIZE);
  char fifop[256], bitp[256], logp[256], refbit[256];
  pid_t producer, encoder;
  int status, argc;
  char argvbuf[40][128];
  char *argv[41];
  make_raw(raw, 9);
  path_join(fifop, sizeof(fifop), workdir, "pe.fifo");
  unlink(fifop);
  TASK_CHECK(mkfifo(fifop, 0600) == 0);

  producer = fork();
  TASK_CHECK(producer >= 0);
  if (producer == 0) {
    int fd = open(fifop, O_WRONLY);
    size_t off = 0;
    if (fd < 0)
      _exit(2);
    while (off < (size_t)9 * TEST_FRAME_SIZE) {
      size_t c = 251;
      if (c > (size_t)9 * TEST_FRAME_SIZE - off)
        c = (size_t)9 * TEST_FRAME_SIZE - off;
      write_full_fd(fd, raw + off, c);
      off += c;
    }
    close(fd);
    _exit(0);
  }
  path_join(bitp, sizeof(bitp), workdir, "pe.bit");
  path_join(logp, sizeof(logp), workdir, "pe.log");
  {
    int k = 0;
    snprintf(argvbuf[k], 128, "%s", encoder_bin()); k++;
    snprintf(argvbuf[k], 128, "-stream_input"); k++; snprintf(argvbuf[k], 128, "1"); k++;
    snprintf(argvbuf[k], 128, "-if"); k++; snprintf(argvbuf[k], 128, "%s", fifop); k++;
    snprintf(argvbuf[k], 128, "-of"); k++; snprintf(argvbuf[k], 128, "%s", bitp); k++;
    snprintf(argvbuf[k], 128, "-width"); k++; snprintf(argvbuf[k], 128, "%d", TEST_W); k++;
    snprintf(argvbuf[k], 128, "-height"); k++; snprintf(argvbuf[k], 128, "%d", TEST_H); k++;
    snprintf(argvbuf[k], 128, "-n"); k++; snprintf(argvbuf[k], 128, "9"); k++;
    snprintf(argvbuf[k], 128, "-qp"); k++; snprintf(argvbuf[k], 128, "30"); k++;
    snprintf(argvbuf[k], 128, "-log2_sb_size"); k++; snprintf(argvbuf[k], 128, "6"); k++;
    snprintf(argvbuf[k], 128, "-max_num_ref"); k++; snprintf(argvbuf[k], 128, "2"); k++;
    snprintf(argvbuf[k], 128, "-num_reorder_pics"); k++; snprintf(argvbuf[k], 128, "3"); k++;
    snprintf(argvbuf[k], 128, "-dyadic_coding"); k++; snprintf(argvbuf[k], 128, "1"); k++;
    snprintf(argvbuf[k], 128, "-snrcalc"); k++; snprintf(argvbuf[k], 128, "0"); k++;
    snprintf(argvbuf[k], 128, "-cdef"); k++; snprintf(argvbuf[k], 128, "0"); k++;
    argc = k;
    for (k = 0; k < argc; k++)
      argv[k] = argvbuf[k];
    argv[argc] = NULL;
  }
  encoder = fork();
  TASK_CHECK(encoder >= 0);
  if (encoder == 0) {
    int fd = open(logp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    execv(encoder_bin(), argv);
    _exit(127);
  }
  while (waitpid(encoder, &status, 0) < 0 && errno == EINTR)
    ;
  TASK_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  waitpid(producer, NULL, 0);
  unlink(fifop);

  path_join(refbit, sizeof(refbit), workdir, "pe_ref.bit");
  {
    char rawp[256], sarg[32][128];
    char *sargv[33];
    int k = 0, sargc;
    path_join(rawp, sizeof(rawp), workdir, "pe_raw.yuv");
    write_file(rawp, raw, (size_t)9 * TEST_FRAME_SIZE);
    snprintf(sarg[k], 128, "%s", encoder_bin()); k++;
    snprintf(sarg[k], 128, "-if"); k++; snprintf(sarg[k], 128, "%s", rawp); k++;
    snprintf(sarg[k], 128, "-of"); k++; snprintf(sarg[k], 128, "%s", refbit); k++;
    snprintf(sarg[k], 128, "-width"); k++; snprintf(sarg[k], 128, "%d", TEST_W); k++;
    snprintf(sarg[k], 128, "-height"); k++; snprintf(sarg[k], 128, "%d", TEST_H); k++;
    snprintf(sarg[k], 128, "-n"); k++; snprintf(sarg[k], 128, "9"); k++;
    snprintf(sarg[k], 128, "-qp"); k++; snprintf(sarg[k], 128, "30"); k++;
    snprintf(sarg[k], 128, "-log2_sb_size"); k++; snprintf(sarg[k], 128, "6"); k++;
    snprintf(sarg[k], 128, "-max_num_ref"); k++; snprintf(sarg[k], 128, "2"); k++;
    snprintf(sarg[k], 128, "-num_reorder_pics"); k++; snprintf(sarg[k], 128, "3"); k++;
    snprintf(sarg[k], 128, "-dyadic_coding"); k++; snprintf(sarg[k], 128, "1"); k++;
    snprintf(sarg[k], 128, "-snrcalc"); k++; snprintf(sarg[k], 128, "0"); k++;
    snprintf(sarg[k], 128, "-cdef"); k++; snprintf(sarg[k], 128, "0"); k++;
    sargc = k;
    for (k = 0; k < sargc; k++)
      sargv[k] = sarg[k];
    sargv[sargc] = NULL;
    TASK_CHECK(run_encoder(sargv, NULL, 0, 0, logp) == 0);
  }
  TASK_CHECK(files_equal(bitp, refbit));
  free(raw);
  return 0;
}

const struct task_case task_cases[] = {
  {"StreamInput", "raw_pixels_preserved", case_raw_pixels_preserved},
  {"StreamInput", "y4m_tags_geometry_and_order", case_y4m_tags_geometry_and_order},
  {"StreamInput", "y4m_x_extension_long_header", case_y4m_x_extension_long_header},
  {"StreamInput", "y4m10_little_endian", case_y4m10_little_endian},
  {"StreamInput", "skip_and_n", case_skip_and_n},
  {"StreamInput", "fragmented_fifo", case_fragmented_fifo},
  {"StreamInput", "malformed_inputs_rejected", case_malformed_inputs_rejected},
  {"StreamReorder", "reorder_window_sizes", case_reorder_window_sizes},
  {"StreamReorder", "non_dyadic_reorder", case_non_dyadic_reorder},
  {"StreamReorder", "handshake_output_progress", case_handshake_output_progress},
  {"StreamProcess", "tail_incomplete_gop", case_tail_incomplete_gop},
  {"StreamProcess", "skip_n_segment", case_skip_n_segment},
  {"StreamProcess", "truncation_nonzero", case_truncation_nonzero},
  {"StreamProcess", "hbd_y4m_vs_raw_pixels", case_hbd_y4m_vs_raw_pixels},
  {"StreamProcess", "decode_roundtrip", case_decode_roundtrip},
  {"StreamProcess", "fifo_end_to_end", case_fifo_end_to_end},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
