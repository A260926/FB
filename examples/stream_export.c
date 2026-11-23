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
 * stream_export.c
 *
 * End-to-end example for Thorenc's non-seekable streaming input.
 *
 * The program generates nine 64x64 8-bit I420 pictures itself, pipes them
 * in fixed-size chunks (no whole-sequence YUV file is ever created for the
 * encoder) into a real Thorenc process running with "-stream_input 1 -if -",
 * and then invokes Thordec on the resulting bitstream.  Everything is
 * written into the directory passed as the single command line argument:
 *
 *   <dir>/out.bit     encoded bitstream
 *   <dir>/decoded.yuv decoder output in display order
 *
 * The encoder and decoder locations can be overridden with THORENC_BIN and
 * THORDEC_BIN; they default to the task-build locations used by the
 * repository build.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>

#define WIDTH 64
#define HEIGHT 64
#define FRAMES 9
#define Y_SIZE (WIDTH * HEIGHT)
#define C_SIZE (WIDTH * HEIGHT / 4)
#define FRAME_SIZE (Y_SIZE + 2 * C_SIZE)          /* 6144 bytes */
#define CHUNK_SIZE 1024                           /* fragmented delivery */

static const char *encoder_bin(void)
{
  const char *p = getenv("THORENC_BIN");
  return (p && *p) ? p : ".task-build/Thorenc";
}

static const char *decoder_bin(void)
{
  const char *p = getenv("THORDEC_BIN");
  return (p && *p) ? p : ".task-build/Thordec";
}

static void write_retry(int fd, const uint8_t *buf, size_t n)
{
  size_t off = 0;
  while (off < n) {
    ssize_t w = write(fd, buf + off, n - off);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      perror("write to encoder pipe");
      exit(1);
    }
    off += (size_t)w;
  }
}

/* Deterministic moving pattern, one frame at a time, no frame table kept. */
static void generate_frame(uint8_t *dst, int frame_num)
{
  int x, y, i;
  for (y = 0; y < HEIGHT; y++)
    for (x = 0; x < WIDTH; x++)
      dst[y * WIDTH + x] =
          (uint8_t)(x * 3 + y * 2 + frame_num * 17 +
                    (x > frame_num * 3 && x < frame_num * 3 + 12 ? 40 : 0));
  for (i = 0; i < 2 * C_SIZE; i++)
    dst[Y_SIZE + i] = (uint8_t)(96 + (i >= C_SIZE ? 40 : 0) + frame_num * 2);
}

static int wait_for(pid_t pid, const char *what)
{
  int status;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    ;
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    fprintf(stderr, "%s failed (status=0x%x)\n", what, status);
    return -1;
  }
  return 0;
}

int main(int argc, char **argv)
{
  char out_bit[512], decoded_yuv[512];
  int pipefd[2];
  pid_t producer, encoder_pid, decoder_pid;
  uint8_t frame[FRAME_SIZE];
  int f;

  if (argc != 2) {
    fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
    return 2;
  }
  snprintf(out_bit, sizeof(out_bit), "%s/out.bit", argv[1]);
  snprintf(decoded_yuv, sizeof(decoded_yuv), "%s/decoded.yuv", argv[1]);

  if (pipe(pipefd) != 0) {
    perror("pipe");
    return 1;
  }

  encoder_pid = fork();
  if (encoder_pid < 0) {
    perror("fork encoder");
    return 1;
  }
  if (encoder_pid == 0) {
    /* Encoder child: read the pictures from the pipe. */
    dup2(pipefd[0], STDIN_FILENO);
    close(pipefd[0]);
    close(pipefd[1]);
    execl(encoder_bin(), encoder_bin(),
          "-stream_input", "1", "-if", "-",
          "-of", out_bit,
          "-width", "64", "-height", "64",
          "-n", "9", "-qp", "34", "-log2_sb_size", "6",
          "-num_reorder_pics", "3", "-max_num_ref", "2",
          "-dyadic_coding", "1", "-snrcalc", "0", "-cdef", "0",
          (char *)NULL);
    perror("exec Thorenc");
    _exit(127);
  }
  close(pipefd[0]);

  /*
   * Producer: synthesize each picture and push it in CHUNK_SIZE pieces,
   * keeping the B-frame lookahead satisfied while never materializing the
   * whole sequence.
   */
  producer = fork();
  if (producer < 0) {
    perror("fork producer");
    return 1;
  }
  if (producer == 0) {
    close(pipefd[0]);
    for (f = 0; f < FRAMES; f++) {
      size_t off;
      generate_frame(frame, f);
      for (off = 0; off < FRAME_SIZE; off += CHUNK_SIZE) {
        size_t c = FRAME_SIZE - off < CHUNK_SIZE ? FRAME_SIZE - off : CHUNK_SIZE;
        write_retry(pipefd[1], frame + off, c);
      }
    }
    close(pipefd[1]);
    _exit(0);
  }
  close(pipefd[1]);

  if (wait_for(encoder_pid, "Thorenc") != 0)
    return 1;
  if (wait_for(producer, "producer") != 0)
    return 1;

  /* Decode the bitstream back to raw pictures in display order. */
  decoder_pid = fork();
  if (decoder_pid < 0) {
    perror("fork decoder");
    return 1;
  }
  if (decoder_pid == 0) {
    execl(decoder_bin(), decoder_bin(), out_bit, decoded_yuv, (char *)NULL);
    perror("exec Thordec");
    _exit(127);
  }
  if (wait_for(decoder_pid, "Thordec") != 0)
    return 1;

  printf("stream_export: %d frames, bitstream=%s, decoded=%s\n",
         FRAMES, out_bit, decoded_yuv);
  return 0;
}
