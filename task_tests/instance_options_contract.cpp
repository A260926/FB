// Regression contract for jdis per-instance printing options.
//
// The Java probe (task_tests/InstanceOptionsProbe.java) calls the production
// org.openjdk.asmtools.jdis.Main with real class files read as bytes.  Each
// scenario returns the actual disassembly text; this test compares those
// bodies with independently produced single-run bodies and checks that debug
// information is neither leaked between instances nor duplicated.
#include "task_support/check.h"

#include <map>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace {

const char *kProbe = "java -cp .task-build/classes InstanceOptionsProbe";
const char *kLocal = ".task-build/fixture/LocalFixture.class";
const char *kDebug = ".task-build/fixture/DebugFixture.class";

using Fields = std::map<std::string, std::string>;

// Runs the probe and captures its KEY=value output lines.
bool run_probe(const std::vector<std::string> &args, Fields &out) {
    std::string cmd = kProbe;
    for (const std::string &a : args) {
        cmd += " ";
        cmd += a;
    }
    FILE *p = popen(cmd.c_str(), "r");
    if (!p) {
        return false;
    }
    char *buf = nullptr;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&buf, &cap, p)) >= 0) {
        std::string line(buf, (size_t) len);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        size_t eq = line.find('=');
        if (eq != std::string::npos) {
            out[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    free(buf);
    int rc = pclose(p);
    return rc == 0;
}

std::string field(const Fields &f, const std::string &key) {
    auto it = f.find(key);
    return it == f.end() ? std::string() : it->second;
}

int number(const Fields &f, const std::string &key) {
    return std::atoi(field(f, key).c_str());
}

// Emulates the probe's escaping so that bodies can be concatenated.
std::vector<std::string> split_escaped(const std::string &s) {
    std::vector<std::string> parts;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[i + 1];
            if (n == 'n' || n == 'r' || n == 't' || n == '\\') {
                cur += '\\';
                cur += n;
                ++i;
                continue;
            }
        }
        cur += s[i];
    }
    parts.push_back(cur);
    return parts;
}

bool has(const std::string &body, const std::string &needle) {
    return body.find(needle) != std::string::npos;
}

// The reference body produced by a fresh, independently configured instance.
std::string reference(const std::string &cls, const std::vector<std::string> &opts) {
    std::vector<std::string> args = {"once", cls};
    args.insert(args.end(), opts.begin(), opts.end());
    Fields f;
    if (!run_probe(args, f)) {
        return std::string();
    }
    return field(f, "OUT");
}

} // namespace

static int case_ownership_default_first() {
    Fields f;
    TASK_CHECK(run_probe({"ownership", kLocal}, f));
    std::string refDefault = reference(kLocal, {});
    std::string refDetailed = reference(kLocal, {"-g", "-table", "-lvt", "-lnt:numbers"});
    TASK_CHECK(!refDefault.empty());
    TASK_CHECK(!refDetailed.empty());
    // A was built first with no options, B second with detailed options.
    TASK_CHECK(field(f, "A") == refDefault);
    TASK_CHECK(field(f, "B") == refDetailed);
    TASK_CHECK(field(f, "A") != field(f, "B"));
    return 0;
}

static int case_ownership_detailed_first() {
    Fields f;
    TASK_CHECK(run_probe({"reverse", kLocal}, f));
    TASK_CHECK(field(f, "A") == reference(kLocal, {"-g", "-table", "-lvt", "-lnt:numbers"}));
    TASK_CHECK(field(f, "B") == reference(kLocal, {}));
    return 0;
}

static int case_ownership_reargue() {
    Fields f;
    TASK_CHECK(run_probe({"reargue", kLocal}, f));
    TASK_CHECK(field(f, "G") == reference(kLocal, {"-g"}));
    TASK_CHECK(field(f, "OTHER") == reference(kLocal, {"-lvt", "-lnt:numbers"}));
    TASK_CHECK(field(f, "G_AFTER_OTHER") == reference(kLocal, {"-g"}));
    TASK_CHECK(field(f, "TABLE") == reference(kLocal, {"-table"}));
    TASK_CHECK(field(f, "DETAILED") == reference(kLocal, {"-g", "-table", "-lvt", "-lnt:numbers"}));
    return 0;
}

static int case_ownership_interleaved() {
    Fields f;
    TASK_CHECK(run_probe({"interleave", kLocal}, f));
    std::string defaultRef = reference(kLocal, {});
    std::string detailedRef = reference(kLocal, {"-g", "-table", "-lvt", "-lnt:numbers"});
    TASK_CHECK(field(f, "A_ONCE") == defaultRef);
    TASK_CHECK(field(f, "B_ONCE") == detailedRef);
    // Two alternated runs of one instance are exactly two copies of one run.
    TASK_CHECK(field(f, "A_TWICE") == defaultRef + defaultRef);
    TASK_CHECK(field(f, "B_TWICE") == detailedRef + detailedRef);
    return 0;
}

static int case_ownership_parallel() {
    Fields f;
    TASK_CHECK(run_probe({"parallel", kLocal}, f));
    TASK_CHECK(field(f, "A") == reference(kLocal, {}));
    TASK_CHECK(field(f, "B") == reference(kLocal, {"-g", "-table", "-lvt", "-lnt:numbers"}));
    return 0;
}

static int case_debug_repeat_stable() {
    Fields f;
    TASK_CHECK(run_probe({"repeat", kDebug}, f));
    std::string refDetailed = reference(kDebug, {"-g", "-table", "-lvt", "-lnt:numbers"});
    std::string refPlain = reference(kDebug, {"-g", "-lvt", "-lnt:numbers"});
    TASK_CHECK(!refDetailed.empty());
    TASK_CHECK(!refPlain.empty());
    TASK_CHECK(field(f, "FIRST") == refDetailed);
    TASK_CHECK(field(f, "SECOND") == refDetailed);
    TASK_CHECK(field(f, "PLAIN_FIRST") == refPlain);
    TASK_CHECK(field(f, "PLAIN_SECOND") == refPlain);
    // Local variable tables, exception ranges, and line numbers do not grow.
    TASK_CHECK(number(f, "FIRST_LOCAL_STARTS") == number(f, "SECOND_LOCAL_STARTS"));
    TASK_CHECK(number(f, "FIRST_ENDVARS") == number(f, "SECOND_ENDVARS"));
    TASK_CHECK(number(f, "FIRST_TRAPS") == number(f, "SECOND_TRAPS"));
    TASK_CHECK(number(f, "FIRST_LINES") == number(f, "SECOND_LINES"));
    TASK_CHECK(number(f, "PLAIN_FIRST_VARS") == number(f, "PLAIN_SECOND_VARS"));
    TASK_CHECK(number(f, "PLAIN_FIRST_ENDVARS") == number(f, "PLAIN_SECOND_ENDVARS"));
    TASK_CHECK(number(f, "PLAIN_FIRST_TYPES") == number(f, "PLAIN_SECOND_TYPES"));
    TASK_CHECK(number(f, "PLAIN_FIRST_STACK") == number(f, "PLAIN_SECOND_STACK"));
    // ... and the first run really did emit them.
    TASK_CHECK(number(f, "FIRST_LOCAL_STARTS") > 0);
    TASK_CHECK(number(f, "FIRST_TRAPS") > 0);
    TASK_CHECK(number(f, "FIRST_LINES") > 0);
    TASK_CHECK(number(f, "PLAIN_FIRST_VARS") > 0);
    TASK_CHECK(number(f, "PLAIN_FIRST_ENDVARS") > 0);
    TASK_CHECK(number(f, "PLAIN_FIRST_TYPES") > 0);
    return 0;
}

static int case_debug_ownership_of_tables() {
    // The detailed instance must own its LocalVariable(Type)Table and line
    // information even when a default instance exists and runs first.
    Fields f;
    TASK_CHECK(run_probe({"ownership", kDebug}, f));
    std::string detailed = reference(kDebug, {"-g", "-table", "-lvt", "-lnt:numbers"});
    std::string value = field(f, "B");
    TASK_CHECK(value == detailed);
    TASK_CHECK(has(value, "LocalVariables:"));
    TASK_CHECK(has(value, "LocalVariableTypes:"));
    TASK_CHECK(has(value, "// \"item\":\"TT;\""));
    TASK_CHECK(has(field(f, "A"), "invokeinterface"));
    // Default mode keeps the plain JASM form: no local variable tables or line tables.
    TASK_CHECK(!has(field(f, "A"), "LocalVariables:"));
    TASK_CHECK(!has(field(f, "A"), "LocalVariableTypes:"));
    TASK_CHECK(!has(field(f, "A"), "LineNumberTable:"));
    return 0;
}

static int case_debug_separate_switches() {
    // -lvt alone keeps only the LocalVariableTable; the type table needs its own switch.
    Fields lvt;
    TASK_CHECK(run_probe({"once", kDebug, "-lvt"}, lvt));
    std::string val = field(lvt, "OUT");
    // Without -table the variable scope events are printed as var/endvar lines,
    // and blank -lvt expands to both the variable and type tables.
    TASK_CHECK(has(val, " var "));
    TASK_CHECK(has(val, " endvar "));
    TASK_CHECK(has(val, " type "));
    TASK_CHECK(has(val, " endtype "));
    TASK_CHECK(val == reference(kDebug, {"-lvt"}));

    Fields lvtVars;
    TASK_CHECK(run_probe({"once", kDebug, "-lvt:vars"}, lvtVars));
    std::string varsVal = field(lvtVars, "OUT");
    TASK_CHECK(has(varsVal, " var "));
    TASK_CHECK(!has(varsVal, " type "));
    TASK_CHECK(varsVal == reference(kDebug, {"-lvt:vars"}));

    Fields lvtTypes;
    TASK_CHECK(run_probe({"once", kDebug, "-lvt:types"}, lvtTypes));
    std::string typesVal = field(lvtTypes, "OUT");
    TASK_CHECK(!has(typesVal, " var "));
    TASK_CHECK(has(typesVal, " type "));
    TASK_CHECK(typesVal == reference(kDebug, {"-lvt:types"}));

    // With -table the same information becomes the local variable table.
    Fields lvtTable;
    TASK_CHECK(run_probe({"once", kDebug, "-table", "-lvt"}, lvtTable));
    std::string tableVal = field(lvtTable, "OUT");
    TASK_CHECK(has(tableVal, "LocalVariables:"));
    TASK_CHECK(has(tableVal, "LocalVariableTypes:"));
    TASK_CHECK(tableVal == reference(kDebug, {"-table", "-lvt"}));

    Fields lnt;
    TASK_CHECK(run_probe({"once", kDebug, "-lnt:numbers"}, lnt));
    TASK_CHECK(field(lnt, "OUT") == reference(kDebug, {"-lnt:numbers"}));
    TASK_CHECK(!has(field(lnt, "OUT"), "LocalVariables:"));
    TASK_CHECK(has(field(lnt, "OUT"), "// "));
    return 0;
}

static int case_format_default_and_detailed() {
    std::string refDefault = reference(kLocal, {});
    std::string refDetailed = reference(kLocal, {"-g", "-table", "-lvt", "-lnt:numbers"});
    TASK_CHECK(!refDefault.empty());
    TASK_CHECK(!refDetailed.empty());
    // Default keeps the label form (L<pc>:) and no bytecode positions or
    // constant pool indexes; the detailed form has both.
    TASK_CHECK(has(refDefault, "L30:"));
    TASK_CHECK(!has(refDefault, "const #1"));
    TASK_CHECK(!has(refDefault, " 4:"));
    TASK_CHECK(has(refDetailed, "const #1"));
    TASK_CHECK(has(refDetailed, " 4:"));
    TASK_CHECK(has(refDetailed, "LocalVariables:"));
    // Both end with the class footer.
    TASK_CHECK(has(refDefault, "// end Class LocalFixture"));
    TASK_CHECK(has(refDetailed, "// end Class LocalFixture"));
    return 0;
}

static int case_format_independent_combinations() {
    const std::vector<std::vector<std::string>> combos = {
            {},
            {"-g"},
            {"-table"},
            {"-lvt"},
            {"-lnt:numbers"},
            {"-g", "-table"},
            {"-gg"},
    };
    for (const auto &opts : combos) {
        std::string expected = reference(kLocal, opts);
        TASK_CHECK(!expected.empty());
        Fields f;
        std::vector<std::string> args = {"once", kLocal};
        args.insert(args.end(), opts.begin(), opts.end());
        TASK_CHECK(run_probe(args, f));
        TASK_CHECK(field(f, "OUT") == expected);
    }
    return 0;
}

static int case_format_interleaved_bodies() {
    // Each instance keeps its own table/comment alignment while alternating.
    Fields f;
    TASK_CHECK(run_probe({"interleave", kDebug}, f));
    std::string detailed = reference(kDebug, {"-g", "-table", "-lvt", "-lnt:numbers"});
    std::string plain = reference(kDebug, {});
    TASK_CHECK(field(f, "A_TWICE") == plain + plain);
    TASK_CHECK(field(f, "B_TWICE") == detailed + detailed);
    // The detailed body keeps constant pool indexes in operands and in the pool.
    TASK_CHECK(has(field(f, "B_ONCE"), "invokeinterface   #"));
    TASK_CHECK(has(field(f, "B_ONCE"), "// \"item\":\"TT;\""));
    TASK_CHECK(has(field(f, "B_ONCE"), "LocalVariableTypes:"));
    return 0;
}

const struct task_case task_cases[] = {
        {"ownership", "default_first", case_ownership_default_first},
        {"ownership", "detailed_first", case_ownership_detailed_first},
        {"ownership", "reargue", case_ownership_reargue},
        {"ownership", "interleaved", case_ownership_interleaved},
        {"ownership", "parallel", case_ownership_parallel},
        {"debug", "repeat_stable", case_debug_repeat_stable},
        {"debug", "tables_owned", case_debug_ownership_of_tables},
        {"debug", "separate_switches", case_debug_separate_switches},
        {"format", "default_and_detailed", case_format_default_and_detailed},
        {"format", "independent_combinations", case_format_independent_combinations},
        {"format", "interleaved_bodies", case_format_interleaved_bodies},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
