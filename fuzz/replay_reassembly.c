/*
 * Replay driver for RXA1 schedules.  It accepts exactly one file
 * argument, runs the same rxcheck_run as the fuzzing harness, prints
 * the trace to stdout and exits with the rxcheck_run return code.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "reassembly.h"

int main(int argc, char **argv) {
  FILE *fp;
  long fsize;
  uint8_t *data;
  size_t size;
  rxcheck_result result;
  int rv;

  if (argc != 2) {
    fprintf(stderr, "usage: %s <schedule>\n", argv[0]);
    return 2;
  }

  fp = fopen(argv[1], "rb");
  if (fp == NULL) {
    fprintf(stderr, "cannot open %s\n", argv[1]);
    return 2;
  }

  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return 2;
  }

  fsize = ftell(fp);
  if (fsize < 0) {
    fclose(fp);
    return 2;
  }

  rewind(fp);

  size = (size_t)fsize;
  data = malloc(size == 0 ? 1 : size);
  if (data == NULL) {
    fclose(fp);
    return 2;
  }

  if (fread(data, 1, size, fp) != size) {
    free(data);
    fclose(fp);
    return 2;
  }

  fclose(fp);

  memset(&result, 0, sizeof(result));
  rv = rxcheck_run(data, size, &result, stdout);

  free(data);

  return rv;
}
