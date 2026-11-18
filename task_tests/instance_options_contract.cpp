// Regression contract for per-instance jdis options.
//
// The test drives the production org.openjdk.asmtools.jdis.Main through a Java probe
// (task_tests/InstanceOptionsProbe.java).  Fixture class files are only ever read as raw
// bytes by the probe; no analysed class is loaded or executed by the JVM.
//
// Every case compares real disassembly bodies produced by the tool: a body produced with
// several live instances must be byte-for-byte identical to the body produced by an
// isolated single-instance run with the same arguments.

#include "check.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

const char *const DETAILED = "-g,-table,-lvt,-lnt:numbers";

struct Section {
    std::map<std::string, std::string> bodies;
    std::vector<std::string> order;
};

// ---------------------------------------------------------------- environment

std::string javaExe() {
    const char *explicitJava = std::getenv("TASK_JAVA");
    if (explicitJava && *explicitJava) {
        return explicitJava;
    }
    const char *home = std::getenv("JAVA_HOME");
    if (home && *home) {
        std::string candidate = std::string(home) + "/bin/java";
        if (std::FILE *f = std::fopen(candidate.c_str(), "r")) {
            std::fclose(f);
            return candidate;
        }
    }
    // Resolve javac from PATH; the matching java normally lives in the same bin dir.
    if (std::FILE *f = popen("command -v javac 2>/dev/null", "r")) {
        char buf[4096];
        std::string javac;
        while (std::fgets(buf, sizeof(buf), f)) {
            javac += buf;
        }
        pclose(f);
        while (!javac.empty() && (javac.back() == '\n' || javac.back() == '\r')) {
            javac.pop_back();
        }
        if (!javac.empty()) {
            std::string dir = javac.substr(0, javac.find_last_of('/'));
            std::string candidate = dir + "/java";
            if (std::FILE *j = std::fopen(candidate.c_str(), "r")) {
                std::fclose(j);
                return candidate;
            }
        }
    }
    return "java";
}

std::string classPath() {
    const char *cp = std::getenv("TASK_CLASSES");
    if (cp && *cp) {
        return cp;
    }
    return ".task-build/classes";
}

std::string fixture(const std::string &name) {
    const char *dir = std::getenv("TASK_FIXTURES");
    std::string root = (dir && *dir) ? dir : ".task-build/fixture";
    return root + "/" + name + ".class";
}

// Runs the probe and parses "@@BEGIN name ... @@END name" sections.
bool runProbe(const std::string &command, const std::string &fixtureName,
              const std::string &extra, Section &section) {
    std::string cmd = "\"" + javaExe() + "\" -cp \"" + classPath() +
                      "\" InstanceOptionsProbe " + command + " \"" + fixture(fixtureName) + "\"";
    if (!extra.empty()) {
        cmd += " \"" + extra + "\"";
    }
    cmd += " 2>&1";

    std::FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return false;
    }
    std::string output;
    char buf[8192];
    while (std::fgets(buf, sizeof(buf), pipe)) {
        output += buf;
    }
    int status = pclose(pipe);
    if (status != 0) {
        std::fprintf(stderr, "probe failed (%d): %s\n%s\n", status, cmd.c_str(), output.c_str());
        return false;
    }

    std::string current;
    std::string body;
    size_t pos = 0;
    while (pos <= output.size()) {
        size_t nl = output.find('\n', pos);
        std::string line = (nl == std::string::npos) ? output.substr(pos) : output.substr(pos, nl - pos);
        if (line.rfind("@@BEGIN ", 0) == 0) {
            current = line.substr(8);
            body.clear();
        } else if (line.rfind("@@END ", 0) == 0) {
            std::string name = line.substr(6);
            if (name != current) {
                std::fprintf(stderr, "mismatched section %s/%s\n", current.c_str(), name.c_str());
                return false;
            }
            section.bodies[current] = body;
            section.order.push_back(current);
            current.clear();
            body.clear();
        } else if (!current.empty()) {
            body += line;
            if (nl != std::string::npos) {
                body += '\n';
            }
        }
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }
    return !section.bodies.empty();
}

const std::string &body(const Section &s, const char *name) {
    static const std::string empty;
    auto it = s.bodies.find(name);
    if (it == s.bodies.end()) {
        std::fprintf(stderr, "missing section %s\n", name);
        return empty;
    }
    return it->second;
}

// The probe emits both the bodies produced by the live instances and the bodies
// produced by fresh isolated instances inside the same JVM ("REF*").  Comparisons
// against "REF*" therefore never depend on anything but the disassembler state.

bool sameText(const std::string &a, const std::string &b) {
    return !a.empty() && a == b;
}

bool differs(const std::string &a, const std::string &b) {
    return !a.empty() && !b.empty() && a != b;
}

bool contains(const std::string &text, const char *needle) {
    return text.find(needle) != std::string::npos;
}

size_t count(const std::string &text, const char *needle) {
    size_t n = 0, pos = 0;
    const size_t len = std::strlen(needle);
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += len;
    }
    return n;
}

bool nonEmpty(const std::string &text) {
    return !text.empty();
}

// ---------------------------------------------------------------------- cases

// Ownership: construction order, alternation, repeated argument lists, parallelism.

int case_default_created_first() {
    Section s;
    TASK_CHECK(runProbe("default-then-detailed", "LocalFixture", "", s));
    TASK_CHECK(nonEmpty(body(s, "REF_A")));
    TASK_CHECK(nonEmpty(body(s, "REF_B")));
    TASK_CHECK(sameText(body(s, "A"), body(s, "REF_A")));
    TASK_CHECK(sameText(body(s, "B"), body(s, "REF_B")));
    TASK_CHECK(differs(body(s, "REF_A"), body(s, "REF_B")));
    return 0;
}

int case_detailed_created_first() {
    Section s;
    TASK_CHECK(runProbe("detailed-then-default", "LocalFixture", "", s));
    TASK_CHECK(sameText(body(s, "A"), body(s, "REF_A")));
    TASK_CHECK(sameText(body(s, "B"), body(s, "REF_B")));
    return 0;
}

int case_interleaved_instances() {
    Section s;
    TASK_CHECK(runProbe("interleaved", "LocalFixture", "", s));
    TASK_CHECK(sameText(body(s, "A1"), body(s, "REF_A")));
    TASK_CHECK(sameText(body(s, "A2"), body(s, "REF_A")));
    TASK_CHECK(sameText(body(s, "B1"), body(s, "REF_B")));
    TASK_CHECK(sameText(body(s, "B2"), body(s, "REF_B")));
    TASK_CHECK(sameText(body(s, "A1"), body(s, "A2")));
    TASK_CHECK(sameText(body(s, "B1"), body(s, "B2")));
    return 0;
}

int case_same_instance_reargs() {
    Section s;
    TASK_CHECK(runProbe("reargs", "LocalFixture", "", s));
    TASK_CHECK(sameText(body(s, "R1"), body(s, "REF_A")));
    TASK_CHECK(sameText(body(s, "R2"), body(s, "REF_A")));
    TASK_CHECK(sameText(body(s, "D"), body(s, "REF_D")));
    // A later call without arguments keeps the last per-call configuration of this
    // instance (the historical disasm()/disasm(String...) behaviour).
    TASK_CHECK(sameText(body(s, "R3"), body(s, "REF_D")));
    return 0;
}

int case_parallel_instances() {
    Section s;
    TASK_CHECK(runProbe("parallel", "LocalFixture", "", s));
    TASK_CHECK(sameText(body(s, "A"), body(s, "REF_A")));
    TASK_CHECK(sameText(body(s, "B"), body(s, "REF_B")));
    return 0;
}

int case_default_mode_body() {
    Section s;
    TASK_CHECK(runProbe("body", "LocalFixture", "", s));
    const std::string &refDefault = body(s, "B");
    // The default mode keeps label identifiers and prints no constant pool or tables.
    TASK_CHECK(contains(refDefault, "L"));
    TASK_CHECK(!contains(refDefault, "const #"));
    TASK_CHECK(!contains(refDefault, "LocalVariables:"));
    return 0;
}

int case_detailed_mode_body() {
    Section s;
    TASK_CHECK(runProbe("body", "LocalFixture", DETAILED, s));
    const std::string &refDetailed = body(s, "B");
    TASK_CHECK(contains(refDetailed, "const #"));
    TASK_CHECK(contains(refDetailed, "LocalVariables:"));
    TASK_CHECK(contains(refDetailed, "LocalVariableTypes:"));
    TASK_CHECK(contains(refDetailed, "LineNumberTable"));
    return 0;
}

// Debug: local variable scopes, exceptions and line numbers per invocation.

int case_debug_tables_stable_across_runs() {
    Section s;
    TASK_CHECK(runProbe("repeat", "GenericFixture", DETAILED, s));
    const std::string &ref = body(s, "REF");
    TASK_CHECK(contains(ref, "LocalVariables:"));
    TASK_CHECK(contains(ref, "LocalVariableTypes:"));
    TASK_CHECK(contains(ref, "number_of_entries"));
    TASK_CHECK(sameText(body(s, "FIRST"), ref));
    TASK_CHECK(sameText(body(s, "SECOND"), ref));
    TASK_CHECK(sameText(body(s, "THIRD"), ref));
    return 0;
}

int case_debug_events_do_not_accumulate() {
    Section s;
    TASK_CHECK(runProbe("repeat", "ExceptionFixture", DETAILED, s));
    const std::string &ref = body(s, "REF");
    const std::string &first = body(s, "FIRST");
    const std::string &second = body(s, "SECOND");
    const std::string &third = body(s, "THIRD");
    TASK_CHECK(sameText(first, second));
    TASK_CHECK(sameText(second, third));
    TASK_CHECK(sameText(first, ref));

    // Local variable begin/end events, exception ranges and line comments are produced
    // exactly as often as in the isolated run - repeated execution adds nothing.
    TASK_CHECK(count(second, "LocalVariables:") == count(ref, "LocalVariables:"));
    TASK_CHECK(count(second, "try ") == count(ref, "try "));
    TASK_CHECK(count(second, "catch ") == count(ref, "catch "));
    TASK_CHECK(count(second, "// ") == count(ref, "// "));
    TASK_CHECK(nonEmpty(second));
    return 0;
}

int case_generic_locals_and_lines() {
    Section s;
    TASK_CHECK(runProbe("default-then-detailed", "GenericFixture", "", s));
    const std::string &refDefault = body(s, "REF_A");
    const std::string &refDetailed = body(s, "REF_B");

    // Generic local variable types come from the instance's own options.
    TASK_CHECK(contains(refDetailed, "LocalVariableTypes:"));
    TASK_CHECK(contains(refDetailed, "\"value\":\"TT;\""));
    TASK_CHECK(contains(refDetailed, "\"result\":\"Ljava/util/List<TT;>;\""));
    // In the default mode the same fixture prints no debug tables and keeps labels.
    TASK_CHECK(!contains(refDefault, "LocalVariables:"));
    TASK_CHECK(!contains(refDefault, "LocalVariableTypes:"));
    TASK_CHECK(contains(refDefault, "stack_frame_type"));

    TASK_CHECK(sameText(body(s, "A"), refDefault));
    TASK_CHECK(sameText(body(s, "B"), refDetailed));
    return 0;
}

// Format: constant pool, CP indexes, PC/labels and table alignment per instance.

int case_cp_indexes_and_pc() {
    Section s;
    TASK_CHECK(runProbe("format-pair", "LocalFixture", std::string(DETAILED) + ";" + DETAILED, s));
    const std::string &ref = body(s, "REF_A");
    TASK_CHECK(contains(ref, "const #1 "));
    TASK_CHECK(contains(ref, " #"));
    // Detailed output prints program counters for every instruction.
    TASK_CHECK(contains(ref, "  0:"));
    TASK_CHECK(sameText(body(s, "A"), ref));
    TASK_CHECK(sameText(body(s, "B"), body(s, "REF_B")));
    TASK_CHECK(sameText(body(s, "REF_A"), body(s, "REF_B")));
    return 0;
}

int case_default_and_detailed_pair() {
    Section s;
    TASK_CHECK(runProbe("format-pair", "LocalFixture", ";" + std::string(DETAILED), s));
    TASK_CHECK(sameText(body(s, "A"), body(s, "REF_A")));
    TASK_CHECK(sameText(body(s, "B"), body(s, "REF_B")));
    TASK_CHECK(differs(body(s, "REF_A"), body(s, "REF_B")));

    Section reversed;
    TASK_CHECK(runProbe("format-pair", "LocalFixture", std::string(DETAILED) + ";", reversed));
    TASK_CHECK(sameText(body(reversed, "A"), body(reversed, "REF_A")));
    TASK_CHECK(sameText(body(reversed, "B"), body(reversed, "REF_B")));
    return 0;
}

int case_table_alignment_per_instance() {
    Section s;
    TASK_CHECK(runProbe("format-pair", "ExceptionFixture",
                        std::string("-g,-table,-lnt:numbers") + ";" + "-g,-lnt:numbers", s));
    const std::string &refTable = body(s, "REF_A");
    const std::string &refPlain = body(s, "REF_B");
    TASK_CHECK(sameText(body(s, "A"), refTable));
    TASK_CHECK(sameText(body(s, "B"), refPlain));
    TASK_CHECK(differs(refTable, refPlain));
    TASK_CHECK(contains(refTable, "const #1 "));
    return 0;
}

int case_legacy_static_surface() {
    Section s;
    TASK_CHECK(runProbe("legacy-static", "LocalFixture", "", s));
    // A configured instance prints its own options and leaves the legacy set alone.
    TASK_CHECK(sameText(body(s, "A"), body(s, "REF_A")));
    TASK_CHECK(!body(s, "GLOBAL_BEFORE").empty());
    TASK_CHECK(sameText(body(s, "GLOBAL_BEFORE"), body(s, "GLOBAL_AFTER")));
    TASK_CHECK(sameText(body(s, "GLOBAL_CONTAINS_CP"), std::string("false\n")));
    // The no-argument constructor form still works as before.
    TASK_CHECK(sameText(body(s, "D"), body(s, "REF_D")));
    // And it keeps following the legacy static configuration.
    TASK_CHECK(sameText(body(s, "D2"), body(s, "REF_D2")));
    TASK_CHECK(differs(body(s, "D"), body(s, "D2")));
    return 0;
}

int case_format_interleaved_instances() {
    const std::string tableArgs = "-g,-table,-lvt,-lnt:numbers";
    const std::string plainArgs = "-g,-lvt,-lnt:numbers";

    Section s;
    TASK_CHECK(runProbe("format-pair", "GenericFixture", tableArgs + ";" + plainArgs, s));
    TASK_CHECK(sameText(body(s, "A"), body(s, "REF_A")));
    TASK_CHECK(sameText(body(s, "B"), body(s, "REF_B")));
    TASK_CHECK(differs(body(s, "REF_A"), body(s, "REF_B")));

    Section reversed;
    TASK_CHECK(runProbe("format-pair", "GenericFixture", plainArgs + ";" + tableArgs, reversed));
    TASK_CHECK(sameText(body(reversed, "A"), body(reversed, "REF_A")));
    TASK_CHECK(sameText(body(reversed, "B"), body(reversed, "REF_B")));
    return 0;
}

} // namespace

extern const struct task_case task_cases[];
extern const size_t task_case_count;

const struct task_case task_cases[] = {
    {"ownership", "default_created_first", case_default_created_first},
    {"ownership", "detailed_created_first", case_detailed_created_first},
    {"ownership", "interleaved_instances", case_interleaved_instances},
    {"ownership", "same_instance_reargs", case_same_instance_reargs},
    {"ownership", "parallel_instances", case_parallel_instances},
    {"debug", "default_mode_body", case_default_mode_body},
    {"debug", "detailed_mode_body", case_detailed_mode_body},
    {"debug", "debug_tables_stable_across_runs", case_debug_tables_stable_across_runs},
    {"debug", "debug_events_do_not_accumulate", case_debug_events_do_not_accumulate},
    {"debug", "generic_locals_and_lines", case_generic_locals_and_lines},
    {"format", "cp_indexes_and_pc", case_cp_indexes_and_pc},
    {"format", "default_and_detailed_pair", case_default_and_detailed_pair},
    {"format", "table_alignment_per_instance", case_table_alignment_per_instance},
    {"format", "format_interleaved_instances", case_format_interleaved_instances},
    {"format", "legacy_static_surface", case_legacy_static_surface},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
