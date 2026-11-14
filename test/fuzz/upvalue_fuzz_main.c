// upvalue_fuzz_main.c
//
// Entry point for the open-upvalue structured fuzz chain.
//
//   * Exposes LLVMFuzzerTestOneInput so the same translation unit can be
//     linked against a libFuzzer runtime when one is available.
//   * Without libFuzzer it runs as a standalone driver: the fixed corpus and
//     at least 300 deterministic generated plans, each replayed twice in a
//     fresh VM, and prints the real comparison / stack-growth / GC counts.
//   * Can also replay raw corpus files passed on the command line and dump the
//     rendered Wren program for a fixed plan.
//
// Invalid or truncated inputs are rejected during decoding, before any VM is
// created, so a malformed input can never masquerade as an interpreter defect.
#include "upvalue_fuzz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gFailures = 0;

static int runOneInput(const uint8_t *data, size_t size, UpCounters *totals,
                       int requireReplay) {
  UpPlan plan;
  UpDecodeStatus ds = upDecodePlan(data, size, &plan);
  if (ds != UP_DECODE_OK) {
    if (totals) totals->plansRejected++;
    return 0; // controlled rejection, never an interpreter failure
  }

  for (int replay = 0; replay <= requireReplay; replay++) {
    UpCounters run;
    memset(&run, 0, sizeof(run));
    UpRunStatus rs = upExecutePlan(&plan, replay, &run);
    if (totals != NULL) {
      totals->comparisons += run.comparisons;
      totals->closureCalls += run.closureCalls;
      totals->fiberResumes += run.fiberResumes;
      totals->explicitGCs += run.explicitGCs;
      totals->hookCollections += run.hookCollections;
      totals->hookMovesOpen += run.hookMovesOpen;
      totals->allocatorMoves += run.allocatorMoves;
      totals->allocatorAllocs += run.allocatorAllocs;
      totals->allocatorFrees += run.allocatorFrees;
      totals->replays++;
    }
    if (rs != UP_RUN_OK) {
      fprintf(stderr,
              "FAIL replay=%d status=%d mismatch=%s\n",
              replay, (int)rs, upLastMismatch());
      gFailures++;
      return 1;
    }
  }
  if (totals) totals->plansRun++;
  return 0;
}

// libFuzzer / compatible fuzzing entry point.
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  return runOneInput(data, size, NULL, 1);
}

// Everything below is the standalone, no-libFuzzer driver.
#ifndef WREN_LIBFUZZER
static int readFile(const char *path, uint8_t **out, size_t *outSize) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) return 0;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
  long n = ftell(f);
  if (n < 0) { fclose(f); return 0; }
  rewind(f);
  uint8_t *buf = (uint8_t *)malloc((size_t)n + 1);
  if (buf == NULL) { fclose(f); return 0; }
  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[got] = 0;
  *out = buf;
  *outSize = got;
  return 1;
}

static void printCounters(const UpCounters *c) {
  printf("plans: run=%ld rejected=%ld replays=%ld\n",
         c->plansRun, c->plansRejected, c->replays);
  printf("comparisons=%ld fiberResumes=%ld\n",
         c->comparisons, c->fiberResumes);
  printf("stack: movingAllocatorRelocations=%ld hookMovesWithOpenUpvalue=%ld\n",
         c->allocatorMoves, c->hookMovesOpen);
  printf("gc: explicit=%ld hookCollections=%ld allocator(allocs=%ld frees=%ld)\n",
         c->explicitGCs, c->hookCollections,
         c->allocatorAllocs, c->allocatorFrees);
}

// Encodes the fixed and generated plans into a directory as raw corpus files.
static int writeCorpus(const char *dir) {
  UpPlan fixed[8];
  int nf = upFixedPlans(fixed, 8);
  uint8_t buf[8192];
  char path[1024];
  for (int i = 0; i < nf; i++) {
    size_t n = upEncodePlan(&fixed[i], buf, sizeof(buf));
    snprintf(path, sizeof(path), "%s/fixed-%d.upv", dir, i);
    FILE *f = fopen(path, "wb");
    if (f == NULL) return 0;
    fwrite(buf, 1, n, f);
    fclose(f);
  }
  UpRng rng;
  upRngSeed(&rng, 0x557076616c7565ULL);
  for (int i = 0; i < UP_GENERATED_PLANS; i++) {
    UpPlan plan;
    upGeneratePlan(&rng, &plan);
    size_t n = upEncodePlan(&plan, buf, sizeof(buf));
    if (n == 0) continue;
    snprintf(path, sizeof(path), "%s/gen-%04d.upv", dir, i);
    FILE *f = fopen(path, "wb");
    if (f == NULL) return 0;
    fwrite(buf, 1, n, f);
    fclose(f);
  }
  return 1;
}

// Prints the rendered Wren source of one fixed plan as a runnable example.
static int renderFixed(int index) {
  UpPlan fixed[8];
  int nf = upFixedPlans(fixed, 8);
  if (index < 0 || index >= nf) {
    fprintf(stderr, "fixed plan index must be in [0,%d)\n", nf);
    return 2;
  }
  char *src = upRenderProgram(&fixed[index]);
  if (src == NULL) return 3;
  fputs(src, stdout);
  free(src);
  return 0;
}

static int runFullSuite(void) {
  UpCounters totals;
  memset(&totals, 0, sizeof(totals));

  // Fixed corpus first, including the combined mega-plan.
  UpPlan fixed[8];
  int nf = upFixedPlans(fixed, 8);
  uint8_t wire[8192];
  for (int i = 0; i < nf; i++) {
    size_t n = upEncodePlan(&fixed[i], wire, sizeof(wire));
    if (n == 0) {
      fprintf(stderr, "fixed plan %d failed to encode\n", i);
      return 4;
    }
    // Travel through the public decode path like every external input.
    if (runOneInput(wire, n, &totals, 1) != 0) return 5;
  }

  // Generated set: distinct plans, each replayed twice in a fresh VM.
  UpRng rng;
  upRngSeed(&rng, 0x557076616c7565ULL);
  int distinct = 0;
  for (int i = 0; i < UP_GENERATED_PLANS; i++) {
    UpPlan plan;
    upGeneratePlan(&rng, &plan);
    size_t n = upEncodePlan(&plan, wire, sizeof(wire));
    if (n == 0) continue;
    if (runOneInput(wire, n, &totals, 1) != 0) {
      fprintf(stderr, "generated plan %d failed\n", i);
      return 6;
    }
    distinct++;
  }

  if (distinct < 300) {
    fprintf(stderr, "only %d distinct generated plans ran (need >= 300)\n",
            distinct);
    return 7;
  }

  printCounters(&totals);
  if (gFailures != 0) return 8;
  return 0;
}

// When this translation unit is linked against a real libFuzzer runtime
// (-fsanitize=fuzzer), define WREN_LIBFUZZER so the runtime supplies main and
// only LLVMFuzzerTestOneInput is exported. Without it, the standalone driver's
// main below runs the fixed corpus and the deterministic generated set.
int main(int argc, char **argv) {
  if (argc >= 3 && strcmp(argv[1], "--corpus") == 0) {
    return writeCorpus(argv[2]) ? 0 : 9;
  }
  if (argc >= 3 && strcmp(argv[1], "--decode-gen") == 0) {
    UpRng rng;
    upRngSeed(&rng, 0x557076616c7565ULL);
    int target = atoi(argv[2]);
    UpPlan plan;
    for (int i = 0; i <= target; i++) upGeneratePlan(&rng, &plan);
    uint8_t wire[8192];
    size_t n = upEncodePlan(&plan, wire, sizeof(wire));
    UpPlan again;
    UpDecodeStatus ds = upDecodePlan(wire, n, &again);
    printf("decode status %d wireSize %zu opCount %d declCount %d streams %d\n",
           (int)ds, n, plan.opCount, plan.declCount, plan.streamCount);
    return ds == UP_DECODE_OK ? 0 : 1;
  }
  if (argc >= 3 && strcmp(argv[1], "--render-gen") == 0) {
    UpRng rng;
    upRngSeed(&rng, 0x557076616c7565ULL);
    int target = atoi(argv[2]);
    UpPlan plan;
    for (int i = 0; i <= target; i++) upGeneratePlan(&rng, &plan);
    char *src = upRenderProgram(&plan);
    if (src == NULL) return 3;
    fputs(src, stdout);
    free(src);
    return 0;
  }
  if (argc >= 3 && strcmp(argv[1], "--render") == 0) {
    return renderFixed(atoi(argv[2]));
  }
  if (argc >= 2) {
    // Offline reproduction of one or more raw corpus files.
    UpCounters totals;
    memset(&totals, 0, sizeof(totals));
    for (int i = 1; i < argc; i++) {
      uint8_t *buf;
      size_t size;
      if (!readFile(argv[i], &buf, &size)) {
        fprintf(stderr, "cannot read %s\n", argv[i]);
        return 10;
      }
      int rc = runOneInput(buf, size, &totals, 1);
      free(buf);
      if (rc != 0) return 11;
    }
    printCounters(&totals);
    return gFailures ? 12 : 0;
  }
  return runFullSuite();
}
#endif // WREN_LIBFUZZER
