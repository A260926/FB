// Copyright (C) 2016  Lukas Lalinsky
// Distributed under the MIT license, see the LICENSE file for details.

#include <algorithm>
#include <numeric>
#include <iostream>
#include "fingerprint_matcher.h"
#include "fingerprinter_configuration.h"
#include "utils.h"
#include "utils/gaussian_filter.h"
#include "utils/gradient.h"
#include "debug.h"

namespace chromaprint {

namespace {

// A maximal run along a single offset: every pairwise Hamming distance in the
// run is at most the threshold, neither end can be extended, and the run is at
// least as long as the requested minimum duration.
struct OrderedCandidate
{
	size_t pos1;
	size_t pos2;
	size_t duration;
	size_t bit_errors;
};

// Comparison of the chain selection metrics that do not depend on the segment
// tuple sequence. Returns true if the first chain is strictly better.
bool ChainMetricsBetter(size_t duration1, size_t errors1, size_t count1,
                        size_t duration2, size_t errors2, size_t count2)
{
	if (duration1 != duration2) {
		return duration1 > duration2; // most matched items first
	}
	if (errors1 != errors2) {
		return errors1 < errors2; // then fewest total bit errors
	}
	return count1 < count2; // then fewest segments
}

// Reconstruct the candidate indices of a chain, ending at last_index and
// following predecessor links, in chain order.
std::vector<size_t> ReconstructChain(int last_index, const std::vector<int> &previous)
{
	std::vector<size_t> chain;
	for (int index = last_index; index >= 0; index = previous[static_cast<size_t>(index)]) {
		chain.push_back(static_cast<size_t>(index));
	}
	std::reverse(chain.begin(), chain.end());
	return chain;
}

// Lexicographic comparison of the (pos1, pos2, duration) tuple sequences.
bool ChainTupleLexicographicallyLess(const std::vector<size_t> &chain1,
                                     const std::vector<size_t> &chain2,
                                     const std::vector<OrderedCandidate> &candidates)
{
	const size_t count = std::min(chain1.size(), chain2.size());
	for (size_t i = 0; i < count; i++) {
		const OrderedCandidate &a = candidates[chain1[i]];
		const OrderedCandidate &b = candidates[chain2[i]];
		if (a.pos1 != b.pos1) {
			return a.pos1 < b.pos1;
		}
		if (a.pos2 != b.pos2) {
			return a.pos2 < b.pos2;
		}
		if (a.duration != b.duration) {
			return a.duration < b.duration;
		}
	}
	return chain1.size() < chain2.size();
}

}; // namespace

/* fingerprint matcher settings */
#define ACOUSTID_MAX_BIT_ERROR 2
#define ACOUSTID_MAX_ALIGN_OFFSET 120
#define ACOUSTID_QUERY_START 80
#define ACOUSTID_QUERY_LENGTH 120
#define ACOUSTID_QUERY_BITS 28
#define ACOUSTID_QUERY_MASK (((1<<ACOUSTID_QUERY_BITS)-1)<<(32-ACOUSTID_QUERY_BITS))
#define ACOUSTID_QUERY_STRIP(x) ((x) & ACOUSTID_QUERY_MASK)

#define ALIGN_BITS 12
#define ALIGN_MASK ((1 << ALIGN_BITS) - 1)
#define ALIGN_STRIP(x) ((uint32_t)(x) >> (32 - ALIGN_BITS))

#define UNIQ_BITS 16
#define UNIQ_MASK ((1 << MATCH_BITS) - 1)
#define UNIQ_STRIP(x) ((uint32_t)(x) >> (32 - MATCH_BITS))

FingerprintMatcher::FingerprintMatcher(FingerprinterConfiguration *config)
	: m_config(config)
{
}

double FingerprintMatcher::GetHashTime(size_t i) const {
	return m_config->item_duration_in_seconds() * i;
}

double FingerprintMatcher::GetHashDuration(size_t i) const {
	return GetHashTime(i) + m_config->delay_in_seconds();
}

bool FingerprintMatcher::Match(const std::vector<uint32_t> &fp1, const std::vector<uint32_t> &fp2)
{
	return Match(fp1.data(), fp1.size(), fp2.data(), fp2.size());
}

bool FingerprintMatcher::Match(const uint32_t fp1_data[], size_t fp1_size, const uint32_t fp2_data[], size_t fp2_size)
{
	const uint32_t hash_shift = 32 - ALIGN_BITS;
	const uint32_t hash_mask = ((1u << ALIGN_BITS) - 1) << hash_shift;
	const uint32_t offset_mask = (1u << (32 - ALIGN_BITS - 1)) - 1;
	const uint32_t source_mask = 1u << (32 - ALIGN_BITS - 1);

	if (fp1_size + 1 >= offset_mask) {
		DEBUG("chromaprint::FingerprintMatcher::Match() -- Fingerprint 1 too long.");
		return false;
	}
	if (fp2_size + 1 >= offset_mask) {
		DEBUG("chromaprint::FingerprintMatcher::Match() -- Fingerprint 2 too long.");
		return false;
	}

	m_offsets.clear();
	m_offsets.reserve(fp1_size + fp2_size);
	for (size_t i = 0; i < fp1_size; i++) {
		m_offsets.push_back((ALIGN_STRIP(fp1_data[i]) << hash_shift) | uint32_t(i & offset_mask));
	}
	for (size_t i = 0; i < fp2_size; i++) {
		m_offsets.push_back((ALIGN_STRIP(fp2_data[i]) << hash_shift) | uint32_t(i & offset_mask) | source_mask);
	}
	std::sort(m_offsets.begin(), m_offsets.end());

	m_histogram.assign(fp1_size + fp2_size, 0);
	for (auto it = m_offsets.cbegin(); it != m_offsets.cend(); ++it) {
		const uint32_t hash = (*it) & hash_mask;
		const uint32_t offset1 = (*it) & offset_mask;
		const uint32_t source1 = (*it) & source_mask;
		if (source1 != 0) {
			// if we got hash from fp2, it means there is no hash from fp1,
			// because if there was, it would be first
			continue;
		}
		auto it2 = it;
		while (++it2 != m_offsets.end()) {
			const uint32_t hash2 = (*it2) & hash_mask;
			if (hash != hash2) {
				break;
			}
			const uint32_t offset2 = (*it2) & offset_mask;
			const uint32_t source2 = (*it2) & source_mask;
			if (source2 != 0) {
				const size_t offset_diff = offset1 + fp2_size - offset2;
				m_histogram[offset_diff] += 1;
			}
		}
	}

	m_best_alignments.clear();
	const auto histogram_size = m_histogram.size();
	for (size_t i = 0; i < histogram_size; i++) {
		const uint32_t count = m_histogram[i];
		if (m_histogram[i] > 1) {
			const bool is_peak_left = (i > 0) ? m_histogram[i - 1] <= count : true;
			const bool is_peak_right = (i < histogram_size - 1) ? m_histogram[i + 1] <= count : true;
			if (is_peak_left && is_peak_right) {
				m_best_alignments.push_back(std::make_pair(count, i));
			}
		}
	}
	std::sort(m_best_alignments.rbegin(), m_best_alignments.rend());

	m_segments.clear();

	for (const auto &item : m_best_alignments) {
		const int offset_diff = int(item.second - fp2_size);

		const size_t offset1 = offset_diff > 0 ? offset_diff : 0;
		const size_t offset2 = offset_diff < 0 ? -offset_diff : 0;

		auto it1 = fp1_data + offset1;
		auto it2 = fp2_data + offset2;

		const auto size = std::min(fp1_size - offset1, fp2_size - offset2);
		std::vector<float> bit_counts(size);
		for (size_t i = 0; i < size; i++) {
			bit_counts[i] = HammingDistance(*it1++, *it2++) + rand() * (0.001f / RAND_MAX);
		}

		std::vector<float> orig_bit_counts = bit_counts;
		std::vector<float> smoothed_bit_counts;
		GaussianFilter(bit_counts, smoothed_bit_counts, 8.0, 3);

		std::vector<float> gradient(size);
		Gradient(smoothed_bit_counts.begin(), smoothed_bit_counts.end(), gradient.begin());

		for (size_t i = 0; i < size; i++) {
			gradient[i] = std::abs(gradient[i]);
		}

		std::vector<size_t> gradient_peaks;
		for (size_t i = 0; i < size; i++) {
			const auto gi = gradient[i];
			if (i > 0 && i < size - 1 && gi > 0.15 && gi >= gradient[i - 1] && gi >= gradient[i + 1]) {
				if (gradient_peaks.empty() || gradient_peaks.back() + 1 < i) {
					gradient_peaks.push_back(i);
				}
			}
		}
		gradient_peaks.push_back(size);

		size_t begin = 0;
		for (size_t end : gradient_peaks) {
			const auto duration = end - begin;
			const auto score = std::accumulate(orig_bit_counts.begin() + begin, orig_bit_counts.begin() + end, 0.0) / duration;
			if (score < m_match_threshold) {
				bool added = false;
				if (!m_segments.empty()) {
					auto &s1 = m_segments.back();
					if (std::abs(s1.score - score) < 0.7) {
						s1 = s1.merged(Segment(offset1 + begin, offset2 + begin, duration, score));
						added = true;
					}
				}
				if (!added) {
					m_segments.emplace_back(offset1 + begin, offset2 + begin, duration, score);
				}
			}
			begin = end;
		}

		// TODO try to merge segments from multiple offsets

		break;
	}

	return true;
}

bool FingerprintMatcher::MatchOrdered(const std::vector<uint32_t> &fp1, const std::vector<uint32_t> &fp2,
                                      int max_bit_error, size_t min_duration)
{
	// Always start from an empty result so that stale segments can never leak
	// into a later call, including failure and no-match cases.
	m_segments.clear();

	if (max_bit_error < 0 || max_bit_error > 32 || min_duration == 0) {
		return false;
	}

	const size_t n1 = fp1.size();
	const size_t n2 = fp2.size();
	if (n1 == 0 || n2 == 0) {
		return true;
	}

	// Enumerate every offset d = pos1 - pos2. Each diagonal is split into
	// maximal runs where every pairwise Hamming distance is within the
	// threshold. A run is a candidate only if it cannot be extended at either
	// end and it is at least min_duration items long.
	std::vector<OrderedCandidate> candidates;
	for (ptrdiff_t d = -ptrdiff_t(n2) + 1; d <= ptrdiff_t(n1) - 1; d++) {
		const size_t start1 = d >= 0 ? size_t(d) : 0;
		const size_t start2 = d >= 0 ? 0 : size_t(-d);
		const size_t len = std::min(n1 - start1, n2 - start2);

		size_t run_duration = 0;
		size_t run_errors = 0;
		for (size_t k = 0; k <= len; k++) {
			unsigned int dist = 0;
			const bool within =
				k < len && (dist = HammingDistance(fp1[start1 + k], fp2[start2 + k])) <= unsigned(max_bit_error);
			if (within) {
				run_duration++;
				run_errors += dist;
			}
			else {
				if (run_duration >= min_duration) {
					candidates.push_back(OrderedCandidate{
						start1 + k - run_duration, start2 + k - run_duration, run_duration, run_errors});
				}
				run_duration = 0;
				run_errors = 0;
			}
		}
	}

	if (candidates.empty()) {
		return true;
	}

	// Canonical enumeration order so equivalent inputs give identical results.
	std::sort(candidates.begin(), candidates.end(),
	          [](const OrderedCandidate &a, const OrderedCandidate &b) {
		if (a.pos1 != b.pos1) return a.pos1 < b.pos1;
		if (a.pos2 != b.pos2) return a.pos2 < b.pos2;
		if (a.duration != b.duration) return a.duration < b.duration;
		return a.bit_errors < b.bit_errors;
	});

	// Dynamic programming over candidates. A chain ending at candidate i may
	// append candidate j only when j starts at or after i ends on both axes,
	// so segments stay ordered and never overlap on either time line.
	struct ChainState {
		size_t total_duration = 0;
		size_t total_errors = 0;
		size_t segment_count = 0;
	};

	const size_t num_candidates = candidates.size();
	std::vector<ChainState> best(num_candidates);
	std::vector<int> previous(num_candidates, -1);

	auto metrics_equal = [](const ChainState &a, const ChainState &b) {
		return a.total_duration == b.total_duration &&
		       a.total_errors == b.total_errors &&
		       a.segment_count == b.segment_count;
	};

	for (size_t i = 0; i < num_candidates; i++) {
		const OrderedCandidate &current = candidates[i];
		best[i] = ChainState{current.duration, current.bit_errors, 1};
		previous[i] = -1;

		for (size_t j = 0; j < i; j++) {
			const OrderedCandidate &earlier = candidates[j];
			if (earlier.pos1 + earlier.duration > current.pos1 ||
			    earlier.pos2 + earlier.duration > current.pos2) {
				continue;
			}

			ChainState candidate_state{
				best[j].total_duration + current.duration,
				best[j].total_errors + current.bit_errors,
				best[j].segment_count + 1
			};

			bool take = false;
			if (ChainMetricsBetter(candidate_state.total_duration, candidate_state.total_errors,
			                       candidate_state.segment_count,
			                       best[i].total_duration, best[i].total_errors,
			                       best[i].segment_count)) {
				take = true;
			}
			else if (metrics_equal(candidate_state, best[i])) {
				// Final tie-break: lexicographically smallest tuple sequence.
				const auto candidate_chain = ReconstructChain(int(j), previous);
				const auto current_chain = ReconstructChain(previous[i], previous);
				take = ChainTupleLexicographicallyLess(candidate_chain, current_chain, candidates);
			}

			if (take) {
				best[i] = candidate_state;
				previous[i] = int(j);
			}
		}
	}

	// Select the best chain among all possible end points.
	size_t best_index = 0;
	for (size_t i = 1; i < num_candidates; i++) {
		if (ChainMetricsBetter(best[i].total_duration, best[i].total_errors, best[i].segment_count,
		                       best[best_index].total_duration, best[best_index].total_errors,
		                       best[best_index].segment_count)) {
			best_index = i;
		}
		else if (metrics_equal(best[i], best[best_index])) {
			const auto chain_i = ReconstructChain(int(i), previous);
			const auto chain_best = ReconstructChain(int(best_index), previous);
			if (ChainTupleLexicographicallyLess(chain_i, chain_best, candidates)) {
				best_index = i;
			}
		}
	}

	for (size_t index : ReconstructChain(int(best_index), previous)) {
		const OrderedCandidate &candidate = candidates[index];
		const double score = double(candidate.bit_errors) / double(candidate.duration);
		m_segments.emplace_back(candidate.pos1, candidate.pos2, candidate.duration, score);
	}

	return true;
}

}; // namespace chromaprint
