// Regression tests for structuring shared-entry labelled blocks.
//
// The tests drive both production entry points (org.benf.cfr.reader.Main and the
// bundled ApiDump), recompile the emitted Java with javac -proc:none, and then
// parse javap output of BOTH the original and recompiled classes into a control
// flow graph.  Assertions are graph facts (branch/init/header/back-edge/exit
// relations and which path a side effect belongs to), not text searches or
// instruction counts.  Fixture methods are never executed.
#include "task_support/check.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

// Runtime classpath and original fixture directory.  Overridable only so the
// suite can be pointed at an unpatched CFR build for a negative check.
const std::string kCfrClasses = [] {
    const char* env = std::getenv("TASK_CFR_CLASSES");
    return env && *env ? std::string(env) : std::string(".task-build/cfr");
}();
const std::string kWorkDir = ".task-build/task-work";
const std::string kMarkers = "\\*\\* GOTO|Unable to fully structure code";

std::string runCapture(const std::string& cmd) {
    std::string out;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return out;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) out += buf;
    pclose(pipe);
    return out;
}

int runStatus(const std::string& cmd) {
    int rc = system(cmd.c_str());
    return rc;
}

std::string shellQuote(const std::string& s) {
    return "'" + std::regex_replace(s, std::regex("'"), "'\\''") + "'";
}

bool fileContains(const std::string& path, const std::string& re) {
    std::ifstream in(path);
    if (!in) return false;
    std::regex rx(re);
    std::string line;
    while (std::getline(in, line)) {
        if (std::regex_search(line, rx)) return true;
    }
    return false;
}

struct Insn {
    long offset = 0;
    std::string mnemonic;
    long branchTarget = -1;      // valid for if*/goto
    std::string effectName;      // method/field name for invoke/field ops
    std::vector<int> succ;       // indices into MethodModel::insns
};

struct MethodModel {
    std::string name;
    std::vector<Insn> insns;
    std::map<long, int> atOffset;

    const Insn* at(long off) const {
        auto it = atOffset.find(off);
        return it == atOffset.end() ? nullptr : &insns[it->second];
    }

    bool isTerminator(const std::string& m) const {
        if (m == "goto" || m == "goto_w" || m == "athrow" ||
            m == "tableswitch" || m == "lookupswitch") return true;
        if (m == "return" || m == "ireturn" || m == "lreturn" ||
            m == "freturn" || m == "dreturn" || m == "areturn") return true;
        return false;
    }

    // Nodes reachable from start index following CFG edges.
    std::set<int> reachable(int start) const {
        std::set<int> seen;
        std::vector<int> stack(1, start);
        while (!stack.empty()) {
            int idx = stack.back();
            stack.pop_back();
            if (idx < 0 || idx >= (int)insns.size()) continue;
            if (!seen.insert(idx).second) continue;
            for (int s : insns[idx].succ) stack.push_back(s);
        }
        return seen;
    }

    // Nodes that can reach the given index (reverse reachability).
    std::set<int> ancestors(int target) const {
        std::vector<std::vector<int>> pred(insns.size());
        for (size_t i = 0; i < insns.size(); ++i)
            for (int s : insns[i].succ) pred[s].push_back((int)i);
        std::set<int> seen;
        std::vector<int> stack(1, target);
        while (!stack.empty()) {
            int idx = stack.back();
            stack.pop_back();
            if (!seen.insert(idx).second) continue;
            for (int p : pred[idx]) stack.push_back(p);
        }
        return seen;
    }
};

std::string trailingName(const std::string& line, char opener, char closer) {
    size_t slash = line.find("//");
    if (slash == std::string::npos) return "";
    size_t op = line.find(opener, slash);
    if (op == std::string::npos) return "";
    std::string head = line.substr(slash + 2, op - slash - 2);
    size_t dot = head.find_last_of("./ ");
    std::string nm = (dot == std::string::npos) ? head : head.substr(dot + 1);
    while (!nm.empty() && (nm.back() == ' ' || nm.back() == '\t' || nm.back() == ':'))
        nm.pop_back();
    (void)closer;
    return nm;
}

// Parse one method out of `javap -c -p` text.
MethodModel parseMethod(const std::string& javapText, const std::string& methodName) {
    MethodModel model;
    model.name = methodName;
    std::istringstream in(javapText);
    std::string line;
    std::regex headerRx("^\\s+(?:[\\w\\[\\]]+\\s+)*(\\w+)\\s*\\(");
    std::regex insnRx("^\\s+(\\d+):\\s+(\\S+)(.*)$");
    std::smatch hm;
    bool inMethod = false;
    while (std::getline(in, line)) {
        if (std::regex_search(line, hm, headerRx) &&
            line.find(';') != std::string::npos &&
            line.find('(') < line.find(';')) {
            inMethod = hm[1].str() == methodName;
            continue;
        }
        if (!inMethod) continue;
        std::smatch m;
        if (!std::regex_search(line, m, insnRx)) continue;
        Insn insn;
        insn.offset = std::stol(m[1].str());
        insn.mnemonic = m[2].str();
        std::string rest = m[3].str();
        if (insn.mnemonic == "goto" || insn.mnemonic == "goto_w" ||
            insn.mnemonic.rfind("if", 0) == 0) {
            std::smatch tm;
            if (std::regex_search(rest, tm, std::regex("\\s*(\\d+)")))
                insn.branchTarget = std::stol(tm[1].str());
        }
        if (insn.mnemonic.rfind("invoke", 0) == 0)
            insn.effectName = "invoke " + trailingName(line, '(', ')');
        else if (insn.mnemonic == "putstatic" || insn.mnemonic == "putfield")
            insn.effectName = "write " + trailingName(line, ':', ' ');
        else if (insn.mnemonic == "getstatic" || insn.mnemonic == "getfield")
            insn.effectName = "read " + trailingName(line, ':', ' ');
        model.insns.push_back(insn);
    }
    for (size_t i = 0; i < model.insns.size(); ++i)
        model.atOffset[model.insns[i].offset] = (int)i;
    for (size_t i = 0; i < model.insns.size(); ++i) {
        const Insn& insn = model.insns[i];
        if (insn.branchTarget >= 0) {
            auto it = model.atOffset.find(insn.branchTarget);
            if (it != model.atOffset.end()) model.insns[i].succ.push_back(it->second);
        }
        if (!model.isTerminator(insn.mnemonic) && i + 1 < model.insns.size())
            model.insns[i].succ.push_back((int)i + 1);
    }
    return model;
}

// Shared topology facts for a method containing one loop with a bypass into a
// shared entry.  Offsets are computed independently per class, so the same
// structure can be compared even though javac renumbers everything.
struct Topology {
    bool valid = false;
    long header = -1;       // target of the back edge(s)
    long guard = -1;        // loop condition branch
    long join = -1;         // block after the loop, bypass and loop exit meet here
    long bypass = -1;      // forward goto skipping header into the join block
    std::vector<long> backEdges;
};

Topology analyzeTopology(const MethodModel& m) {
    Topology t;
    std::set<long> headers;
    for (const auto& i : m.insns) {
        if (i.branchTarget >= 0 && i.branchTarget < i.offset) {
            t.backEdges.push_back(i.offset);
            headers.insert(i.branchTarget);
        }
    }
    if (headers.size() != 1) return t;
    t.header = *headers.begin();
    const Insn* guardInsn = nullptr;
    for (const auto& i : m.insns) {
        if (i.offset >= t.header && i.branchTarget >= 0 &&
            i.branchTarget > i.offset && i.mnemonic.rfind("if", 0) == 0) {
            guardInsn = &i;
            break;
        }
    }
    if (!guardInsn) return t;
    t.guard = guardInsn->offset;
    t.join = guardInsn->branchTarget;
    for (const auto& i : m.insns) {
        if ((i.mnemonic == "goto" || i.mnemonic == "goto_w") &&
            i.offset < t.header && i.branchTarget == t.join) {
            t.bypass = i.offset;
            break;
        }
    }
    if (t.bypass < 0) return t;
    t.valid = true;
    return t;
}

// Classify effect descriptors by the path they belong to.
struct EffectRegions {
    std::vector<std::string> bypassArm;  // before header, can reach the bypass goto
    std::vector<std::string> init;       // before header, on the loop-entry path
    std::vector<std::string> body;       // at/after header, before join
    std::vector<std::string> post;       // at/after join
};

int indexOf(const MethodModel& m, long offset) {
    auto it = m.atOffset.find(offset);
    return it == m.atOffset.end() ? -1 : it->second;
}

EffectRegions classifyEffects(const MethodModel& m, const Topology& t) {
    EffectRegions regions;
    int bypassIdx = indexOf(m, t.bypass);
    std::set<int> arm = m.ancestors(bypassIdx);
    for (size_t i = 0; i < m.insns.size(); ++i) {
        const Insn& insn = m.insns[i];
        if (insn.effectName.empty()) continue;
        std::string desc = insn.mnemonic + " " + insn.effectName;
        if (insn.offset >= t.join) {
            regions.post.push_back(desc);
        } else if (insn.offset >= t.header) {
            regions.body.push_back(desc);
        } else if (arm.count((int)i)) {
            regions.bypassArm.push_back(desc);
        } else {
            regions.init.push_back(desc);
        }
    }
    return regions;
}

std::string javap(const std::string& classDir, const std::string& className) {
    return runCapture("javap -c -p -cp " + shellQuote(classDir) + " " + className + " 2>/dev/null");
}

std::string mainOut(const std::string& cls) { return kWorkDir + "/main-" + cls; }
std::string apiOut(const std::string& cls) { return kWorkDir + "/api-" + cls + ".java"; }
std::string rcDir(const std::string& cls) { return kWorkDir + "/rc-" + cls; }

bool decompileBoth(const std::string& cls) {
    std::error_code ec;
    fs::create_directories(kWorkDir, ec);
    std::string input = kCfrClasses + "/" + cls + ".class";
    std::string mainCmd = "java -cp " + shellQuote(kCfrClasses) +
                          " org.benf.cfr.reader.Main " + shellQuote(input) +
                          " --outputdir " + shellQuote(mainOut(cls)) + " >/dev/null 2>&1";
    std::string apiCmd = "java -cp " + shellQuote(kCfrClasses) +
                         " ApiDump " + shellQuote(input) + " " + shellQuote(apiOut(cls)) +
                         " >/dev/null 2>&1";
    runStatus(mainCmd);
    runStatus(apiCmd);
    if (!fs::exists(mainOut(cls) + "/" + cls + ".java", ec)) return false;
    if (!fs::exists(apiOut(cls), ec)) return false;
    return true;
}

bool noMarkers(const std::string& cls) {
    return !fileContains(mainOut(cls) + "/" + cls + ".java", kMarkers) &&
           !fileContains(apiOut(cls), kMarkers);
}

bool recompile(const std::string& cls) {
    std::error_code ec;
    fs::create_directories(rcDir(cls), ec);
    std::string cmd = "javac -proc:none --release 8 -d " + shellQuote(rcDir(cls)) +
                      " " + shellQuote(mainOut(cls) + "/" + cls + ".java") +
                      " >/dev/null 2>&1";
    return runStatus(cmd) == 0 &&
           fs::exists(rcDir(cls) + "/" + cls + ".class", ec);
}

Topology topologyOf(const std::string& classDir, const std::string& cls,
                    const std::string& method) {
    return analyzeTopology(parseMethod(javap(classDir, cls), method));
}

// ---- structural assertions shared by every shared-entry fixture ----
int checkSharedTopology(const std::string& cls, const std::string& method,
                        unsigned expectedBackEdges) {
    // The structured Java must exist and compile before its class can be checked.
    TASK_CHECK(decompileBoth(cls));
    TASK_CHECK(noMarkers(cls));
    TASK_CHECK(recompile(cls));

    Topology t = topologyOf(kCfrClasses, cls, method);
    TASK_CHECK(t.valid);
    TASK_CHECK(t.backEdges.size() == expectedBackEdges);
    const MethodModel m = parseMethod(javap(kCfrClasses, cls), method);
    TASK_CHECK(indexOf(m, t.header) >= 0);
    TASK_CHECK(indexOf(m, t.guard) >= 0);
    TASK_CHECK(indexOf(m, t.join) >= 0);
    TASK_CHECK(indexOf(m, t.bypass) >= 0);
    // Bypass skips the shared entry and the loop header, landing on the join.
    TASK_CHECK(t.bypass < t.header);
    TASK_CHECK(t.header < t.guard);
    TASK_CHECK(t.join > t.guard);
    // Every back edge returns to the same loop header.
    for (long off : t.backEdges) {
        const Insn* be = m.at(off);
        TASK_CHECK(be != nullptr);
        TASK_CHECK(be->branchTarget == t.header);
    }
    // Both the loop guard and the bypass converge on the join block.
    const Insn* guard = m.at(t.guard);
    const Insn* bypass = m.at(t.bypass);
    TASK_CHECK(guard != nullptr && bypass != nullptr);
    TASK_CHECK(guard->branchTarget == t.join);
    TASK_CHECK(bypass->branchTarget == t.join);
    // The join block is reachable from the method entry and has statements.
    std::set<int> reach = m.reachable(0);
    TASK_CHECK(reach.count(indexOf(m, t.join)));
    // Statements after the loop really are after it, not swallowed into an arm.
    bool hasPost = false;
    for (const auto& i : m.insns)
        if (i.offset >= t.join && i.mnemonic.rfind("return", 0) != 0) hasPost = true;
    TASK_CHECK(hasPost);

    // The recompiled class must carry the same structural relations, independently
    // renumbered, proving the emitted Java encodes (not just prints) the structure.
    Topology rt = topologyOf(rcDir(cls), cls, method);
    TASK_CHECK(rt.valid);
    TASK_CHECK(rt.backEdges.size() == expectedBackEdges);
    TASK_CHECK(rt.bypass < rt.header);
    TASK_CHECK(rt.header < rt.guard);
    TASK_CHECK(rt.join > rt.guard);
    const MethodModel rm = parseMethod(javap(rcDir(cls), cls), method);
    const Insn* rguard = rm.at(rt.guard);
    const Insn* rbypass = rm.at(rt.bypass);
    TASK_CHECK(rguard != nullptr && rbypass != nullptr);
    TASK_CHECK(rguard->branchTarget == rt.join);
    TASK_CHECK(rbypass->branchTarget == rt.join);
    for (long off : rt.backEdges) {
        const Insn* be = rm.at(off);
        TASK_CHECK(be != nullptr);
        TASK_CHECK(be->branchTarget == rt.header);
    }
    return 0;
}

int case_shared_entry() {
    return checkSharedTopology("SeSharedEntry", "scan", 1);
}

int case_inverted() {
    return checkSharedTopology("SeInverted", "scan", 1);
}

int case_continue_loop() {
    // A continue adds a second back edge to the same header.
    return checkSharedTopology("SeContinue", "scan", 2);
}

int case_ordinary_loop() {
    MethodModel sum = parseMethod(javap(kCfrClasses, "SeOrdinary"), "sum");
    TASK_CHECK(!sum.insns.empty());
    std::set<long> headers;
    unsigned forwardGotos = 0;
    for (const auto& i : sum.insns) {
        if (i.branchTarget >= 0 && i.branchTarget < i.offset) headers.insert(i.branchTarget);
        if ((i.mnemonic == "goto" || i.mnemonic == "goto_w") &&
            i.branchTarget > i.offset) ++forwardGotos;
    }
    TASK_CHECK(headers.size() == 1);
    TASK_CHECK(forwardGotos == 0);
    TASK_CHECK(decompileBoth("SeOrdinary"));
    TASK_CHECK(noMarkers("SeOrdinary"));
    std::ifstream out(mainOut("SeOrdinary") + "/SeOrdinary.java");
    std::string text((std::istreambuf_iterator<char>(out)),
                     std::istreambuf_iterator<char>());
    TASK_CHECK(text.find("for (") != std::string::npos);
    TASK_CHECK(text.find("block") == std::string::npos);
    TASK_CHECK(text.find("continue;") != std::string::npos);
    return 0;
}

int case_effect_placement_original() {
    MethodModel m = parseMethod(javap(kCfrClasses, "SeSideEffects"), "scan");
    Topology t = analyzeTopology(m);
    TASK_CHECK(t.valid);
    EffectRegions r = classifyEffects(m, t);
    // Bypass arm performs exactly one observe() and nothing else side effectful.
    TASK_CHECK(r.bypassArm.size() == 1);
    TASK_CHECK(r.bypassArm[0] == "invokestatic invoke observe");
    // Shared initialisation: the second observe(), the field write and reads,
    // in source order, all on the loop-entry path.
    TASK_CHECK(std::find(r.init.begin(), r.init.end(),
                         "invokestatic invoke observe") != r.init.end());
    TASK_CHECK(std::find(r.init.begin(), r.init.end(),
                         "putstatic write side") != r.init.end());
    // The loop body and the post-loop block each write trace exactly once.
    TASK_CHECK(std::count(r.body.begin(), r.body.end(),
                          "putstatic write trace") == 1);
    TASK_CHECK(std::count(r.post.begin(), r.post.end(),
                          "putstatic write trace") == 1);
    // Zero-iteration path: initialisation reaches the header without looping.
    int initObserve = -1;
    for (size_t i = 0; i < m.insns.size(); ++i) {
        if (m.insns[i].offset >= t.header) break;
        if (m.insns[i].effectName == "invoke observe") initObserve = (int)i;
    }
    TASK_CHECK(initObserve >= 0);
    TASK_CHECK(m.reachable(initObserve).count(indexOf(m, t.header)));
    // Back edge path must not re-run initialisation.
    TASK_CHECK(!m.reachable(indexOf(m, t.header)).count(initObserve));
    // Bypass path must not run initialisation either.
    TASK_CHECK(!m.reachable(indexOf(m, t.join)).count(initObserve));
    return 0;
}

int case_effect_placement_equivalent() {
    TASK_CHECK(decompileBoth("SeSideEffects"));
    TASK_CHECK(noMarkers("SeSideEffects"));
    TASK_CHECK(recompile("SeSideEffects"));
    MethodModel orig = parseMethod(javap(kCfrClasses, "SeSideEffects"), "scan");
    MethodModel rc = parseMethod(javap(rcDir("SeSideEffects"), "SeSideEffects"), "scan");
    Topology to = analyzeTopology(orig);
    Topology tc = analyzeTopology(rc);
    TASK_CHECK(to.valid && tc.valid);
    TASK_CHECK(to.backEdges.size() == tc.backEdges.size());
    EffectRegions ro = classifyEffects(orig, to);
    EffectRegions rcRegions = classifyEffects(rc, tc);
    TASK_CHECK(ro.bypassArm == rcRegions.bypassArm);
    TASK_CHECK(ro.init == rcRegions.init);
    TASK_CHECK(ro.body == rcRegions.body);
    TASK_CHECK(ro.post == rcRegions.post);
    return 0;
}

int case_compatibility_shared() {
    const std::string cls = "SeSharedEntry";
    TASK_CHECK(decompileBoth(cls));
    TASK_CHECK(noMarkers(cls));
    TASK_CHECK(recompile(cls));
    Topology to = topologyOf(kCfrClasses, cls, "scan");
    Topology tc = topologyOf(rcDir(cls), cls, "scan");
    TASK_CHECK(to.valid && tc.valid);
    TASK_CHECK(to.backEdges.size() == 1 && tc.backEdges.size() == 1);
    TASK_CHECK(to.bypass < to.header && tc.bypass < tc.header);
    TASK_CHECK(to.join > to.guard && tc.join > tc.guard);
    // The recompiled class keeps the bypass/guard convergence relation.
    MethodModel rc = parseMethod(javap(rcDir(cls), cls), "scan");
    const Insn* guard = rc.at(tc.guard);
    const Insn* bypass = rc.at(tc.bypass);
    TASK_CHECK(guard != nullptr && bypass != nullptr);
    TASK_CHECK(guard->branchTarget == tc.join);
    TASK_CHECK(bypass->branchTarget == tc.join);
    return 0;
}

int case_compatibility_all_fixtures() {
    const std::string classes[] = {"SeSharedEntry", "SeSideEffects", "SeInverted",
                                   "SeContinue", "SeOrdinary"};
    for (const std::string& cls : classes) {
        TASK_CHECK(decompileBoth(cls));
        TASK_CHECK(noMarkers(cls));
        TASK_CHECK(recompile(cls));
        std::error_code ec;
        TASK_CHECK(fs::exists(rcDir(cls) + "/" + cls + ".class", ec));
    }
    return 0;
}

}  // namespace

const struct task_case task_cases[] = {
#define ENTRY(g, n, f) {g, n, f}
    ENTRY("structure", "shared_entry_topology", case_shared_entry),
    ENTRY("structure", "inverted_condition_topology", case_inverted),
    ENTRY("structure", "continue_back_edges_topology", case_continue_loop),
    ENTRY("structure", "ordinary_loop_unchanged", case_ordinary_loop),
    ENTRY("effects", "side_effect_path_placement_original", case_effect_placement_original),
    ENTRY("effects", "side_effect_path_placement_recompiled", case_effect_placement_equivalent),
    ENTRY("compatibility", "main_and_api_shared_entry_recompile", case_compatibility_shared),
    ENTRY("compatibility", "all_fixtures_recompile_clean", case_compatibility_all_fixtures),
#undef ENTRY
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
