/*
 * Standalone executable driver for the structured serialization model.
 *
 * Shares the exact checking core with fuzz_serialize_model.cpp.  It runs the
 * fixed alignment families first and then a deterministic --cases sweep.
 *
 * Usage:
 *   serialize_model_main [--seed N] [--cases N] [--input HEX] [--family N]
 *                        [--skip-fixed] [--dump-config]
 */

#include "serialize_model_core.hpp"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

namespace
{
	void usage()
	{
		fprintf(stderr, "usage: serialize_model_main [--seed N] [--cases N] [--input HEX] [--family N] [--skip-fixed] [--dump-config] [--dump-fixed-count] [--check-fixed-invariants]\n");
	}

	bool parse_unsigned(const char* text, size_t& out)
	{
		if (!text || !*text) return false;

		char* end = 0;
		unsigned long long value = strtoull(text, &end, 0);

		if (!end || *end != 0) return false;

		out = static_cast<size_t>(value);
		return true;
	}

	bool parse_hex_bytes(const char* text, std::vector<uint8_t>& out)
	{
		size_t length = strlen(text);

		// An empty hex string denotes the empty fuzz input; it is a valid,
		// replayable input for an arbitrary-byte entry point.
		if (length == 0) return true;

		if (length % 2 != 0) return false;

		for (size_t i = 0; i < length; i += 2)
		{
			char buffer[3];
			buffer[0] = text[i];
			buffer[1] = text[i + 1];
			buffer[2] = 0;

			char* end = 0;
			unsigned long value = strtoul(buffer, &end, 16);

			if (!end || *end != 0 || value > 0xff) return false;

			out.push_back(static_cast<uint8_t>(value));
		}

		return true;
	}

	bool run_one(const std::string& label, const uint8_t* data, size_t size, size_t* executions)
	{
		serialize_model::Failure failure;

		if (!serialize_model::check_input(data, size, failure, executions))
		{
			fprintf(stderr, "FAILED %s\n", label.c_str());
			serialize_model::print_failure(failure, stderr);
			return false;
		}

		return true;
	}

	// Deterministic 16-byte input for sweep case `index` and `seed`.
	void sweep_bytes(size_t seed, size_t index, uint8_t* bytes, size_t count)
	{
		uint64_t state = static_cast<uint64_t>(seed) + (static_cast<uint64_t>(index) << 32);

		for (size_t b = 0; b < count; ++b)
		{
			state = (state ^ (state >> 33)) * 0xff51afd7ed558ccdULL;
			state ^= state >> 33;

			bytes[b] = static_cast<uint8_t>((state >> 24) & 0xff);
		}
	}
}

int main(int argc, char** argv)
{
	size_t seed = 1319;
	size_t cases = 120;
	size_t family = 0;
	bool have_family = false;
	bool skip_fixed = false;
	std::string input_text;
	bool have_input = false;

	for (int i = 1; i < argc; ++i)
	{
		const char* arg = argv[i];

		if (strcmp(arg, "--seed") == 0)
		{
			if (i + 1 >= argc || !parse_unsigned(argv[++i], seed))
			{
				usage();
				return 2;
			}
		}
		else if (strcmp(arg, "--cases") == 0)
		{
			if (i + 1 >= argc || !parse_unsigned(argv[++i], cases))
			{
				usage();
				return 2;
			}
		}
		else if (strcmp(arg, "--family") == 0)
		{
			if (i + 1 >= argc || !parse_unsigned(argv[++i], family))
			{
				usage();
				return 2;
			}

			have_family = true;
		}
		else if (strcmp(arg, "--input") == 0)
		{
			if (i + 1 >= argc)
			{
				usage();
				return 2;
			}

			input_text = argv[++i];
			have_input = true;
		}
		else if (strcmp(arg, "--skip-fixed") == 0)
		{
			skip_fixed = true;
		}
		else if (strcmp(arg, "--dump-config") == 0)
		{
			printf("config %s\n", serialize_model::config_id().c_str());
			return 0;
		}
		else if (strcmp(arg, "--dump-fixed-count") == 0)
		{
			printf("%zu\n", serialize_model::fixed_family_count());
			return 0;
		}
		else if (strcmp(arg, "--check-fixed-invariants") == 0)
		{
			serialize_model::InvariantReport report;
			serialize_model::check_fixed_suite_invariants(report);

			if (!report.ok)
			{
				fprintf(stderr, "MODEL_INVARIANT_FAILED: %s\n", report.detail.c_str());
				printf("SERIALIZE_MODEL_DRIVER_FAILED\n");
				return 1;
			}

			printf("SERIALIZE_MODEL_DRIVER_OK\n");
			return 0;
		}
		else
		{
			usage();
			return 2;
		}
	}

	printf("config %s\n", serialize_model::config_id().c_str());

	size_t executions[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	size_t cases_run = 0;
	size_t fixed_run = 0;
	bool ok = true;

	size_t encoding_count = 0;
	const serialize_model::OutputEncoding* encodings = serialize_model::output_encodings(encoding_count);

	if (encoding_count > 8) encoding_count = 8;

	auto report_counts = [&]()
	{
		for (size_t e = 0; e < encoding_count; ++e)
			printf("executions encoding=%s count=%zu\n", encodings[e].name, executions[e]);

		printf("cases=%zu fixed=%zu\n", cases_run, fixed_run);
	};

	// Replay path: run the given bytes once through the same fuzz entry.
	if (have_input)
	{
		std::vector<uint8_t> bytes;

		if (!parse_hex_bytes(input_text.c_str(), bytes))
		{
			fprintf(stderr, "invalid --input hex\n");
			return 2;
		}

		uint8_t empty = 0;
		ok = run_one("input", bytes.empty() ? &empty : &bytes[0], bytes.size(), executions);
		cases_run = 1;
		fixed_run = 0;
		report_counts();

		if (ok)
		{
			printf("SERIALIZE_MODEL_DRIVER_OK\n");
			return 0;
		}

		printf("SERIALIZE_MODEL_DRIVER_FAILED\n");
		return 1;
	}

	// Optional single fixed family (used by the mutant verification).
	if (have_family)
	{
		if (family >= serialize_model::fixed_family_count())
		{
			fprintf(stderr, "invalid --family %zu\n", family);
			return 2;
		}

		uint8_t bytes[4] = { 'S', 'M', 'F', static_cast<uint8_t>(family) };
		ok = run_one(serialize_model::fixed_family_name(family), bytes, sizeof(bytes), executions);
		cases_run = 0;
		fixed_run = 1;
		printf("fixed family=%s\n", serialize_model::fixed_family_name(family));
		report_counts();

		if (ok)
		{
			printf("SERIALIZE_MODEL_DRIVER_OK\n");
			return 0;
		}

		printf("SERIALIZE_MODEL_DRIVER_FAILED\n");
		return 1;
	}

	if (!skip_fixed)
	{
		for (size_t f = 0; f < serialize_model::fixed_family_count(); ++f)
		{
			uint8_t bytes[4] = { 'S', 'M', 'F', static_cast<uint8_t>(f) };

			if (!run_one(serialize_model::fixed_family_name(f), bytes, sizeof(bytes), executions))
			{
				ok = false;
				break;
			}

			printf("fixed family=%s\n", serialize_model::fixed_family_name(f));
			++fixed_run;
		}
	}

	if (ok)
	{
		for (size_t c = 0; c < cases; ++c)
		{
			uint8_t bytes[16];
			sweep_bytes(seed, c, bytes, sizeof(bytes));

			serialize_model::InvariantReport report;
			serialize_model::check_case_invariants(serialize_model::generate_case(bytes, sizeof(bytes)), report, true);

			if (!report.ok)
			{
				fprintf(stderr, "MODEL_INVARIANT_FAILED: %s\n", report.detail.c_str());
				ok = false;
				break;
			}

			if (!run_one(std::string("case_") + std::to_string(c), bytes, sizeof(bytes), executions))
			{
				ok = false;
				break;
			}

			++cases_run;
		}
	}

	if (ok)
	{
		report_counts();
	}

	if (ok && cases_run + fixed_run != 0)
	{
		printf("SERIALIZE_MODEL_DRIVER_OK\n");
		return 0;
	}

	printf("SERIALIZE_MODEL_DRIVER_FAILED\n");
	return 1;
}
