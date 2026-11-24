#include "fuzz/stateful/model.h"

#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  if (argc == 1) {
    return qpack_model_replay(stdin, stdout) == 0 ? 0 : 1;
  }

  if (argc == 4 && strcmp(argv[1], "--generate") == 0) {
    char *end = NULL;
    unsigned long long seed;
    unsigned long steps;

    seed = strtoull(argv[2], &end, 10);
    if (end == argv[2] || *end != '\0') {
      fprintf(stderr, "invalid SEED: %s\n", argv[2]);
      return 2;
    }
    steps = strtoul(argv[3], &end, 10);
    if (end == argv[3] || *end != '\0' || steps > 1000000UL) {
      fprintf(stderr, "invalid STEPS: %s\n", argv[3]);
      return 2;
    }
    return qpack_model_generate((uint64_t)seed, (unsigned int)steps, stdout);
  }

  fprintf(stderr, "usage: %s [--generate SEED STEPS]\n", argv[0]);
  return 2;
}
