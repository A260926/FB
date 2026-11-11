/*
 * Copyright (C) 2016  Lukas Lalinsky
 * Distributed under the MIT license, see the LICENSE file for details.
 *
 * C99 smoke test for chromaprint_match_ordered(). Covers binary and base64
 * input, return values, result ownership, error handling and recovery after a
 * failed call. Every check below is executed; the process exits non-zero if
 * any of them fails.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chromaprint.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                      \
	do {                                                                 \
		g_checks++;                                                      \
		if (!(cond)) {                                                   \
			g_failures++;                                                \
			fprintf(stderr, "CHECK failed at %s:%d: %s\n",               \
			        __FILE__, __LINE__, #cond);                          \
		}                                                                \
	} while (0)

static char *encode_fp(const uint32_t *fp, int size, int algorithm,
                       int base64, int *out_size)
{
	char *encoded = NULL;
	int encoded_size = 0;
	int ok = chromaprint_encode_fingerprint(fp, size, algorithm,
	                                        &encoded, &encoded_size, base64);
	CHECK(ok == 1);
	CHECK(encoded != NULL);
	CHECK(encoded_size > 0);
	*out_size = encoded_size;
	return encoded;
}

int main(void)
{
	enum { N1 = 20, N2 = 14 };
	uint32_t fp1[N1];
	uint32_t fp2[N2];
	int i;

	/* fp1 = 1..20, fp2 = 1..6 followed by 13..20 (items 7..12 removed). */
	for (i = 0; i < N1; i++) {
		fp1[i] = (uint32_t)(i + 1);
	}
	for (i = 0; i < 6; i++) {
		fp2[i] = fp1[i];
	}
	for (i = 0; i < 8; i++) {
		fp2[6 + i] = fp1[12 + i];
	}

	int bin_size1 = 0, bin_size2 = 0;
	int b64_size1 = 0, b64_size2 = 0;
	char *bin1 = encode_fp(fp1, N1, CHROMAPRINT_ALGORITHM_TEST2, 0, &bin_size1);
	char *bin2 = encode_fp(fp2, N2, CHROMAPRINT_ALGORITHM_TEST2, 0, &bin_size2);
	char *b64_1 = encode_fp(fp1, N1, CHROMAPRINT_ALGORITHM_TEST2, 1, &b64_size1);
	char *b64_2 = encode_fp(fp2, N2, CHROMAPRINT_ALGORITHM_TEST2, 1, &b64_size2);
	CHECK(bin1 != NULL && bin2 != NULL && b64_1 != NULL && b64_2 != NULL);

	/* Binary fingerprints carry embedded NUL bytes; they must be handled by
	 * the explicit length, not by string scanning. */
	CHECK(memchr(bin1, 0, (size_t)bin_size1) != NULL);

	/* ------------------------------------------------------------------ */
	/* Binary input, expected coordinates (0,0,6) and (12,6,8).           */
	/* ------------------------------------------------------------------ */
	ChromaprintSegment *seg_bin = NULL;
	int num_bin = -1;
	int ret = chromaprint_match_ordered(bin1, bin_size1, bin2, bin_size2,
	                                    0, 0, 4, &seg_bin, &num_bin);
	CHECK(ret == 1);
	CHECK(num_bin == 2);
	CHECK(seg_bin != NULL);
	if (num_bin == 2) {
		CHECK(seg_bin[0].pos1 == 0);
		CHECK(seg_bin[0].pos2 == 0);
		CHECK(seg_bin[0].duration == 6);
		CHECK(seg_bin[0].score == 0.0);
		CHECK(seg_bin[1].pos1 == 12);
		CHECK(seg_bin[1].pos2 == 6);
		CHECK(seg_bin[1].duration == 8);
		CHECK(seg_bin[1].score == 0.0);
	}

	/* The result must stay valid after the inputs are released. */
	chromaprint_dealloc(bin1);
	chromaprint_dealloc(bin2);
	bin1 = NULL;
	bin2 = NULL;
	if (num_bin == 2) {
		CHECK(seg_bin[1].pos1 == 12);
		CHECK(seg_bin[1].duration == 8);
	}

	/* ------------------------------------------------------------------ */
	/* Base64 input gives the same coordinates.                           */
	/* ------------------------------------------------------------------ */
	ChromaprintSegment *seg_b64 = NULL;
	int num_b64 = -1;
	ret = chromaprint_match_ordered(b64_1, b64_size1, b64_2, b64_size2,
	                                1, 0, 4, &seg_b64, &num_b64);
	CHECK(ret == 1);
	CHECK(num_b64 == num_bin);
	CHECK(seg_b64 != NULL);
	if (num_b64 == 2 && seg_bin != NULL) {
		for (i = 0; i < num_b64; i++) {
			CHECK(seg_b64[i].pos1 == seg_bin[i].pos1);
			CHECK(seg_b64[i].pos2 == seg_bin[i].pos2);
			CHECK(seg_b64[i].duration == seg_bin[i].duration);
			CHECK(seg_b64[i].score == seg_bin[i].score);
		}
	}

	/* Two calls produced independently owned arrays; free them separately. */
	chromaprint_dealloc(seg_b64);
	chromaprint_dealloc(seg_bin);
	seg_b64 = NULL;
	seg_bin = NULL;
	chromaprint_dealloc(b64_1);
	chromaprint_dealloc(b64_2);

	/* ------------------------------------------------------------------ */
	/* Failure paths: every failure returns 0 and clears the outputs.     */
	/* ------------------------------------------------------------------ */
	int bin1_size = 0, bin2_size = 0;
	char *raw1 = encode_fp(fp1, N1, CHROMAPRINT_ALGORITHM_TEST2, 0, &bin1_size);
	char *raw2 = encode_fp(fp2, N2, CHROMAPRINT_ALGORITHM_TEST2, 0, &bin2_size);

	ChromaprintSegment *seg = NULL;
	int num = -1;

	/* Header shorter than four bytes. */
	ret = chromaprint_match_ordered(raw1, 3, raw2, bin2_size,
	                                0, 0, 4, &seg, &num);
	CHECK(ret == 0);
	CHECK(seg == NULL);
	CHECK(num == 0);

	/* Declared item count that the truncated body cannot satisfy. */
	seg = (ChromaprintSegment *)(intptr_t)0x1;
	num = -1;
	ret = chromaprint_match_ordered(raw1, 5, raw2, bin2_size,
	                                0, 0, 4, &seg, &num);
	CHECK(ret == 0);
	CHECK(seg == NULL);
	CHECK(num == 0);

	/* Unknown algorithm in the first fingerprint. */
	int unknown_size = 0;
	char *unknown = encode_fp(fp1, N1, 99, 0, &unknown_size);
	seg = (ChromaprintSegment *)(intptr_t)0x1;
	num = -1;
	ret = chromaprint_match_ordered(unknown, unknown_size, raw2, bin2_size,
	                                0, 0, 4, &seg, &num);
	CHECK(ret == 0);
	CHECK(seg == NULL);
	CHECK(num == 0);
	chromaprint_dealloc(unknown);

	/* Mismatched algorithms between the two fingerprints. */
	int algo1_size = 0;
	char *algo1 = encode_fp(fp1, N1, CHROMAPRINT_ALGORITHM_TEST1, 0, &algo1_size);
	seg = (ChromaprintSegment *)(intptr_t)0x1;
	num = -1;
	ret = chromaprint_match_ordered(algo1, algo1_size, raw2, bin2_size,
	                                0, 0, 4, &seg, &num);
	CHECK(ret == 0);
	CHECK(seg == NULL);
	CHECK(num == 0);
	chromaprint_dealloc(algo1);

	/* Invalid parameters. */
	seg = (ChromaprintSegment *)(intptr_t)0x1;
	num = -1;
	ret = chromaprint_match_ordered(raw1, bin1_size, raw2, bin2_size,
	                                0, 33, 4, &seg, &num);
	CHECK(ret == 0);
	CHECK(seg == NULL);
	CHECK(num == 0);

	seg = (ChromaprintSegment *)(intptr_t)0x1;
	num = -1;
	ret = chromaprint_match_ordered(raw1, bin1_size, raw2, bin2_size,
	                                0, 0, 0, &seg, &num);
	CHECK(ret == 0);
	CHECK(seg == NULL);
	CHECK(num == 0);

	/* NULL output pointers are reported as failure without crashing. */
	ret = chromaprint_match_ordered(raw1, bin1_size, raw2, bin2_size,
	                                0, 0, 4, NULL, &num);
	CHECK(ret == 0);
	ret = chromaprint_match_ordered(raw1, bin1_size, raw2, bin2_size,
	                                0, 0, 4, &seg, NULL);
	CHECK(ret == 0);

	/* ------------------------------------------------------------------ */
	/* A successful call after the failures must not carry stale state.   */
	/* ------------------------------------------------------------------ */
	seg = NULL;
	num = -1;
	ret = chromaprint_match_ordered(raw1, bin1_size, raw2, bin2_size,
	                                0, 0, 4, &seg, &num);
	CHECK(ret == 1);
	CHECK(num == 2);
	CHECK(seg != NULL);
	if (num == 2) {
		CHECK(seg[0].pos1 == 0 && seg[0].pos2 == 0 && seg[0].duration == 6);
		CHECK(seg[1].pos1 == 12 && seg[1].pos2 == 6 && seg[1].duration == 8);
	}
	chromaprint_dealloc(seg);

	/* ------------------------------------------------------------------ */
	/* Legal empty fingerprints and genuine no-matches return 1 with an   */
	/* empty result.                                                      */
	/* ------------------------------------------------------------------ */
	int empty_size = 0;
	char *empty_fp = encode_fp(NULL, 0, CHROMAPRINT_ALGORITHM_TEST2, 0, &empty_size);
	CHECK(empty_size == 4);
	seg = (ChromaprintSegment *)(intptr_t)0x1;
	num = -1;
	ret = chromaprint_match_ordered(empty_fp, empty_size, raw2, bin2_size,
	                                0, 0, 1, &seg, &num);
	CHECK(ret == 1);
	CHECK(seg == NULL);
	CHECK(num == 0);

	/* Values far enough apart share no bits within the threshold. */
	uint32_t unlike1[3] = { 0xffffffffu, 0xfffffff0u, 0xfffffff1u };
	uint32_t unlike2[3] = { 0x00000000u, 0x0000000fu, 0x0000000eu };
	int u1_size = 0, u2_size = 0;
	char *enc_u1 = encode_fp(unlike1, 3, CHROMAPRINT_ALGORITHM_TEST2, 0, &u1_size);
	char *enc_u2 = encode_fp(unlike2, 3, CHROMAPRINT_ALGORITHM_TEST2, 0, &u2_size);
	seg = (ChromaprintSegment *)(intptr_t)0x1;
	num = -1;
	ret = chromaprint_match_ordered(enc_u1, u1_size, enc_u2, u2_size,
	                                0, 0, 1, &seg, &num);
	CHECK(ret == 1);
	CHECK(seg == NULL);
	CHECK(num == 0);

	/* Trailing bytes after a valid fingerprint are ignored as long as the
	 * declared item count can still be decoded from the explicit length. */
	char *padded = (char *)malloc((size_t)bin1_size + 8);
	CHECK(padded != NULL);
	memcpy(padded, raw1, (size_t)bin1_size);
	memset(padded + bin1_size, 0xab, 8);
	seg = NULL;
	num = -1;
	ret = chromaprint_match_ordered(padded, bin1_size + 8, raw2, bin2_size,
	                                0, 0, 4, &seg, &num);
	CHECK(ret == 1);
	CHECK(num == 2);
	CHECK(seg != NULL);
	chromaprint_dealloc(seg);
	free(padded);

	chromaprint_dealloc(enc_u1);
	chromaprint_dealloc(enc_u2);
	chromaprint_dealloc(empty_fp);
	chromaprint_dealloc(raw1);
	chromaprint_dealloc(raw2);

	/* A run that executed no checks at all must never count as success. */
	if (g_checks == 0) {
		g_failures++;
		fprintf(stderr, "no checks were executed\n");
	}

	printf("ordered_c_smoke: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
