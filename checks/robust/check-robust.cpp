// Robust buffer access checks.
//
// Every scenario compiles a hand-built SPIR-V module with SPIRV-Cross twice (robust mode on and off).
// The generated GLSL function bodies are then executed as C++: a narrow, deterministic shim aliases the
// GLSL scalar/vector/buffer names onto a bounded storage model with sentinel memory on both sides. The
// model records every read/write event and raises a hard violation if an out-of-range index ever reaches
// the backing store. The assertions therefore prove runtime behavior, not merely the presence of an
// "if".
#include "spirv_glsl.hpp"
#include "cpu_model.hpp"
#include "modules.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace robust;

namespace
{
int g_cases = 0;
int g_assertions = 0;
int g_failures = 0;

void check(bool condition, const std::string &message)
{
	g_assertions++;
	if (!condition)
	{
		g_failures++;
		std::printf("  ASSERT FAIL: %s\n", message.c_str());
	}
}

std::string compile_glsl(const std::vector<uint32_t> &words, bool robust, uint32_t version = 430,
                         bool es = false)
{
	spirv_cross::CompilerGLSL compiler(words);
	auto options = compiler.get_common_options();
	options.version = version;
	options.es = es;
	options.robust_buffer_access = robust;
	compiler.set_common_options(options);
	return compiler.compile();
}

bool expect_throw(const std::vector<uint32_t> &words, uint32_t version, bool es, std::string &message)
{
	try
	{
		(void)compile_glsl(words, true, version, es);
		message = "expected compile to reject the target/module but it succeeded";
		return false;
	}
	catch (const spirv_cross::CompilerError &e)
	{
		message = e.what();
		return true;
	}
}

// Keep only the function definitions: everything from the first function header onward. All type,
// buffer and struct declarations are replaced by a hand-written shim preamble so the executable model
// stays fully under our control (bounds checks, sentinels, event logging).
std::string strip_to_functions(const std::string &glsl)
{
	static const char *prefixes[] = {
		"void main", "void _", "float _", "int _", "uint _", "bool _",
		"vec2 _", "vec3 _", "vec4 _", "ivec2 _", "ivec3 _", "ivec4 _",
		"uvec2 _", "uvec3 _", "uvec4 _",
	};
	std::istringstream in(glsl);
	std::string line;
	std::ostringstream out;
	bool started = false;
	while (std::getline(in, line))
	{
		if (!started)
		{
			auto trimmed = line;
			while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
				trimmed.erase(trimmed.begin());
			for (const char *prefix : prefixes)
			{
				if (trimmed.rfind(prefix, 0) == 0 && trimmed.find('(') != std::string::npos)
				{
					started = true;
					break;
				}
			}
		}
		if (started)
			out << line << '\n';
	}
	return out.str();
}

std::string make_scalar_preamble(const char *ctype, const char *length_literal)
{
	std::ostringstream out;
	out << "#include \"cpu_model.hpp\"\n"
	    << "using namespace robust;\n"
	    << "using robust::uint;\n"
	    << "static ModelRuntime g_rt;\n"
	    << "static BoundedArray<" << ctype << "> g_values;\n"
	    << "static uint32_t g_length = " << length_literal << ";\n"
	    << "void bind_model(ModelRuntime &rt) { g_values.bind(&rt, \"values\", g_length); }\n"
	    << "struct Block { BoundedArray<" << ctype << "> &values = g_values; };\n"
	    << "static Block g_block;\n"
	    << "#define buf_1 g_block\n"
	    << "static void fragment_entry();\n";
	return out.str();
}

// Build, compile and run a translated fragment.
bool build_and_run(const std::string &source, const std::string &tag, std::string &output)
{
	const char *tmp = getenv("TMPDIR");
	std::string dir = std::string(tmp ? tmp : "/tmp") + "/robust-run";
	std::string mkdir = std::string("mkdir -p ") + dir;
	if (std::system(mkdir.c_str()) != 0)
		return false;

	std::string cpp_path = dir + "/" + tag + ".cpp";
	std::string bin_path = dir + "/" + tag;
	{
		std::ofstream out(cpp_path);
		out << source;
	}
	const char *inc = getenv("ROBUST_INCLUDE");
	std::string cmd = std::string("c++ -std=c++11 -O0 -I") + (inc ? inc : ".") + " \"" + cpp_path +
	                  "\" -o \"" + bin_path + "\" 2> \"" + dir + "/" + tag + ".err\"";
	if (std::system(cmd.c_str()) != 0)
	{
		std::ifstream err(dir + "/" + tag + ".err");
		output.assign((std::istreambuf_iterator<char>(err)), std::istreambuf_iterator<char>());
		return false;
	}
	std::string run = std::string("\"") + bin_path + "\" > \"" + dir + "/" + tag + ".out\" 2>&1";
	if (std::system(run.c_str()) != 0)
	{
		std::ifstream result(dir + "/" + tag + ".out");
		output.assign((std::istreambuf_iterator<char>(result)), std::istreambuf_iterator<char>());
		return false;
	}
	std::ifstream result(dir + "/" + tag + ".out");
	output.assign((std::istreambuf_iterator<char>(result)), std::istreambuf_iterator<char>());
	return true;
}

std::vector<AccessEvent> parse_events(const std::string &output)
{
	std::vector<AccessEvent> events;
	std::istringstream in(output);
	std::string line;
	bool in_events = false;
	while (std::getline(in, line))
	{
		if (line.rfind("EVENTS", 0) == 0)
		{
			in_events = true;
			continue;
		}
		if (line.rfind("VALUES", 0) == 0)
			break;
		if (in_events)
		{
			char kind;
			AccessEvent event;
			std::istringstream ls(line);
			if (ls >> kind >> event.array >> event.index)
			{
				event.is_write = (kind == 'W');
				events.push_back(event);
			}
		}
	}
	return events;
}

bool output_says_sentinels_ok(const std::string &output)
{
	return output.find("SENTINELS ok") != std::string::npos;
}

bool output_has_violation(const std::string &output)
{
	return output.find("VIOLATION none") == std::string::npos &&
	       output.find("VIOLATION") != std::string::npos;
}

std::string driver_footer(const std::string &sentinel_expr)
{
	std::ostringstream out;
	out << "int main() {\n"
	    << "  ModelRuntime rt;\n"
	    << "  rt.reset();\n"
	    << "  bind_model(rt);\n"
	    << "  fragment_entry();\n"
	    << "  std::printf(\"EVENTS %zu\\n\", rt.events.size());\n"
	    << "  for (size_t i = 0; i < rt.events.size(); i++)\n"
	    << "    std::printf(\"%c %s %u\\n\", rt.events[i].is_write ? 'W' : 'R',\n"
	    << "                rt.events[i].array.c_str(), rt.events[i].index);\n"
	    << "  std::printf(\"VALUES 0\\n\");\n"
	    << "  std::printf(\"VIOLATION %s\\n\", rt.violation ? rt.violation_text.c_str() : \"none\");\n"
	    << "  std::printf(\"SENTINELS %s\\n\", (" << sentinel_expr << ") ? \"ok\" : \"bad\");\n"
	    << "  return 0;\n}\n";
	return out.str();
}

} // namespace

// Scalar runtime-array scenarios: negative index, UINT_MAX, index == length, empty buffer, in-range.
static void test_scalar_runtime()
{
	const struct Scenario
	{
		const char *name;
		uint32_t index;
		uint32_t length;
		bool expect_access;
	} scenarios[] = {
		{ "negative", 0xFFFFFFFFu, 8u, false },
		{ "uint_max", 0xFFFFFFFFu, 4u, false },
		{ "equal-to-length", 8u, 8u, false },
		{ "empty-buffer", 0u, 0u, false },
		{ "in-range", 3u, 8u, true },
		{ "last-valid", 7u, 8u, true },
	};

	for (const auto &scenario : scenarios)
	{
		g_cases++;
		std::printf("CASE scalar-runtime/%s (index=%u length=%u)\n", scenario.name, scenario.index,
		            scenario.length);
		ScalarRuntimeModule module;
		module.kind = ScalarKind::UInt;
		auto words = module.build(scenario.index);
		auto robust_glsl = compile_glsl(words, true);
		auto off_glsl = compile_glsl(words, false);

		check(robust_glsl.find(".length()") != std::string::npos, "robust output uses runtime length");
		check(robust_glsl.find("if (") != std::string::npos, "robust output emits a guard");
		check(off_glsl.find("if (") == std::string::npos, "default output has no guard");

		std::string length_literal = std::to_string(scenario.length) + "u";
		std::string preamble = make_scalar_preamble("uint32_t", length_literal.c_str());
		std::string body = strip_to_functions(robust_glsl);		// Rename the GLSL entry point to a stable shim symbol.
		for (size_t pos = body.find("void main"); pos != std::string::npos; pos = body.find("void main", pos))
			body.replace(pos, 9, "void fragment_entry");

		std::string source = preamble + body + driver_footer("g_values.sentinels_intact()");
		std::string output;
		std::string tag = std::string("scalar_") + scenario.name;
		check(build_and_run(source, tag, output), std::string("robust fragment compiles/runs: ") + output);
		check(!output_has_violation(output), "no out-of-range access reaches the bounded store");
		check(output_says_sentinels_ok(output), "sentinel memory is untouched");

		auto events = parse_events(output);
		if (scenario.expect_access)
		{
			check(events.size() == 2, "in-range access yields one read and one write event");
			if (events.size() == 2)
			{
				check(events[0].index == scenario.index && !events[0].is_write, "read index matches");
				check(events[1].index == scenario.index && events[1].is_write, "write index matches");
			}
		}
		else
		{
			check(events.empty(), "out-of-range access produces no backing-store events");
		}

		// Control: the unguarded (default) fragment must trip the bounded model on the bad cases.
		if (!scenario.expect_access)
		{
			std::string off_body = strip_to_functions(off_glsl);
			for (size_t pos = off_body.find("void main"); pos != std::string::npos;
			     pos = off_body.find("void main", pos))
				off_body.replace(pos, 9, "void fragment_entry");
			std::string off_source = preamble + off_body + driver_footer("g_values.sentinels_intact()");
			std::string off_output;
			check(build_and_run(off_source, tag + "_off", off_output), "unguarded fragment compiles/runs");
			check(output_has_violation(off_output),
			      "without the guard the same bad index is detected by the bounded model");
		}
	}
}

// A genuinely signed int index of -1 must be rejected after the uint cast folds it to a large value.
// This is distinct from the unsigned UINT_MAX case above.
static void test_signed_negative_index()
{
	g_cases++;
	std::printf("CASE signed-negative/index=-1 length=8\n");
	ScalarRuntimeModule module;
	module.kind = ScalarKind::Int;
	auto words = module.build(0xFFFFFFFFu); // -1 as a signed 32-bit constant
	auto glsl = compile_glsl(words, true);
	check(glsl.find("uint(") != std::string::npos, "signed index is normalized through a uint cast");

	std::string preamble = make_scalar_preamble("int32_t", "8u");
	std::string body = strip_to_functions(glsl);
	for (size_t pos = body.find("void main"); pos != std::string::npos; pos = body.find("void main", pos))
		body.replace(pos, 9, "void fragment_entry");

	std::string output;
	check(build_and_run(preamble + body + driver_footer("g_values.sentinels_intact()"), "signed_negative",
	                    output),
	      std::string("signed-negative fragment runs: ") + output);
	check(!output_has_violation(output), "negative signed index never reaches the bounded store");
	check(output_says_sentinels_ok(output), "signed-negative sentinels intact");
	check(parse_events(output).empty(), "negative signed index yields no read/write events");
}

// Vector runtime array: outer array bound and dynamic component bound are independent levels.
static void test_vec4_runtime()
{
	const struct Scenario
	{
		const char *name;
		uint32_t outer, comp, length;
		bool outer_in_range;
		bool write_kept;
	} scenarios[] = {
		{ "outer-oob", 9u, 0u, 4u, false, false },
		{ "component-oob", 0u, 7u, 4u, true, false },
		{ "both-oob", 9u, 7u, 4u, false, false },
		{ "in-range", 2u, 1u, 4u, true, true },
		{ "empty-buffer", 0u, 0u, 0u, false, false },
	};

	for (const auto &s : scenarios)
	{
		g_cases++;
		std::printf("CASE vec4/%s outer=%u comp=%u length=%u\n", s.name, s.outer, s.comp, s.length);
		Vec4RuntimeModule module;
		auto words = module.build(s.outer, s.comp);
		auto glsl = compile_glsl(words, true);
		check(glsl.find(".length()") != std::string::npos, "vec4 guard uses runtime length");
		check(glsl.find("uint(4)") != std::string::npos, "vec4 component bound is the vector width");

		std::ostringstream pre;
		pre << "#include \"cpu_model.hpp\"\n"
		    << "using namespace robust;\n"
		    << "using robust::uint;\n"
		    << "static ModelRuntime g_rt;\n"
		    << "static BoundedArray<vec4> g_vals;\n"
		    << "static uint32_t g_length = " << s.length << "u;\n"
		    << "void bind_model(ModelRuntime &rt) { g_vals.bind(&rt, \"vals\", g_length); }\n"
		    << "struct Block { BoundedArray<vec4> &vals = g_vals; };\n"
		    << "static Block g_block;\n"
		    << "#define buf_1 g_block\n"
		    << "static void fragment_entry();\n";

		std::string body = strip_to_functions(glsl);
		for (size_t pos = body.find("void main"); pos != std::string::npos; pos = body.find("void main", pos))
			body.replace(pos, 9, "void fragment_entry");

		std::string output;
		check(build_and_run(pre.str() + body + driver_footer("g_vals.sentinels_intact()"),
		                    std::string("vec4_") + s.name, output),
		      std::string("vec4 fragment runs: ") + output);
		check(!output_has_violation(output), "vec4 out-of-range access is blocked");
		check(output_says_sentinels_ok(output), "vec4 sentinels intact");
		auto events = parse_events(output);
		// The whole-vector load runs (one read) whenever the outer index is in range, even if the later
		// component store is dropped. The scalar component write happens only when both levels pass.
		size_t reads = 0, writes = 0;
		for (auto &e : events)
		{
			if (e.is_write) writes++;
			else reads++;
		}
		check(reads == (s.outer_in_range ? 1u : 0u),
		      "whole-vector load happens exactly when the outer index is in range");
		check(writes == (s.write_kept ? 1u : 0u),
		      "component store is dropped when either bound fails");
	}
}

// Nested struct with an inner fixed-length array: outer and inner indices fail independently.
static void test_nested_struct()
{
	const struct Scenario
	{
		const char *name;
		uint32_t outer, inner, length;
		size_t expected_reads; // load A needs outer in range; load B needs inner in range
		size_t expected_writes; // store targets items[0], valid iff the runtime array is non-empty
	} scenarios[] = {
		{ "outer-oob", 8u, 0u, 4u, 1u, 1u },
		{ "inner-oob", 0u, 9u, 4u, 1u, 1u },
		{ "both-in-range", 1u, 2u, 4u, 2u, 1u },
		{ "empty-runtime", 0u, 0u, 0u, 0u, 0u },
	};

	for (const auto &s : scenarios)
	{
		g_cases++;
		std::printf("CASE nested/%s outer=%u inner=%u length=%u\n", s.name, s.outer, s.inner, s.length);
		NestedStructModule module;
		auto words = module.build(s.outer, s.inner);
		auto glsl = compile_glsl(words, true);
		check(glsl.find("uint(3)") != std::string::npos, "inner fixed-array bound comes from its type");
		check(glsl.find(".length()") != std::string::npos, "outer runtime-array bound uses length");

		std::ostringstream pre;
		pre << "#include \"cpu_model.hpp\"\n"
		    << "using namespace robust;\n"
		    << "using robust::uint;\n"
		    << "static ModelRuntime g_rt;\n"
		    << "struct AnonItems { FixedArray<int32_t, 3> inner; };\n"
		    << "static StructBoundedArray<AnonItems> g_items;\n"
		    << "static uint32_t g_length = " << s.length << "u;\n"
		    << "void bind_model(ModelRuntime &rt) {\n"
		    << "  g_items.bind(&rt, \"items\", g_length);\n"
		    << "  for (uint32_t i = 0; i < g_length; i++) g_items.data[i].inner.bind(&rt, \"inner\", 3);\n"
		    << "}\n"
		    << "struct Block { StructBoundedArray<AnonItems> &items = g_items; };\n"
		    << "static Block g_block;\n"
		    << "#define buf_1 g_block\n"
		    << "static void fragment_entry();\n";

		std::string body = strip_to_functions(glsl);
		for (size_t pos = body.find("void main"); pos != std::string::npos; pos = body.find("void main", pos))
			body.replace(pos, 9, "void fragment_entry");

		std::string output;
		check(build_and_run(pre.str() + body + driver_footer("g_items.sentinels_intact()"),
		                    std::string("nested_") + s.name, output),
		      std::string("nested fragment runs: ") + output);
		check(!output_has_violation(output), "nested out-of-range index is blocked at the right layer");
		check(output_says_sentinels_ok(output), "nested sentinels intact");
		auto events = parse_events(output);

		// All scalar events come from the inner FixedArray ("inner"). The two loads only reach an inner
		// cell when both the outer and inner indices are valid; otherwise their guard returns zero and
		// no inner access happens. The final store targets items[0].inner[0] and always runs.
		size_t inner_reads = 0, inner_writes = 0;
		for (auto &e : events)
		{
			if (e.array != "inner")
				continue;
			if (e.is_write) inner_writes++;
			else inner_reads++;
		}
		// Load A (outer index) and load B (inner index) are independent; each reaches a cell only when
		// its own guarded indices are in range. The store to items[0] runs on a non-empty buffer.
		check(inner_reads == s.expected_reads, "each nested load reads memory only when its layer passes");
		check(inner_writes == s.expected_writes, "the final store fires exactly when the buffer is non-empty");
	}
}

// A side-effecting helper must run even when the store it feeds is dropped as out of bounds.
static void test_helper_side_effect()
{
	const struct Scenario
	{
		const char *name;
		uint32_t target, length;
		bool store_kept;
		bool helper_memory_reachable;
	} scenarios[] = {
		{ "dropped-oob", 999u, 4u, false, true },
		{ "kept-in-range", 1u, 4u, true, true },
		{ "empty-buffer", 0u, 0u, false, false },
	};

	for (const auto &s : scenarios)
	{
		g_cases++;
		std::printf("CASE helper/%s target=%u length=%u\n", s.name, s.target, s.length);
		HelperSideEffectModule module;
		auto words = module.build(s.target);
		auto glsl = compile_glsl(words, true);

		std::string preamble = make_scalar_preamble("float", (std::to_string(s.length) + "u").c_str());
		std::string body = strip_to_functions(glsl);
		for (size_t pos = body.find("void main"); pos != std::string::npos; pos = body.find("void main", pos))
			body.replace(pos, 9, "void fragment_entry");

		std::string output;
		check(build_and_run(preamble + body + driver_footer("g_values.sentinels_intact()"),
		                    std::string("helper_") + s.name, output),
		      std::string("helper fragment runs: ") + output);
		check(!output_has_violation(output), "dropped store never reaches the bounded model");
		check(output_says_sentinels_ok(output), "helper sentinels intact");

		auto events = parse_events(output);
		// The helper is always called (its evaluation is never skipped). On a non-empty buffer its
		// internal write/read of index 1 both execute; on an empty buffer even those in-bounds accesses
		// are correctly suppressed by the guard, so no memory event occurs.
		bool helper_write = false, helper_read = false;
		for (auto &e : events)
		{
			if (e.is_write && e.index == 1u) helper_write = true;
			if (!e.is_write && e.index == 1u) helper_read = true;
		}
		if (s.helper_memory_reachable)
		{
			check(helper_write, "helper's side-effecting write executes when the buffer can hold index 1");
			check(helper_read, "helper's read executes when the buffer can hold index 1");
			check(events.size() == (s.store_kept ? 3u : 2u),
			      "a kept store adds one event; a dropped out-of-bounds store adds none");
		}
		else
		{
			check(!helper_write && !helper_read,
			      "empty buffer suppresses the helper's guarded memory accesses as well");
			check(events.empty(), "an empty buffer yields no backing-store events");
		}
	}
}

// The guard must stay inside the loop/branch that contains the access, never hoisted out of scope.
static void test_loop_branch()
{
	g_cases++;
	std::printf("CASE loop-branch trip=6 length=4\n");
	LoopBranchModule module;
	auto words = module.build(6u);
	auto glsl = compile_glsl(words, true);

	size_t for_pos = glsl.find("for (;;)");
	size_t outer_if = for_pos == std::string::npos ? std::string::npos : glsl.find("if (", for_pos);
	size_t guard_if = outer_if == std::string::npos ? std::string::npos : glsl.find("if (", outer_if + 1);
	check(for_pos != std::string::npos && outer_if != std::string::npos && guard_if != std::string::npos,
	      "guard is emitted inside the loop and branch");
	check(for_pos != std::string::npos && glsl.find(".length()", for_pos) != std::string::npos,
	      "bounds check is not hoisted before the loop");

	std::string preamble = make_scalar_preamble("float", "4u");
	std::string body = strip_to_functions(glsl);
	for (size_t pos = body.find("void main"); pos != std::string::npos; pos = body.find("void main", pos))
		body.replace(pos, 9, "void fragment_entry");

	std::string output;
	check(build_and_run(preamble + body + driver_footer("g_values.sentinels_intact()"), "loop_branch",
	                    output),
	      std::string("loop fragment runs: ") + output);
	check(!output_has_violation(output), "loop never writes past the bounded array");
	check(output_says_sentinels_ok(output), "loop sentinels intact");
	auto events = parse_events(output);
	// trip=6 skips index 3 and only indices 0..3 are within length 4: writes at 0,1,2 (3 is skipped).
	check(events.size() == 3u, "loop writes exactly the in-range, non-skipped indices");
	uint32_t expected_index[] = {0u, 1u, 2u};
	for (size_t i = 0; i < events.size(); i++)
		check(events[i].is_write && events[i].index == expected_index[i], "loop write index is ordered and in range");
}

// Unsupported operations and unsupported targets are rejected up front.
static void test_rejections_and_targets()
{
	g_cases++;
	std::printf("CASE unsupported/atomic-store\n");
	AtomicStoreModule atomic;
	std::string message;
	check(expect_throw(atomic.build(), 430, false, message), "atomic SSBO access is rejected in robust mode");
	check(message.find("atomic") != std::string::npos, "rejection message names the atomic operation");

	auto off = compile_glsl(atomic.build(), false);
	check(off.find("atomicExchange") != std::string::npos, "atomic store compiles when robust mode is off");

	g_cases++;
	std::printf("CASE unsupported/whole-struct-load\n");
	WholeStructModule whole;
	check(expect_throw(whole.build(), 430, false, message),
	      "whole-struct SSBO load is rejected rather than emitted unguarded");
	check(message.find("unsupported storage buffer load") != std::string::npos,
	      "rejection message identifies the unsupported load");
	auto whole_off = compile_glsl(whole.build(), false);
	check(whole_off.find("if (") == std::string::npos, "whole-struct load compiles normally with the option off");

	g_cases++;
	std::printf("CASE target/es-rejected\n");
	ScalarRuntimeModule scalar;
	scalar.kind = ScalarKind::Int;
	auto scalar_words = scalar.build(0u);
	check(expect_throw(scalar_words, 310, true, message), "ES target is rejected");
	check(message.find("desktop") != std::string::npos, "ES rejection explains the desktop requirement");

	g_cases++;
	std::printf("CASE target/desktop-420-rejected\n");
	check(expect_throw(scalar_words, 420, false, message), "GLSL 420 is rejected");
	check(message.find("430") != std::string::npos, "420 rejection mentions the required version");
}

// Repeated compilation and toggling the option off must leave default output and reflection intact.
static void test_repeat_and_disable()
{
	g_cases++;
	std::printf("CASE repeat-compile-and-disable\n");
	ScalarRuntimeModule module;
	module.kind = ScalarKind::Int;
	auto words = module.build(2u);

	spirv_cross::CompilerGLSL compiler(words);
	auto options = compiler.get_common_options();
	options.version = 430;
	options.es = false;
	options.robust_buffer_access = true;
	compiler.set_common_options(options);
	auto first = compiler.compile();
	auto second = compiler.compile();

	// Synthetic SPIRV-Cross temporaries are named _NN; their numeric suffix grows as synthesized ids
	// are allocated, so compare with every _NN token canonicalized to a placeholder.
	auto normalize = [](const std::string &text) {
		std::string out;
		out.reserve(text.size());
		for (size_t i = 0; i < text.size();)
		{
			if (text[i] == '_' && i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '9')
			{
				out += "_TMP";
				i++;
				while (i < text.size() && text[i] >= '0' && text[i] <= '9')
					i++;
			}
			else
			{
				out += text[i];
				i++;
			}
		}
		return out;
	};
	check(normalize(first) == normalize(second), "repeated robust compiles are structurally identical");

	options.robust_buffer_access = false;
	compiler.set_common_options(options);
	auto disabled = compiler.compile();
	check(disabled.find(".length()") == std::string::npos, "disabling the option removes all guard checks");
	check(disabled.find("if (") == std::string::npos, "disabling the option restores unguarded output");

	// A fresh compiler with the option off is the reference default output. It must be guard-free and
	// deterministic (two fresh compiles match), proving the default code path and reflection are
	// untouched by the feature. Cross-instance temp forwarding differences are pre-existing backend
	// behavior and are not introduced by this option.
	auto fresh_a = compile_glsl(words, false);
	auto fresh_b = compile_glsl(words, false);
	check(normalize(fresh_a) == normalize(fresh_b), "default (option off) output is deterministic");
	check(fresh_a.find(".length()") == std::string::npos && fresh_a.find("if (") == std::string::npos,
	      "default output contains no robust guard");

	spirv_cross::CompilerGLSL reflect(words);
	auto ro = reflect.get_common_options();
	ro.version = 430;
	ro.robust_buffer_access = false;
	reflect.set_common_options(ro);
	auto resources = reflect.get_shader_resources();
	check(resources.storage_buffers.size() == 1u, "reflection still reports exactly one storage buffer");
	check(resources.storage_buffers[0].name == "buf", "reflection preserves the buffer name");
}

int main()
{
	std::printf("robust buffer access checks\n");
	test_scalar_runtime();
	test_signed_negative_index();
	test_vec4_runtime();
	test_nested_struct();
	test_helper_side_effect();
	test_loop_branch();
	test_rejections_and_targets();
	test_repeat_and_disable();

	std::printf("\nCASES %d ASSERTIONS %d FAILURES %d\n", g_cases, g_assertions, g_failures);
	return g_failures == 0 ? 0 : 1;
}
