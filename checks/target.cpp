#include "check.h"
#include "fixture.hpp"
#include "spirv_hlsl.hpp"
#include "spirv_msl.hpp"
#include <set>
#include <stdexcept>
#include <string>

#define REQUIRE(x)                                                                                          \
	do                                                                                                       \
	{                                                                                                        \
		if (!(x))                                                                                            \
			throw std::runtime_error(std::string("requirement failed: ") + #x);                             \
	} while (0)

namespace {

// Builds and flattens one fixture, returning the single combined mapping.
struct Built
{
	spirv_cross::CompilerGLSL *compiler = nullptr;
	spirv_cross::CombinedImageSampler mapping;
	spirv_cross::SmallVector<spirv_cross::CombinedImageSamplerBinding> bindings;
};

Built build_two_axis(unsigned n, unsigned m, bool image_array, bool sampler_array, bool helpers)
{
	auto f = pair_fixture::make(n, m, image_array, sampler_array, helpers);
	auto *compiler = new spirv_cross::CompilerGLSL(f.words);
	compiler->set_common_options(pair_fixture::options());
	compiler->build_combined_image_samplers();

	auto mappings = compiler->get_combined_image_samplers();
	REQUIRE(mappings.size() == 1);
	REQUIRE(mappings[0].image_id == f.images);
	REQUIRE(mappings[0].sampler_id == f.samplers);

	Built b;
	b.compiler = compiler;
	b.mapping = mappings[0];
	b.bindings = compiler->get_combined_image_sampler_bindings(mappings[0].combined_id);
	return b;
}

int pairing_cases()
{
	// 2x3: full enumeration and k = i*M + j ordering.
	{
		auto b = build_two_axis(2, 3, true, true, true);
		TASK_CHECK(b.bindings.size() == 6);
		for (uint32_t i = 0; i < 2; i++)
			for (uint32_t j = 0; j < 3; j++)
			{
				auto &slot = b.bindings[i * 3 + j];
				TASK_CHECK(slot.combined_index == i * 3 + j);
				TASK_CHECK(slot.image_index == i);
				TASK_CHECK(slot.sampler_index == j);
			}
		auto type = b.compiler->get_type_from_variable(b.mapping.combined_id);
		TASK_CHECK(type.array.size() == 1 && type.array[0] == 6);
		delete b.compiler;
	}

	// 3x2: reversed axis lengths, same flattening rule.
	{
		auto b = build_two_axis(3, 2, true, true, true);
		TASK_CHECK(b.bindings.size() == 6);
		for (uint32_t i = 0; i < 3; i++)
			for (uint32_t j = 0; j < 2; j++)
			{
				auto &slot = b.bindings[i * 2 + j];
				TASK_CHECK(slot.combined_index == i * 2 + j);
				TASK_CHECK(slot.image_index == i);
				TASK_CHECK(slot.sampler_index == j);
			}
		auto type = b.compiler->get_type_from_variable(b.mapping.combined_id);
		TASK_CHECK(type.array.size() == 1 && type.array[0] == 6);
		delete b.compiler;
	}

	// Length-1 sampler array: a declared array of one element still produces an
	// array combined resource and a complete, ordered slot table.
	{
		auto b = build_two_axis(2, 1, true, true, true);
		TASK_CHECK(b.bindings.size() == 2);
		TASK_CHECK(b.bindings[0].combined_index == 0 && b.bindings[0].image_index == 0 &&
		           b.bindings[0].sampler_index == 0);
		TASK_CHECK(b.bindings[1].combined_index == 1 && b.bindings[1].image_index == 1 &&
		           b.bindings[1].sampler_index == 0);
		auto type = b.compiler->get_type_from_variable(b.mapping.combined_id);
		TASK_CHECK(type.array.size() == 1 && type.array[0] == 2);
		delete b.compiler;
	}

	// Scalar image + sampler array: only the sampler axis varies.
	{
		auto b = build_two_axis(1, 3, false, true, true);
		TASK_CHECK(b.bindings.size() == 3);
		for (uint32_t j = 0; j < 3; j++)
		{
			TASK_CHECK(b.bindings[j].combined_index == j);
			TASK_CHECK(b.bindings[j].image_index == 0);
			TASK_CHECK(b.bindings[j].sampler_index == j);
		}
		delete b.compiler;
	}
	return 0;
}

// Confirms the emitted GLSL keeps both selector axes in the flattened subscript
// and threads the combined resource through helpers consistently.
int check_glsl_protocol(unsigned n, unsigned m, bool image_array, bool sampler_array, bool helpers)
{
	auto f = pair_fixture::make(n, m, image_array, sampler_array, helpers);
	spirv_cross::CompilerGLSL c(f.words);
	c.set_common_options(pair_fixture::options());
	c.build_combined_image_samplers();
	auto glsl = c.compile();

	auto mappings = c.get_combined_image_samplers();
	TASK_CHECK(mappings.size() == 1);
	auto combined_id = mappings[0].combined_id;
	auto name = c.get_name(combined_id);
	TASK_CHECK(!name.empty());

	// The global uniform must be an array of the flattened length when N*M > 1.
	auto type = c.get_type_from_variable(combined_id);
	unsigned total = n * m;
	if (total > 1)
		TASK_CHECK(type.array.size() == 1 && type.array[0] == total);

	// Both original axes must remain represented in the emitted code.
	if (image_array)
		TASK_CHECK(glsl.find("image_index") != std::string::npos);
	if (sampler_array)
		TASK_CHECK(glsl.find("sampler_index") != std::string::npos);

	// The flattened slot formula i * M + j must appear (only when both axes vary).
	if (image_array && sampler_array && m > 1)
	{
		std::string factor = std::to_string(m);
		TASK_CHECK(glsl.find("* " + factor) != std::string::npos ||
		           glsl.find("*" + factor) != std::string::npos);
	}
	return 0;
}

int calls_cases()
{
	// Full resource array passed down, then selected inside a non-leaf helper.
	TASK_CHECK(check_glsl_protocol(2, 3, true, true, true) == 0);
	// Already-selected scalar handles passed into a leaf helper.
	TASK_CHECK(check_glsl_protocol(2, 3, true, true, false) == 0);
	// A module that samples the same root pair from many call sites still creates
	// exactly one combined uniform for that root pair.
	{
		auto b = build_two_axis(2, 3, true, true, true);
		TASK_CHECK(b.compiler->get_combined_image_samplers().size() == 1);
		delete b.compiler;
	}
	// Two independently built modules both expose the same root identities even
	// though the generated combined id is module-local.
	{
		auto b1 = build_two_axis(2, 3, true, true, true);
		auto b2 = build_two_axis(3, 2, true, true, true);
		TASK_CHECK(b1.mapping.image_id == b2.mapping.image_id);
		TASK_CHECK(b1.mapping.sampler_id == b2.mapping.sampler_id);
		delete b1.compiler;
		delete b2.compiler;
	}
	return 0;
}

int compat_cases()
{
	// Scalar and image-array-only combinations keep their historical shape and
	// expose a valid binding table.
	for (unsigned mode = 0; mode < 2; mode++)
	{
		auto f = pair_fixture::make(mode ? 3u : 1u, 1, mode != 0, false);
		spirv_cross::CompilerGLSL c(f.words);
		c.set_common_options(pair_fixture::options());
		c.build_combined_image_samplers();
		auto mappings = c.get_combined_image_samplers();
		TASK_CHECK(mappings.size() == 1);
		auto id = mappings[0].combined_id;
		auto bindings = c.get_combined_image_sampler_bindings(id);
		auto type = c.get_type_from_variable(id);
		unsigned expected = mode ? 3u : 1u;
		TASK_CHECK(bindings.size() == expected);
		if (mode)
			TASK_CHECK(type.array.size() == 1 && type.array[0] == 3);
		else
			TASK_CHECK(type.array.empty());
		for (auto &b : bindings)
		{
			TASK_CHECK(b.sampler_index == 0);
			TASK_CHECK(b.combined_index == b.image_index);
		}
		// Name and binding remain assignable and reflected.
		c.set_name(id, "paired");
		c.set_decoration(id, spv::DecorationBinding, 7);
		bool reflected = false;
		for (auto &r : c.get_shader_resources().sampled_images)
			if (r.id == id && c.get_decoration(r.id, spv::DecorationBinding) == 7)
				reflected = true;
		TASK_CHECK(reflected);
	}

	// Repeated builds and queries are stable and leave reflection usable.
	{
		auto f = pair_fixture::make(2, 3, true, true);
		spirv_cross::CompilerGLSL c(f.words);
		c.set_common_options(pair_fixture::options());
		for (int iter = 0; iter < 3; iter++)
		{
			c.build_combined_image_samplers();
			auto id = c.get_combined_image_samplers()[0].combined_id;
			auto bindings = c.get_combined_image_sampler_bindings(id);
			TASK_CHECK(bindings.size() == 6);
		}
		TASK_CHECK(!c.compile().empty());
	}

	// Switching the active entry point and rebuilding still yields a complete table.
	{
		auto f = pair_fixture::make(2, 3, true, true);
		spirv_cross::CompilerGLSL c(f.words);
		c.set_common_options(pair_fixture::options());
		auto entries = c.get_entry_points_and_stages();
		TASK_CHECK(!entries.empty());
		c.set_entry_point(entries[0].name, entries[0].execution_model);
		c.build_combined_image_samplers();
		auto id = c.get_combined_image_samplers()[0].combined_id;
		TASK_CHECK(c.get_combined_image_sampler_bindings(id).size() == 6);
	}

	// Every unsupported target must throw rather than silently drop a resource.
	{
		auto f = pair_fixture::make(2, 3, true, true, false);
		auto expect_throw = [&](spirv_cross::Compiler &comp) -> int {
			bool threw = false;
			try
			{
				comp.build_combined_image_samplers();
				comp.compile();
			}
			catch (const spirv_cross::CompilerError &)
			{
				threw = true;
			}
			TASK_CHECK(threw);
			return 0;
		};

		spirv_cross::CompilerGLSL glsl420(f.words);
		{
			spirv_cross::CompilerGLSL::Options o;
			o.version = 420;
			glsl420.set_common_options(o);
		}
		TASK_CHECK(expect_throw(glsl420) == 0);

		spirv_cross::CompilerGLSL es(f.words);
		{
			spirv_cross::CompilerGLSL::Options o;
			o.es = true;
			o.version = 300;
			es.set_common_options(o);
		}
		TASK_CHECK(expect_throw(es) == 0);

		spirv_cross::CompilerGLSL vk(f.words);
		{
			spirv_cross::CompilerGLSL::Options o;
			o.vulkan_semantics = true;
			o.version = 450;
			vk.set_common_options(o);
		}
		TASK_CHECK(expect_throw(vk) == 0);

		spirv_cross::CompilerHLSL hlsl(f.words);
		TASK_CHECK(expect_throw(hlsl) == 0);

		spirv_cross::CompilerMSL msl(f.words);
		TASK_CHECK(expect_throw(msl) == 0);
	}

	// An unknown combined id goes through the standard exception channel.
	{
		auto f = pair_fixture::make(2, 3, true, true);
		spirv_cross::CompilerGLSL c(f.words);
		c.set_common_options(pair_fixture::options());
		c.build_combined_image_samplers();
		bool threw = false;
		try
		{
			(void)c.get_combined_image_sampler_bindings(0xDEAD);
		}
		catch (const spirv_cross::CompilerError &)
		{
			threw = true;
		}
		TASK_CHECK(threw);
	}
	return 0;
}

} // namespace

const struct task_case task_cases[] = {
	{ "pairing", "two-by-three", pairing_cases },
	{ "calls", "helper-and-scalar-protocol", calls_cases },
	{ "compat", "legacy-and-rejection", compat_cases },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
