/*
 * Copyright (c) 2019 Trail of Bits, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Example: run two ordinary benign DeepState tests through the isolated
 * replay-batch entry point and publish a small transcript beside their
 * artifacts.
 *
 *   ./replay_batch <new-output-dir>
 *
 * The output directory must not exist yet; the batch entry publishes it
 * atomically.  `transcript.txt` is written afterwards and contains the
 * marker line `replay-batch normal=2`. */

#include <deepstate/Replay.h>

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

/* Consumes a fixed two-byte input. */
static void consume_two(void) {
  if (DeepState_UChar() != 0x21) {
    DeepState_Fail();
  }
  if (DeepState_UChar() != 0x43) {
    DeepState_Fail();
  }
  DeepState_Pass();
}

/* Consumes a single byte. */
static void consume_one(void) {
  if (DeepState_UChar() != 0x65) {
    DeepState_Fail();
  }
  DeepState_Pass();
}

int main(int argc, char **argv) {
  if (argc != 2) {
    static const char usage[] = "usage: replay_batch <new-output-dir>\n";
    (void) !write(STDERR_FILENO, usage, sizeof(usage) - 1);
    return 2;
  }
  const char *output_dir = argv[1];

  static const uint8_t input_a[] = {0x21, 0x43};
  static const uint8_t input_b[] = {0x65};

  struct DeepState_TestInfo test_a = {
      NULL, consume_two, "ConsumeTwo", "examples/replay_batch.c", 1};
  struct DeepState_TestInfo test_b = {
      NULL, consume_one, "ConsumeOne", "examples/replay_batch.c", 2};

  struct DeepState_ReplayCase cases[2];
  memset(cases, 0, sizeof(cases));
  cases[0].id = 1;
  cases[0].test = &test_a;
  cases[0].data = input_a;
  cases[0].size = sizeof(input_a);
  cases[1].id = 2;
  cases[1].test = &test_b;
  cases[1].data = input_b;
  cases[1].size = sizeof(input_b);

  struct DeepState_ReplayConfig config;
  memset(&config, 0, sizeof(config));
  config.run_id = 1;
  config.jobs = 2;
  config.log_limit = 64 * 1024;
  config.cancel_fd = -1;
  config.output_dir = output_dir;

  struct DeepState_ReplayResult results[2];
  int rc = DeepState_ReplayBatch(cases, 2, &config, results);
  if (rc != 0) {
    return rc == 1 ? 1 : 3;
  }

  for (size_t i = 0; i < 2; ++i) {
    if (results[i].state != DeepState_ReplayComplete ||
        results[i].result != DeepState_TestRunPass) {
      return 4;
    }
  }

  char path[4096];
  int n = snprintf(path, sizeof(path), "%s/transcript.txt", output_dir);
  if (n < 0 || (size_t) n >= sizeof(path)) {
    return 5;
  }
  int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
  if (fd < 0) {
    return 5;
  }
  static const char line[] = "replay-batch normal=2\n";
  size_t off = 0;
  while (off < sizeof(line) - 1) {
    ssize_t k = write(fd, line + off, sizeof(line) - 1 - off);
    if (k <= 0) {
      close(fd);
      return 6;
    }
    off += (size_t) k;
  }
  if (close(fd) != 0) {
    return 6;
  }
  return 0;
}
