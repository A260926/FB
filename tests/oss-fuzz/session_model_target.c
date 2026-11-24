/*
 * session_model_target.c -- OSS-Fuzz entry point for the benign session
 * scheduling model.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This file is part of the CoAP library libcoap. Please see README for terms
 * of use.
 *
 * The LLVM entry reuses the shared coap_model_run() executor. Malformed input
 * is ignored quietly; any model mismatch is a hard failure. No raw byte
 * mutation, unknown protocol data or external target is involved.
 */

#include "tests/oss-fuzz/session_model.h"

#include <stdio.h>

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  int rc;

  if (!data || size == 0)
    return 0;

  rc = coap_model_run(data, size, stdout);
  if (rc == 1) {
    /* First differential mismatch: fail this input immediately. */
    return 1;
  }
  /* rc == 0: model consistent. rc == 2: malformed input or resource/IO
   * error, which is ignored like any other non-actionable fuzz input. */
  return 0;
}
