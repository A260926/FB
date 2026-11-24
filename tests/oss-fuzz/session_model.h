/*
 * session_model.h -- Benign deterministic session scheduling model for the
 * OSS-Fuzz style regression harness.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This file is part of the CoAP library libcoap. Please see README for terms
 * of use.
 */

/**
 * @file session_model.h
 * @brief Text driven CoAP session scheduling model and differential checker.
 */

#ifndef SESSION_MODEL_H_
#define SESSION_MODEL_H_

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Runs the complete session scheduling model contained in @p data.
 *
 * The input is parsed as a whole before any CoAP context or worker process is
 * created. Every declared CASE is executed in its own worker process against a
 * real libcoap context (loopback UDP client sessions, 100% send packet loss)
 * while an independent absolute-time model predicts the differential
 * retransmission queue, the retry stages and every response/nack callback.
 * Reachable states and transition edges are merged deterministically and the
 * coverage report is written to @p report.
 *
 * @param data   The complete COAP_MODEL_PLAN text. At most 65536 bytes.
 * @param size   Number of valid bytes in @p data.
 * @param report Open stream for the report; must not be NULL.
 *
 * @return 0 if the model matches the real library on every CASE and the full
 *           report was written.
 *         1 on the first model mismatch; only a single MISMATCH line is
 *           written to @p report.
 *         2 on format, resource or I/O errors; nothing is written to
 *           @p report.
 */
int coap_model_run(const uint8_t *data, size_t size, FILE *report);

#ifdef __cplusplus
}
#endif

#endif /* SESSION_MODEL_H_ */
