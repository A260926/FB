/*
   Copyright (C) 2026 Annodex Association

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   - Neither the name of the Annodex Association nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
   PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE ASSOCIATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
 * packet-seek.c
 *
 * Example of oggz_seek_packets(): jump directly to a packet ordinal of
 * one logical Ogg stream, then continue reading all interleaved streams
 * in physical page order through the usual read callback.
 *
 * Usage: packet-seek filename serialno packetno
 *
 * Works with any seekable input: here a stdio FILE handle is used; a
 * custom source providing read/seek/tell callbacks works identically.
 * No metric and no OGGZ_AUTO flag are required.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>

#include "oggz/oggz.h"

static int
read_packet (OGGZ * oggz, oggz_packet * zp, long serialno, void * user_data)
{
  ogg_packet * op = &zp->op;
  (void) oggz;
  (void) user_data;

  printf ("serialno=%010ld packetno=%lld bytes=%ld"
          " b_o_s=%d e_o_s=%d granulepos=%lld\n",
          serialno, (long long) op->packetno, op->bytes,
          op->b_o_s ? 1 : 0, op->e_o_s ? 1 : 0,
          (long long) op->granulepos);

  return OGGZ_CONTINUE;
}

int
main (int argc, char ** argv)
{
  FILE * f;
  OGGZ * oggz;
  long serialno;
  long packetno;
  long target;
  long n;

  if (argc != 4) {
    fprintf (stderr, "usage: %s filename serialno packetno\n", argv[0]);
    return 1;
  }

  serialno = strtol (argv[2], NULL, 0);
  packetno = strtol (argv[3], NULL, 0);

  f = fopen (argv[1], "rb");
  if (f == NULL) {
    perror ("fopen");
    return 1;
  }

  /* OGGZ_READ alone is enough: packet seeking needs neither OGGZ_AUTO
   * nor a configured metric. */
  oggz = oggz_open_stdio (f, OGGZ_READ);
  if (oggz == NULL) {
    fprintf (stderr, "could not open Ogg file\n");
    fclose (f);
    return 1;
  }

  if (oggz_set_read_callback (oggz, -1, read_packet, NULL) != 0) {
    fprintf (stderr, "could not set read callback\n");
    oggz_close (oggz);
    return 1;
  }

  /* Absolute ordinal from the BOS packet, which is packet 0. */
  target = oggz_seek_packets (oggz, serialno, packetno, SEEK_SET);
  if (target == -1) {
    fprintf (stderr,
             "seek to packet %ld of stream %ld failed (bad serialno,"
             " out of range, or source is not seekable)\n",
             packetno, serialno);
    oggz_close (oggz);
    return 1;
  }

  printf ("seeked to absolute packet ordinal %ld\n", target);

  /* The first callback above is the target packet; afterwards every
   * stream's remaining packets arrive in normal physical page order. */
  while ((n = oggz_read (oggz, 1024)) > 0);
  if (n < 0 && n != OGGZ_ERR_STOP_OK) {
    fprintf (stderr, "read error %ld\n", n);
    oggz_close (oggz);
    return 1;
  }

  /* Relative navigation on the same stream. SEEK_CUR is measured from
   * the next packet that would have been delivered, SEEK_END from the
   * total packet count, so -1 names the final packet. */
  target = oggz_seek_packets (oggz, serialno, -1, SEEK_END);
  if (target == -1) {
    fprintf (stderr, "seek to final packet failed\n");
    oggz_close (oggz);
    return 1;
  }
  printf ("final packet ordinal of stream %ld is %ld\n", serialno, target);

  oggz_close (oggz);
  return 0;
}
