// Copyright (C) 2016  Lukas Lalinsky
// Distributed under the MIT license, see the LICENSE file for details.
//
// Tests for the ordered edit matching mode (FingerprintMatcher::MatchOrdered)
// and the chromaprint_match_ordered() C entry point. The reference chain used
// here is an independent exhaustive implementation over short fingerprints;
// the legacy single-offset Match() is never used as a reference.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <vector>

#include "chromaprint.h"
#include "fingerprint_matcher.h"
#include "fingerprinter.h"
#include "fingerprinter_configuration.h"
#include "utils.h"
#include "utils/scope_exit.h"

namespace chromaprint {
namespace {

uint32_t NextXorShift(uint32_t &state)
{
	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;
	return state;
}

std::vector<uint32_t> MakeSequence(size_t size, uint32_t seed = 0x31415926)
{
	std::vector<uint32_t> values;
	uint32_t state = seed;
	for (size_t i = 0; i < size; i++) {
		values.push_back(((uint32_t(i) * 37u % 4096u) << 20) | (NextXorShift(state) & 0xfffffu));
	}
	return values;
}

std::vector<Segment> RunOrdered(const std::vector<uint32_t> &fp1, const std::vector<uint32_t> &fp2,
                                int max_bit_error, size_t min_duration)
{
	FingerprintMatcher matcher(CreateFingerprinterConfiguration(CHROMAPRINT_ALGORITHM_TEST2));
	matcher.MatchOrdered(fp1, fp2, max_bit_error, min_duration);
	return matcher.segments();
}

// ---------------------------------------------------------------------------
// Independent exhaustive reference for short fingerprints
// ---------------------------------------------------------------------------

struct RefCandidate
{
	size_t pos1;
	size_t pos2;
	size_t duration;
	size_t errors;
};

std::vector<RefCandidate> ReferenceCandidates(const std::vector<uint32_t> &fp1,
                                              const std::vector<uint32_t> &fp2,
                                              int max_bit_error, size_t min_duration)
{
	std::vector<RefCandidate> candidates;
	for (ptrdiff_t d = -ptrdiff_t(fp2.size()) + 1; d <= ptrdiff_t(fp1.size()) - 1; d++) {
		const size_t start1 = d >= 0 ? size_t(d) : 0;
		const size_t start2 = d >= 0 ? 0 : size_t(-d);
		const size_t len = std::min(fp1.size() - start1, fp2.size() - start2);

		size_t run = 0;
		size_t errors = 0;
		for (size_t k = 0; k <= len; k++) {
			const bool ok = k < len &&
				HammingDistance(fp1[start1 + k], fp2[start2 + k]) <= unsigned(max_bit_error);
			if (ok) {
				run++;
				errors += HammingDistance(fp1[start1 + k], fp2[start2 + k]);
			} else {
				if (run >= min_duration) {
					candidates.push_back(RefCandidate{start1 + k - run, start2 + k - run, run, errors});
				}
				run = 0;
				errors = 0;
			}
		}
	}

	std::sort(candidates.begin(), candidates.end(), [](const RefCandidate &a, const RefCandidate &b) {
		if (a.pos1 != b.pos1) return a.pos1 < b.pos1;
		if (a.pos2 != b.pos2) return a.pos2 < b.pos2;
		return a.duration < b.duration;
	});
	return candidates;
}

struct RefChain
{
	size_t total_duration = 0;
	size_t total_errors = 0;
	size_t count = 0;
	std::vector<RefCandidate> segments;
};

bool RefBetter(const RefChain &a, const RefChain &b)
{
	if (a.total_duration != b.total_duration) {
		return a.total_duration > b.total_duration;
	}
	if (a.total_errors != b.total_errors) {
		return a.total_errors < b.total_errors;
	}
	if (a.count != b.count) {
		return a.count < b.count;
	}
	for (size_t i = 0; i < std::min(a.segments.size(), b.segments.size()); i++) {
		const RefCandidate &x = a.segments[i];
		const RefCandidate &y = b.segments[i];
		if (x.pos1 != y.pos1) return x.pos1 < y.pos1;
		if (x.pos2 != y.pos2) return x.pos2 < y.pos2;
		if (x.duration != y.duration) return x.duration < y.duration;
	}
	return a.segments.size() < b.segments.size();
}

RefChain ReferenceBestChain(const std::vector<RefCandidate> &candidates)
{
	RefChain best;
	std::vector<RefCandidate> chosen;

	std::function<void(size_t, const RefCandidate *)> search = [&](size_t index, const RefCandidate *last) {
		if (index == candidates.size()) {
			RefChain current;
			current.count = chosen.size();
			for (const auto &c : chosen) {
				current.total_duration += c.duration;
				current.total_errors += c.errors;
			}
			current.segments = chosen;
			if (RefBetter(current, best)) {
				best = current;
			}
			return;
		}

		search(index + 1, last);

		const RefCandidate &c = candidates[index];
		if (last == nullptr ||
		    (last->pos1 + last->duration <= c.pos1 && last->pos2 + last->duration <= c.pos2)) {
			chosen.push_back(c);
			search(index + 1, &c);
			chosen.pop_back();
		}
	};
	search(0, nullptr);
	return best;
}

void ExpectMatchesReference(const std::vector<uint32_t> &fp1, const std::vector<uint32_t> &fp2,
                            int max_bit_error, size_t min_duration)
{
	const auto candidates = ReferenceCandidates(fp1, fp2, max_bit_error, min_duration);
	const RefChain reference = ReferenceBestChain(candidates);

	const auto actual = RunOrdered(fp1, fp2, max_bit_error, min_duration);
	ASSERT_EQ(reference.segments.size(), actual.size());
	for (size_t i = 0; i < actual.size(); i++) {
		const RefCandidate &expected = reference.segments[i];
		EXPECT_EQ(expected.pos1, actual[i].pos1);
		EXPECT_EQ(expected.pos2, actual[i].pos2);
		EXPECT_EQ(expected.duration, actual[i].duration);
		EXPECT_DOUBLE_EQ(double(expected.errors) / double(expected.duration), actual[i].score);
	}
}

// ---------------------------------------------------------------------------
// Numeric baseline from tests/ordered/ordered_baseline.cpp
// ---------------------------------------------------------------------------

TEST(MatchOrdered, BaselineDeletion)
{
	const std::vector<uint32_t> full = MakeSequence(320);
	std::vector<uint32_t> edited(full.begin(), full.begin() + 112);
	edited.insert(edited.end(), full.begin() + 176, full.end());

	const auto segments = RunOrdered(full, edited, 0, 16);
	ASSERT_EQ(2u, segments.size());
	EXPECT_EQ(0u, segments[0].pos1);
	EXPECT_EQ(0u, segments[0].pos2);
	EXPECT_EQ(112u, segments[0].duration);
	EXPECT_DOUBLE_EQ(0.0, segments[0].score);
	EXPECT_EQ(176u, segments[1].pos1);
	EXPECT_EQ(112u, segments[1].pos2);
	EXPECT_EQ(144u, segments[1].duration);
	EXPECT_DOUBLE_EQ(0.0, segments[1].score);
}

// Insert foreign items and delete parts of the original recording.
TEST(MatchOrdered, InsertThenDelete)
{
	const std::vector<uint32_t> full = MakeSequence(40);
	// MakeSequence values stay below 0x5a400000, so values with the top byte
	// set can never collide with an original item.
	const uint32_t foreign1 = 0x80000001u;
	const uint32_t foreign2 = 0x80000002u;

	std::vector<uint32_t> edited;
	edited.insert(edited.end(), full.begin(), full.begin() + 16);
	edited.push_back(foreign1);
	edited.push_back(foreign2);
	edited.insert(edited.end(), full.begin() + 18, full.begin() + 32);
	edited.insert(edited.end(), full.begin() + 34, full.end());

	const auto segments = RunOrdered(full, edited, 0, 4);
	ASSERT_EQ(3u, segments.size());
	EXPECT_EQ(0u, segments[0].pos1);
	EXPECT_EQ(0u, segments[0].pos2);
	EXPECT_EQ(16u, segments[0].duration);
	EXPECT_EQ(18u, segments[1].pos1);
	EXPECT_EQ(18u, segments[1].pos2);
	EXPECT_EQ(14u, segments[1].duration);
	EXPECT_EQ(34u, segments[2].pos1);
	EXPECT_EQ(32u, segments[2].pos2);
	EXPECT_EQ(6u, segments[2].duration);
	for (const auto &s : segments) {
		EXPECT_DOUBLE_EQ(0.0, s.score);
	}
}

// A repeated chorus must not be claimed twice on the same time line.
TEST(MatchOrdered, RepeatedFragmentContention)
{
	std::vector<uint32_t> block;
	for (uint32_t i = 0; i < 8; i++) {
		block.push_back(0x100u + i);
	}
	std::vector<uint32_t> tail;
	for (uint32_t i = 0; i < 4; i++) {
		tail.push_back(0x200u + i);
	}
	const uint32_t garbage = 0x80000003u;

	std::vector<uint32_t> full = block;
	full.insert(full.end(), block.begin(), block.end());
	full.insert(full.end(), tail.begin(), tail.end());
	std::vector<uint32_t> edited = block;
	edited.push_back(garbage);
	edited.insert(edited.end(), tail.begin(), tail.end());

	const auto segments = RunOrdered(full, edited, 0, 4);
	// The block appears twice in the long recording but only once in the
	// edited one. Offset-0 and offset-8 candidates compete for the same edited
	// positions, so only one can be picked; both can chain the tail, giving
	// equal coverage, and the lexicographic tie-break selects the earlier
	// copy.
	ASSERT_EQ(2u, segments.size());
	EXPECT_EQ(0u, segments[0].pos1);
	EXPECT_EQ(0u, segments[0].pos2);
	EXPECT_EQ(8u, segments[0].duration);
	EXPECT_EQ(16u, segments[1].pos1);
	EXPECT_EQ(9u, segments[1].pos2);
	EXPECT_EQ(4u, segments[1].duration);

	// No edited position is covered by two segments.
	std::vector<bool> covered_edited(edited.size(), false);
	for (const auto &s : segments) {
		for (size_t k = 0; k < s.duration; k++) {
			EXPECT_FALSE(covered_edited[s.pos2 + k]);
			covered_edited[s.pos2 + k] = true;
		}
	}

	// The other direction: nothing on the shorter time line is claimed twice.
	const auto reverse = RunOrdered(edited, full, 0, 4);
	size_t reverse_covered = 0;
	for (const auto &s : reverse) {
		reverse_covered += s.duration;
	}
	EXPECT_LE(reverse_covered, edited.size());
}

// Candidates that would force one of the time lines backwards are rejected.
TEST(MatchOrdered, CrossOrderCandidates)
{
	const uint32_t v[] = {
		0x00000000u, 0xffffffffu, 0x55555555u, 0xaaaaaaaau,
		0x0f0f0f0fu, 0xf0f0f0f0u
	};
	const std::vector<uint32_t> full(v, v + 6);
	const uint32_t edited_data[] = { v[2], v[3], v[0], v[1] };
	const std::vector<uint32_t> edited(edited_data, edited_data + 4);

	const auto segments = RunOrdered(full, edited, 0, 2);
	// Candidates (0,2,2) and (2,0,2) cross on both axes; the lexicographically
	// smaller one wins.
	ASSERT_EQ(1u, segments.size());
	EXPECT_EQ(0u, segments[0].pos1);
	EXPECT_EQ(2u, segments[0].pos2);
	EXPECT_EQ(2u, segments[0].duration);
}

// Equal coverage: the chain with fewer bit errors wins.
TEST(MatchOrdered, TieBreakFewerBitErrors)
{
	// Candidate at offset 0 is a clean pair (0 bit errors). The candidate at
	// offset 2 covers the same two edited positions but carries 2 errors.
	const uint32_t fp1_data[] = { 0u, 0u, 1u, 2u };
	const uint32_t fp2_data[] = { 0u, 0u, 0xffffffffu };
	const std::vector<uint32_t> fp1(fp1_data, fp1_data + 4);
	const std::vector<uint32_t> fp2(fp2_data, fp2_data + 3);

	const auto segments = RunOrdered(fp1, fp2, 1, 2);
	ASSERT_EQ(1u, segments.size());
	EXPECT_EQ(0u, segments[0].pos1);
	EXPECT_EQ(0u, segments[0].pos2);
	EXPECT_EQ(2u, segments[0].duration);
	EXPECT_DOUBLE_EQ(0.0, segments[0].score);
}

// Equal coverage and equal bit errors: lexicographically smallest tuple wins.
TEST(MatchOrdered, TieBreakLexicographic)
{
	// Both competing pairs now carry exactly one bit error.
	const uint32_t fp1_data[] = { 0u, 0u, 1u, 2u };
	const uint32_t fp2_data[] = { 1u, 0u, 0xffffffffu };
	const std::vector<uint32_t> fp1(fp1_data, fp1_data + 4);
	const std::vector<uint32_t> fp2(fp2_data, fp2_data + 3);

	const auto segments = RunOrdered(fp1, fp2, 1, 2);
	ASSERT_EQ(1u, segments.size());
	EXPECT_EQ(0u, segments[0].pos1);
	EXPECT_EQ(0u, segments[0].pos2);
	EXPECT_EQ(2u, segments[0].duration);
	EXPECT_DOUBLE_EQ(0.5, segments[0].score);

	// Single-item candidates crossing each other: the pos1 tie-break decides,
	// then pos2 is exercised.
	const uint32_t short_data[] = { 0xaaaaaaaau, 0x55555555u };
	const std::vector<uint32_t> short_full(short_data, short_data + 2);
	const uint32_t swapped_data[] = { 0x55555555u, 0xaaaaaaaau };
	const std::vector<uint32_t> swapped(swapped_data, swapped_data + 2);
	const auto single = RunOrdered(short_full, swapped, 0, 1);
	ASSERT_EQ(1u, single.size());
	EXPECT_EQ(0u, single[0].pos1);
	EXPECT_EQ(1u, single[0].pos2);
	EXPECT_EQ(1u, single[0].duration);
}

// Differences confined to the highest bit must be found when the threshold
// allows them, and must not silently match at threshold 0.
TEST(MatchOrdered, HighBitOnlyTolerance)
{
	const std::vector<uint32_t> base = MakeSequence(5, 0x22222222);
	std::vector<uint32_t> flipped;
	for (uint32_t value : base) {
		flipped.push_back(value ^ 0x80000000u);
	}

	EXPECT_TRUE(RunOrdered(base, flipped, 0, 1).empty());
	const auto segments = RunOrdered(base, flipped, 1, 1);
	ASSERT_EQ(1u, segments.size());
	EXPECT_EQ(0u, segments[0].pos1);
	EXPECT_EQ(0u, segments[0].pos2);
	EXPECT_EQ(5u, segments[0].duration);
	EXPECT_DOUBLE_EQ(1.0, segments[0].score);
}

// Hamming distance exactly at the threshold is accepted, one more is rejected.
TEST(MatchOrdered, ThresholdExactlyEqual)
{
	const uint32_t fp1_data[] = { 0x10u, 0x20u, 0x30u, 0x40u };
	const uint32_t fp2_data[] = {
		0x10u ^ 0x03u, // distance 2
		0x20u ^ 0x03u, // distance 2
		0x30u ^ 0x07u, // distance 3, breaks the run
		0x40u ^ 0x03u  // distance 2
	};
	const std::vector<uint32_t> fp1(fp1_data, fp1_data + 4);
	const std::vector<uint32_t> fp2(fp2_data, fp2_data + 4);

	const auto segments = RunOrdered(fp1, fp2, 2, 1);
	ASSERT_EQ(2u, segments.size());
	EXPECT_EQ(0u, segments[0].pos1);
	EXPECT_EQ(0u, segments[0].pos2);
	EXPECT_EQ(2u, segments[0].duration);
	EXPECT_DOUBLE_EQ(2.0, segments[0].score);
	EXPECT_EQ(3u, segments[1].pos1);
	EXPECT_EQ(3u, segments[1].pos2);
	EXPECT_EQ(1u, segments[1].duration);
	EXPECT_DOUBLE_EQ(2.0, segments[1].score);
}

TEST(MatchOrdered, EmptyInputs)
{
	const std::vector<uint32_t> non_empty = MakeSequence(4);
	const std::vector<uint32_t> empty;

	EXPECT_TRUE(RunOrdered(non_empty, empty, 0, 1).empty());
	EXPECT_TRUE(RunOrdered(empty, non_empty, 0, 1).empty());
	EXPECT_TRUE(RunOrdered(empty, empty, 0, 1).empty());
}

TEST(MatchOrdered, InvalidParametersClearResults)
{
	const std::vector<uint32_t> a = MakeSequence(8);
	const std::vector<uint32_t> b(a.begin(), a.end());
	FingerprintMatcher matcher(CreateFingerprinterConfiguration(CHROMAPRINT_ALGORITHM_TEST2));

	ASSERT_TRUE(matcher.MatchOrdered(a, b, 0, 1));
	ASSERT_FALSE(matcher.segments().empty());

	EXPECT_FALSE(matcher.MatchOrdered(a, b, -1, 1));
	EXPECT_TRUE(matcher.segments().empty());
	EXPECT_FALSE(matcher.MatchOrdered(a, b, 33, 1));
	EXPECT_TRUE(matcher.segments().empty());
	EXPECT_FALSE(matcher.MatchOrdered(a, b, 0, 0));
	EXPECT_TRUE(matcher.segments().empty());

	// A no-match call also clears the previous result.
	const std::vector<uint32_t> foreign = MakeSequence(8, 0x77777777);
	ASSERT_TRUE(matcher.MatchOrdered(a, foreign, 0, 1));
	EXPECT_TRUE(matcher.segments().empty());

	// And a later successful call works normally.
	ASSERT_TRUE(matcher.MatchOrdered(a, b, 0, 1));
	ASSERT_EQ(1u, matcher.segments().size());
	EXPECT_EQ(8u, matcher.segments()[0].duration);
}

TEST(MatchOrdered, DoesNotConsumeRandomState)
{
	const std::vector<uint32_t> full = MakeSequence(64);
	std::vector<uint32_t> edited(full.begin(), full.begin() + 24);
	edited.insert(edited.end(), full.begin() + 40, full.end());

	srand(1);
	const auto first = RunOrdered(full, edited, 1, 4);
	// Burn global random state, including through the legacy matcher which
	// still uses rand().
	srand(987654);
	for (int i = 0; i < 100; i++) {
		(void) rand();
		FingerprintMatcher legacy(CreateFingerprinterConfiguration(CHROMAPRINT_ALGORITHM_TEST2));
		legacy.set_match_threshold(1.0);
		legacy.Match(full, edited);
	}
	const auto second = RunOrdered(full, edited, 1, 4);

	ASSERT_EQ(first.size(), second.size());
	for (size_t i = 0; i < first.size(); i++) {
		EXPECT_EQ(first[i].pos1, second[i].pos1);
		EXPECT_EQ(first[i].pos2, second[i].pos2);
		EXPECT_EQ(first[i].duration, second[i].duration);
		EXPECT_DOUBLE_EQ(first[i].score, second[i].score);
	}
}

// Brute-force comparison against the independent reference on many small
// randomized fingerprints with edits, flips and reorderings.
TEST(MatchOrdered, ExhaustiveReferenceOnShortFingerprints)
{
	uint32_t trial_state = 0x243f6a88;
	for (int trial = 0; trial < 300; trial++) {
		const size_t n1 = 4 + NextXorShift(trial_state) % 6;
		const size_t n2 = 3 + NextXorShift(trial_state) % 6;
		std::vector<uint32_t> fp1;
		for (size_t i = 0; i < n1; i++) {
			fp1.push_back(NextXorShift(trial_state));
		}

		std::vector<uint32_t> fp2;
		// Mix copied slices (with occasional bit flips) with foreign values so
		// that candidates appear on several competing offsets.
		size_t pos = 0;
		while (fp2.size() < n2) {
			if (pos < n1 && NextXorShift(trial_state) % 2 == 0) {
				const size_t take = std::min(n2 - fp2.size(),
				                             size_t(1 + NextXorShift(trial_state) % 3));
				for (size_t k = 0; k < take && pos < n1; k++) {
					uint32_t value = fp1[pos++];
					if (NextXorShift(trial_state) % 4 == 0) {
						value ^= 1u << (NextXorShift(trial_state) % 32);
					}
					fp2.push_back(value);
				}
			} else {
				fp2.push_back(NextXorShift(trial_state));
			}
		}

		const int max_bit_error = int(NextXorShift(trial_state) % 3);
		const size_t min_duration = 1 + NextXorShift(trial_state) % 3;
		ExpectMatchesReference(fp1, fp2, max_bit_error, min_duration);
	}
}

// ---------------------------------------------------------------------------
// End-to-end test on time-varying PCM through the real extraction and
// compression entry points
// ---------------------------------------------------------------------------

std::vector<int16_t> GeneratePcm(int num_samples)
{
	std::vector<int16_t> pcm;
	pcm.reserve(num_samples);
	for (int n = 0; n < num_samples; n++) {
		const double t = double(n) / 11025.0;
		const double progress = double(n) / double(num_samples);
		const double frequency = 220.0 + 500.0 * progress;
		const double sample =
			0.6 * sin(2.0 * M_PI * frequency * t) +
			0.25 * sin(2.0 * 3.0 * frequency * t + 0.7) +
			0.15 * sin(2.0 * 7.0 * (110.0 + 300.0 * progress) * t);
		const int scaled = int(sample * 24000.0);
		pcm.push_back(int16_t(std::max(-32768, std::min(32767, scaled))));
	}
	return pcm;
}

std::vector<uint32_t> ExtractFingerprint(const std::vector<int16_t> &pcm, int algorithm)
{
	Fingerprinter fingerprinter(CreateFingerprinterConfiguration(algorithm));
	EXPECT_TRUE(fingerprinter.Start(11025, 1));
	fingerprinter.Consume(pcm.data(), int(pcm.size()));
	fingerprinter.Finish();
	return fingerprinter.GetFingerprint();
}

TEST(MatchOrdered, ExtractedPcmWithMiddleCut)
{
	const int algorithm = CHROMAPRINT_ALGORITHM_TEST2;
	auto config = CreateFingerprinterConfiguration(algorithm);
	const int step = config->item_duration();
	// Items touching the cut can differ: the analysis frame spans
	// frame_size/step item steps and the classifier filter looks
	// max_filter_width() rows back. Everything at least this many items away
	// from the seam is an exact-match zone; the seam itself is excluded from
	// the exact checks and left to the matcher.
	const int seam_margin = config->frame_size() / step + config->max_filter_width() + 2;
	delete config;

	// Roughly 125 fingerprint items once the FFT window and filter lag are paid.
	const int num_samples = 200000;
	const std::vector<int16_t> full_pcm = GeneratePcm(num_samples);

	// Cut boundaries on integer multiples of the algorithm step.
	const int cut_items = 40;
	const int removed_items = 20;
	const int cut_sample = cut_items * step;
	const int removed_samples = removed_items * step;

	std::vector<int16_t> edited_pcm;
	edited_pcm.reserve(num_samples - removed_samples);
	edited_pcm.insert(edited_pcm.end(), full_pcm.begin(), full_pcm.begin() + cut_sample);
	edited_pcm.insert(edited_pcm.end(), full_pcm.begin() + cut_sample + removed_samples,
	                  full_pcm.end());

	const std::vector<uint32_t> full_fp = ExtractFingerprint(full_pcm, algorithm);
	const std::vector<uint32_t> edited_fp = ExtractFingerprint(edited_pcm, algorithm);
	ASSERT_GT(full_fp.size(), size_t(cut_items + removed_items + 10));
	ASSERT_GT(edited_fp.size(), size_t(cut_items + 10));

	// Frames that do not touch the seam must produce identical items, on both
	// sides of the cut. The seam itself is handled by the margin below.
	for (size_t i = 0; i + size_t(seam_margin) < size_t(cut_items); i++) {
		EXPECT_EQ(full_fp[i], edited_fp[i]) << "prefix item " << i;
	}
	for (size_t j = size_t(cut_items) + seam_margin; j + 2 < edited_fp.size(); j++) {
		ASSERT_LT(j + removed_items, full_fp.size());
		EXPECT_EQ(full_fp[j + removed_items], edited_fp[j]) << "suffix item " << j;
	}

	// Round-trip through both compressed formats before matching.
	char *encoded_full_bin = nullptr;
	char *encoded_full_b64 = nullptr;
	char *encoded_edited_bin = nullptr;
	char *encoded_edited_b64 = nullptr;
	int full_bin_size = 0, full_b64_size = 0;
	int edited_bin_size = 0, edited_b64_size = 0;
	ASSERT_EQ(1, chromaprint_encode_fingerprint(full_fp.data(), int(full_fp.size()), algorithm,
	                                            &encoded_full_bin, &full_bin_size, 0));
	ASSERT_EQ(1, chromaprint_encode_fingerprint(full_fp.data(), int(full_fp.size()), algorithm,
	                                            &encoded_full_b64, &full_b64_size, 1));
	ASSERT_EQ(1, chromaprint_encode_fingerprint(edited_fp.data(), int(edited_fp.size()), algorithm,
	                                            &encoded_edited_bin, &edited_bin_size, 0));
	ASSERT_EQ(1, chromaprint_encode_fingerprint(edited_fp.data(), int(edited_fp.size()), algorithm,
	                                            &encoded_edited_b64, &edited_b64_size, 1));
	SCOPE_EXIT(chromaprint_dealloc(encoded_full_bin));
	SCOPE_EXIT(chromaprint_dealloc(encoded_full_b64));
	SCOPE_EXIT(chromaprint_dealloc(encoded_edited_bin));
	SCOPE_EXIT(chromaprint_dealloc(encoded_edited_b64));

	uint32_t *decoded_full = nullptr;
	uint32_t *decoded_edited = nullptr;
	int decoded_full_size = 0, decoded_edited_size = 0, decoded_algorithm = 0;
	ASSERT_EQ(1, chromaprint_decode_fingerprint(encoded_full_bin, full_bin_size,
	                                            &decoded_full, &decoded_full_size,
	                                            &decoded_algorithm, 0));
	SCOPE_EXIT(chromaprint_dealloc(decoded_full));
	ASSERT_EQ(algorithm, decoded_algorithm);
	ASSERT_EQ(int(full_fp.size()), decoded_full_size);
	for (int i = 0; i < decoded_full_size; i++) {
		EXPECT_EQ(full_fp[i], decoded_full[i]);
	}
	ASSERT_EQ(1, chromaprint_decode_fingerprint(encoded_edited_bin, edited_bin_size,
	                                            &decoded_edited, &decoded_edited_size,
	                                            &decoded_algorithm, 0));
	SCOPE_EXIT(chromaprint_dealloc(decoded_edited));
	ASSERT_EQ(int(edited_fp.size()), decoded_edited_size);

	const std::vector<uint32_t> full_decoded(decoded_full, decoded_full + decoded_full_size);
	const std::vector<uint32_t> edited_decoded(decoded_edited, decoded_edited + decoded_edited_size);
	const auto segments = RunOrdered(full_decoded, edited_decoded, 0, 8);
	ASSERT_GE(segments.size(), 2u);

	// Prefix segment starts at the origin and survives well past the safe zone.
	EXPECT_EQ(0u, segments.front().pos1);
	EXPECT_EQ(0u, segments.front().pos2);
	EXPECT_GE(segments.front().duration, size_t(cut_items - seam_margin));

	// Suffix segment keeps the removed-item offset and reaches the tail.
	const Segment &suffix = segments.back();
	EXPECT_EQ(suffix.pos1 - suffix.pos2, size_t(removed_items));
	EXPECT_GE(suffix.pos1, size_t(cut_items + removed_items - seam_margin));
	EXPECT_GE(suffix.duration, 8u);
	EXPECT_LE(suffix.pos2 + suffix.duration, edited_fp.size());
	EXPECT_LE(edited_fp.size() - (suffix.pos2 + suffix.duration), size_t(seam_margin));

	// The C entry point on both binary and base64 data reports the same
	// coordinates as the C++ matcher.
	ChromaprintSegment *c_bin = nullptr;
	int c_bin_count = 0;
	ASSERT_EQ(1, chromaprint_match_ordered(encoded_full_bin, full_bin_size,
	                                       encoded_edited_bin, edited_bin_size,
	                                       0, 0, 8, &c_bin, &c_bin_count));
	SCOPE_EXIT(chromaprint_dealloc(c_bin));
	ASSERT_EQ(int(segments.size()), c_bin_count);
	for (int i = 0; i < c_bin_count; i++) {
		EXPECT_EQ(int(segments[i].pos1), c_bin[i].pos1);
		EXPECT_EQ(int(segments[i].pos2), c_bin[i].pos2);
		EXPECT_EQ(int(segments[i].duration), c_bin[i].duration);
		EXPECT_DOUBLE_EQ(segments[i].score, c_bin[i].score);
	}

	ChromaprintSegment *c_b64 = nullptr;
	int c_b64_count = 0;
	ASSERT_EQ(1, chromaprint_match_ordered(encoded_full_b64, full_b64_size,
	                                       encoded_edited_b64, edited_b64_size,
	                                       1, 0, 8, &c_b64, &c_b64_count));
	SCOPE_EXIT(chromaprint_dealloc(c_b64));
	ASSERT_EQ(c_bin_count, c_b64_count);
	for (int i = 0; i < c_b64_count; i++) {
		EXPECT_EQ(c_bin[i].pos1, c_b64[i].pos1);
		EXPECT_EQ(c_bin[i].pos2, c_b64[i].pos2);
		EXPECT_EQ(c_bin[i].duration, c_b64[i].duration);
		EXPECT_DOUBLE_EQ(c_bin[i].score, c_b64[i].score);
	}
}

// Time conversion comes from each algorithm's own configuration; the analysis
// delay belongs to the time line once, while segment durations are pure item
// spans and must never add the delay per segment.
TEST(MatchOrdered, TimeConversionTest2AndTest3)
{
	const int algorithms[] = {
		CHROMAPRINT_ALGORITHM_TEST2,
		CHROMAPRINT_ALGORITHM_TEST3
	};

	for (int algorithm : algorithms) {
		SCOPED_TRACE(algorithm);
		// The matcher takes ownership of this configuration, so all time
		// conversion reads go through the same instance.
		FingerprinterConfiguration *config = CreateFingerprinterConfiguration(algorithm);
		const int step_samples = config->item_duration();
		const double item_seconds = config->item_duration_in_seconds();
		const double delay_seconds = config->delay_in_seconds();

		// TEST2 uses the overlapping 4096-sample frames (step 1365); TEST3
		// leaves frame_overlap at zero, so its step is the whole frame.
		if (algorithm == CHROMAPRINT_ALGORITHM_TEST2) {
			EXPECT_EQ(4096 - (4096 - 4096 / 3), step_samples);
		} else {
			EXPECT_EQ(4096, step_samples);
		}
		EXPECT_DOUBLE_EQ(double(step_samples) / 11025.0, item_seconds);
		EXPECT_DOUBLE_EQ(double(config->delay()) / 11025.0, delay_seconds);

		const std::vector<uint32_t> full = MakeSequence(80);
		std::vector<uint32_t> edited(full.begin(), full.begin() + 24);
		edited.insert(edited.end(), full.begin() + 40, full.end());

		FingerprintMatcher matcher(config);
		ASSERT_TRUE(matcher.MatchOrdered(full, edited, 0, 8));
		const auto &found = matcher.segments();
		ASSERT_GE(found.size(), 2u);

		double matched_item_seconds = 0.0;
		for (const auto &segment : found) {
			// Item positions map straight onto the time line.
			EXPECT_DOUBLE_EQ(double(segment.pos1) * item_seconds,
			                 matcher.GetHashTime(segment.pos1));
			// The extraction delay is added exactly once for a position.
			EXPECT_DOUBLE_EQ(double(segment.pos1) * item_seconds + delay_seconds,
			                 matcher.GetHashDuration(segment.pos1));
			// A segment's duration is a pure span of items, no delay term.
			EXPECT_DOUBLE_EQ(double(segment.duration) * item_seconds,
			                 matcher.GetHashTime(segment.pos1 + segment.duration)
			                     - matcher.GetHashTime(segment.pos1));
			matched_item_seconds += double(segment.duration) * item_seconds;
		}

		// The wall-clock span covered by the matched audio contains a single
		// delay, not one delay per segment.
		const Segment &last = found.back();
		const double covered_with_one_delay =
			matched_item_seconds + delay_seconds;
		const double covered_with_delay_per_segment =
			matched_item_seconds + delay_seconds * double(found.size());
		EXPECT_NE(covered_with_one_delay, covered_with_delay_per_segment);
		EXPECT_DOUBLE_EQ(double(last.pos1 + last.duration) * item_seconds + delay_seconds,
		                 matcher.GetHashDuration(last.pos1 + last.duration - 1) + item_seconds);
	}
}

} // namespace
} // namespace chromaprint
