/* Example: losslessly re-cut a Theora/Ogg file.
 *
 * Usage:
 *   theora_edit INPUT OUTPUT STREAM BEGIN END [STREAM BEGIN END ...]
 *
 * Each triple selects the half-open frame range [BEGIN,END) of the zero based
 * logical Theora stream STREAM.  The ranges are concatenated in the order
 * given and written to OUTPUT as one continuous Theora logical stream.
 *
 * Exit status is zero on success and non-zero on any failure.
 */

#include <ogg/theora_edit.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_u64(const char *text, uint64_t *out) {
  char *end = NULL;
  unsigned long long value;
  if (!text || !*text) return 1;
  if (text[0] == '-') return 1;
  value = strtoull(text, &end, 10);
  if (!end || *end != '\0') return 1;
  *out = (uint64_t)value;
  return 0;
}

int main(int argc, char **argv) {
  FILE *input = NULL;
  FILE *output = NULL;
  ogg_theora_plan *plan = NULL;
  ogg_theora_cut *cuts = NULL;
  size_t cut_count, i;
  int rc = 1;
  const int serial = 0x5e4a17;

  if (argc < 6 || (argc - 3) % 3 != 0) {
    fprintf(stderr,
            "usage: %s INPUT OUTPUT STREAM BEGIN END [STREAM BEGIN END ...]\n",
            argv[0]);
    return 2;
  }

  cut_count = (size_t)(argc - 3) / 3;
  cuts = malloc(cut_count * sizeof(*cuts));
  if (!cuts) {
    fprintf(stderr, "out of memory\n");
    goto done;
  }

  for (i = 0; i < cut_count; ++i) {
    const char *stream_arg = argv[3 + i * 3];
    uint64_t stream, begin, end;
    if (parse_u64(stream_arg, &stream) || stream > 0xFFFFFFFFu ||
        parse_u64(argv[4 + i * 3], &begin) ||
        parse_u64(argv[5 + i * 3], &end)) {
      fprintf(stderr, "invalid cut arguments\n");
      goto done;
    }
    cuts[i].stream = (uint32_t)stream;
    cuts[i].begin = begin;
    cuts[i].end = end;
  }

  input = fopen(argv[1], "rb");
  if (!input) {
    fprintf(stderr, "cannot open input: %s\n", argv[1]);
    goto done;
  }
  output = fopen(argv[2], "wb");
  if (!output) {
    fprintf(stderr, "cannot open output: %s\n", argv[2]);
    goto done;
  }

  if (ogg_theora_plan_create(input, cuts, cut_count, &plan) || !plan) {
    fprintf(stderr, "cannot build theora edit plan\n");
    goto done;
  }
  if (ogg_theora_plan_write(plan, output, serial)) {
    fprintf(stderr, "cannot write theora edit\n");
    goto done;
  }
  if (fflush(output)) {
    fprintf(stderr, "cannot flush output\n");
    goto done;
  }

  rc = 0;

done:
  ogg_theora_plan_destroy(plan);
  free(cuts);
  if (input) fclose(input);
  if (output && fclose(output)) rc = 1;
  return rc;
}
