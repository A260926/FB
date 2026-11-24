/*
 * session_model_replay.c -- Command line replay for the session scheduling
 * model.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This file is part of the CoAP library libcoap. Please see README for terms
 * of use.
 *
 * Takes exactly one input file and runs it through the same coap_model_run()
 * executor used by the OSS-Fuzz entry point, writing the report to stdout and
 * propagating the API's status code as the process exit code (0 consistent,
 * 1 first mismatch, 2 format/resource/IO error).
 */

#include "tests/oss-fuzz/session_model.h"

#include <stdio.h>
#include <stdlib.h>

int
main(int argc, char **argv) {
  FILE *f;
  long n;
  uint8_t *buf;
  size_t got;
  int rc;

  if (argc != 2) {
    fprintf(stderr, "usage: %s plan-file\n", argv[0]);
    return 2;
  }

  f = fopen(argv[1], "rb");
  if (!f)
    return 2;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return 2;
  }
  n = ftell(f);
  if (n < 0 || (unsigned long)n > 65536ul) {
    fclose(f);
    return 2;
  }
  rewind(f);
  buf = (uint8_t *)malloc((size_t)n ? (size_t)n : 1);
  if (!buf) {
    fclose(f);
    return 2;
  }
  got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  if (got != (size_t)n) {
    free(buf);
    return 2;
  }

  rc = coap_model_run(buf, (size_t)n, stdout);
  free(buf);
  return rc;
}
