/*
 * Structured serialization fuzz target for pugixml.
 *
 * Arbitrary libFuzzer bytes are mapped through a bounded, deterministic
 * generator into a model of an XML document.  The document is built with the
 * DOM API, saved with xml_writer for UTF-8/UTF-16LE/BE/UTF-32LE/BE, and the
 * fully concatenated output bytes are compared against an independent
 * scalar-by-scalar reference encoder.  See serialize_model_core.hpp for the
 * model and checking core shared with serialize_model_main.cpp.
 */

#include "serialize_model_core.hpp"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	serialize_model::Failure failure;

	if (!serialize_model::check_input(data, size, failure))
	{
		serialize_model::print_failure(failure, stderr);
		abort();
	}

	// The model must never silently degrade to an ASCII-only or empty case.
	// Random inputs additionally require the full four-class UTF-8 matrix; the
	// tiny fixed-family replay inputs are covered by the suite-level check.
	bool is_fixed = size >= 4 && data[0] == 'S' && data[1] == 'M' && data[2] == 'F' && data[3] < serialize_model::fixed_family_count();

	serialize_model::InvariantReport report;
	serialize_model::check_case_invariants(serialize_model::generate_case(data, size), report, !is_fixed);

	if (!report.ok)
	{
		fprintf(stderr, "MODEL_INVARIANT_FAILED: %s\n", report.detail.c_str());
		abort();
	}

	return 0;
}

#ifdef SERIALIZE_MODEL_STANDALONE_FUZZ_MAIN
// Fallback corpus runner used when the toolchain has no libFuzzer runtime.
// It calls the same exported entry point that a real fuzzer would.
#include <stdio.h>
#include <vector>

static bool read_file(const char* path, std::vector<uint8_t>& out)
{
	FILE* file = fopen(path, "rb");
	if (!file) return false;

	unsigned char buffer[4096];
	size_t got = 0;

	while ((got = fread(buffer, 1, sizeof(buffer), file)) != 0)
		out.insert(out.end(), buffer, buffer + got);

	fclose(file);
	return true;
}

int main(int argc, char** argv)
{
	size_t executed = 0;

	for (int i = 1; i < argc; ++i)
	{
		std::vector<uint8_t> bytes;

		if (!read_file(argv[i], bytes))
		{
			fprintf(stderr, "cannot read %s\n", argv[i]);
			return 2;
		}

		uint8_t empty = 0;
		LLVMFuzzerTestOneInput(bytes.empty() ? &empty : &bytes[0], bytes.size());
		++executed;
	}

	printf("fuzz_inputs=%zu\n", executed);

	return executed ? 0 : 1;
}
#endif

