// Copyright (C) 2010-2016  Lukas Lalinsky
// Distributed under the MIT license, see the LICENSE file for details.

#include <vector>
#include <string>
#include <algorithm>
#include <memory>
#include <cstring>
#include <chromaprint.h>
#include "fingerprinter.h"
#include "fingerprint_compressor.h"
#include "fingerprint_decompressor.h"
#include "fingerprint_matcher.h"
#include "fingerprinter_configuration.h"
#include "utils/base64.h"
#include "simhash.h"
#include "debug.h"

using namespace chromaprint;

struct ChromaprintContextPrivate {
	ChromaprintContextPrivate(int algorithm)
		: algorithm(algorithm),
		  fingerprinter(CreateFingerprinterConfiguration(algorithm)) {}
	int algorithm;
	Fingerprinter fingerprinter;
	FingerprintCompressor compressor;
	std::string tmp_fingerprint;
};

struct ChromaprintMatcherContextPrivate {
	int algorithm = -1;
	std::unique_ptr<FingerprintMatcher> matcher;
	std::vector<uint32_t> fp[2];
	FingerprintDecompressor decompressor;
};

extern "C" {

#define FAIL_IF(x, msg) if (x) { DEBUG(msg); return 0; }

#define STR(x) #x
#define VERSION_STR(major, minor, patch) \
	STR(major) "." STR(minor) "." STR(patch)

static const char *version_str = VERSION_STR(
	CHROMAPRINT_VERSION_MAJOR,
	CHROMAPRINT_VERSION_MINOR,
	CHROMAPRINT_VERSION_PATCH);

const char *chromaprint_get_version(void)
{
	return version_str;
}

ChromaprintContext *chromaprint_new(int algorithm)
{
	return new ChromaprintContextPrivate(algorithm);
}

void chromaprint_free(ChromaprintContext *ctx)
{
	if (ctx) {
		delete ctx;
	}
}

int chromaprint_get_algorithm(ChromaprintContext *ctx)
{
	FAIL_IF(!ctx, "context can't be NULL");
	return ctx->algorithm;
}

int chromaprint_set_option(ChromaprintContext *ctx, const char *name, int value)
{
	FAIL_IF(!ctx, "context can't be NULL");
	return ctx->fingerprinter.SetOption(name, value) ? 1 : 0;
}

int chromaprint_get_num_channels(ChromaprintContext *ctx)
{
	return 1;
}

int chromaprint_get_sample_rate(ChromaprintContext *ctx)
{
	return ctx ? ctx->fingerprinter.config()->sample_rate() : 0;
}

int chromaprint_get_item_duration(ChromaprintContext *ctx)
{
	return ctx ? ctx->fingerprinter.config()->item_duration() : 0;
}

int chromaprint_get_item_duration_ms(ChromaprintContext *ctx)
{
	return ctx ? ctx->fingerprinter.config()->item_duration_in_seconds() * 1000 : 0;
}

int chromaprint_get_delay(ChromaprintContext *ctx)
{
	return ctx ? ctx->fingerprinter.config()->delay() : 0;
}

int chromaprint_get_delay_ms(ChromaprintContext *ctx)
{
	return ctx ? ctx->fingerprinter.config()->delay_in_seconds() * 1000 : 0;
}

int chromaprint_start(ChromaprintContext *ctx, int sample_rate, int num_channels)
{
	FAIL_IF(!ctx, "context can't be NULL");
	return ctx->fingerprinter.Start(sample_rate, num_channels) ? 1 : 0;
}

int chromaprint_feed(ChromaprintContext *ctx, const int16_t *data, int length)
{
	FAIL_IF(!ctx, "context can't be NULL");
	ctx->fingerprinter.Consume(data, length);
	return 1;
}

int chromaprint_finish(ChromaprintContext *ctx)
{
	FAIL_IF(!ctx, "context can't be NULL");
	ctx->fingerprinter.Finish();
	return 1;
}

int chromaprint_get_fingerprint(ChromaprintContext *ctx, char **data)
{
	FAIL_IF(!ctx, "context can't be NULL");
	ctx->compressor.Compress(ctx->fingerprinter.GetFingerprint(), ctx->algorithm, ctx->tmp_fingerprint);
	*data = (char *) malloc(GetBase64EncodedSize(ctx->tmp_fingerprint.size()) + 1);
	FAIL_IF(!*data, "can't allocate memory for the result");
	Base64Encode(ctx->tmp_fingerprint.begin(), ctx->tmp_fingerprint.end(), *data, true);
	return 1;
}

int chromaprint_get_raw_fingerprint(ChromaprintContext *ctx, uint32_t **data, int *size)
{
	FAIL_IF(!ctx, "context can't be NULL");
	const auto fingerprint = ctx->fingerprinter.GetFingerprint();
	*data = (uint32_t *) malloc(sizeof(uint32_t) * fingerprint.size());
	FAIL_IF(!*data, "can't allocate memory for the result");
	*size = int(fingerprint.size());
	std::copy(fingerprint.begin(), fingerprint.end(), *data);
	return 1;
}

int chromaprint_get_raw_fingerprint_size(ChromaprintContext *ctx, int *size)
{
	FAIL_IF(!ctx, "context can't be NULL");
	const auto fingerprint = ctx->fingerprinter.GetFingerprint();
	*size = int(fingerprint.size());
	return 1;
}

int chromaprint_get_fingerprint_hash(ChromaprintContext *ctx, uint32_t *hash)
{
	FAIL_IF(!ctx, "context can't be NULL");
	*hash = SimHash(ctx->fingerprinter.GetFingerprint());
	return 1;
}

int chromaprint_clear_fingerprint(ChromaprintContext *ctx)
{
	FAIL_IF(!ctx, "context can't be NULL");
	ctx->fingerprinter.ClearFingerprint();
	return 1;
}

int chromaprint_encode_fingerprint(const uint32_t *fp, int size, int algorithm, char **encoded_fp, int *encoded_size, int base64)
{
	// The encoded header stores the size in 24 bits.
	if ((fp == NULL && size != 0) || size < 0 || size > 0xFFFFFF || encoded_fp == NULL || encoded_size == NULL) {
		return 0;
	}
	std::vector<uint32_t> uncompressed(fp, fp + size);
	std::string encoded = CompressFingerprint(uncompressed, algorithm);
	if (base64) {
		encoded = Base64Encode(encoded);
	}
	*encoded_fp = (char *) malloc(encoded.size() + 1);
	*encoded_size = int(encoded.size());
	std::copy(encoded.data(), encoded.data() + encoded.size() + 1, *encoded_fp);
	return 1;
}

int chromaprint_decode_fingerprint(const char *encoded_fp, int encoded_size, uint32_t **fp, int *size, int *algorithm, int base64)
{
	if (fp == NULL || size == NULL) {
		return 0;
	}
	std::vector<uint32_t> uncompressed;
	int algo;
	bool ok = false;
	if (encoded_size >= 0 && (encoded_fp != NULL || encoded_size == 0)) {
		std::string encoded(encoded_fp, encoded_size);
		if (base64) {
			encoded = Base64Decode(encoded);
		}
		ok = DecompressFingerprint(encoded, uncompressed, algo);
	}
	if (!ok) {
		*fp = nullptr;
		*size = 0;
		if (algorithm) {
			*algorithm = 0;
		}
		return 0;
	}
	*fp = (uint32_t *) malloc(sizeof(uint32_t) * uncompressed.size());
	*size = int(uncompressed.size());
	if (algorithm) {
		*algorithm = algo;
	}
	std::copy(uncompressed.begin(), uncompressed.end(), *fp);
	return 1;
}

int chromaprint_decode_fingerprint_header(const char *encoded_fp, int encoded_size, int *size, int *algorithm, int base64)
{
	size_t size_out;
	int algorithm_out;
	bool ok = false;
	if (encoded_size >= 0 && (encoded_fp != NULL || encoded_size == 0)) {
		std::string encoded(encoded_fp, std::min(6, encoded_size));
		if (base64) {
			encoded = Base64Decode(encoded);
		}
		ok = DecompressFingerprintHeader(encoded, size_out, algorithm_out);
	}
	if (!ok) {
		if (size) {
			*size = 0;
		}
		if (algorithm) {
			*algorithm = 0;
		}
		return 0;
	}
	if (size) {
		*size = int(size_out);
	}
	if (algorithm) {
		*algorithm = algorithm_out;
	}
	return 1;
}

int chromaprint_hash_fingerprint(const uint32_t *fp, int size, uint32_t *hash)
{
	if (fp == NULL || size < 0 || hash == NULL) {
		return 0;
	}
	*hash = SimHash(fp, size);
	return 1;
}

int chromaprint_match_ordered(const char *fp1, int size1,
                              const char *fp2, int size2,
                              int base64,
                              int max_bit_error, int min_duration,
                              ChromaprintSegment **segments,
                              int *num_segments)
{
	if (segments == NULL || num_segments == NULL) {
		return 0;
	}
	// Every exit path leaves valid output parameters in a cleared state.
	*segments = NULL;
	*num_segments = 0;

	if (size1 < 0 || size2 < 0 ||
	    (fp1 == NULL && size1 != 0) || (fp2 == NULL && size2 != 0)) {
		return 0;
	}

	std::vector<uint32_t> raw1;
	std::vector<uint32_t> raw2;
	int algorithm1 = -1;
	int algorithm2 = -1;

	// The buffers are read by their explicit lengths, binary data included,
	// so no NUL terminator is required.
	std::string encoded1(size1 != 0 ? fp1 : "", size1);
	if (base64) {
		encoded1 = Base64Decode(encoded1);
	}
	// A header shorter than four bytes or a declared item count that the
	// remaining data cannot satisfy makes DecompressFingerprint fail.
	if (!DecompressFingerprint(encoded1, raw1, algorithm1)) {
		return 0;
	}

	std::string encoded2(size2 != 0 ? fp2 : "", size2);
	if (base64) {
		encoded2 = Base64Decode(encoded2);
	}
	if (!DecompressFingerprint(encoded2, raw2, algorithm2)) {
		return 0;
	}

	if (algorithm1 < CHROMAPRINT_ALGORITHM_TEST1 ||
	    algorithm1 > CHROMAPRINT_ALGORITHM_TEST5 ||
	    algorithm1 != algorithm2) {
		return 0;
	}

	if (max_bit_error < 0 || max_bit_error > 32 || min_duration <= 0) {
		return 0;
	}

	FingerprintMatcher matcher(CreateFingerprinterConfiguration(algorithm1));
	matcher.MatchOrdered(raw1, raw2, max_bit_error, static_cast<size_t>(min_duration));

	const std::vector<Segment> &result = matcher.segments();
	if (!result.empty()) {
		ChromaprintSegment *output = static_cast<ChromaprintSegment *>(
			malloc(sizeof(ChromaprintSegment) * result.size()));
		if (output == NULL) {
			return 0;
		}
		for (size_t i = 0; i < result.size(); i++) {
			output[i].pos1 = static_cast<int>(result[i].pos1);
			output[i].pos2 = static_cast<int>(result[i].pos2);
			output[i].duration = static_cast<int>(result[i].duration);
			output[i].score = result[i].score;
		}
		*segments = output;
		*num_segments = static_cast<int>(result.size());
	}
	return 1;
}

void chromaprint_dealloc(void *ptr)
{
	free(ptr);
}

}; // extern "C"
