// Content tests for the per-corpus coverage snapshot and comparison
// workflow, registered with the task driver in the groups capture, archive,
// and replay. Every scenario that touches instrumented modules runs in a
// fresh child process (fork), so module load order and bindings never leak
// between cases and reversed load order can be exercised per process.

#define FUZZCOVER_NOFUZZER
#include <fuzzcover/fuzzcover.hpp>
#include <fuzzcover/coverage.hpp>
#include "task_support/check.h"

#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
const char* kModuleA = ".task-build/mod-a/probe.so";
const char* kModuleB = ".task-build/mod-b/probe.so";

const std::string kBuildA(64, 'a');
const std::string kLayoutA(64, 'b');
const std::string kBuildB(64, 'c');
const std::string kLayoutB(64, 'd');
const std::string kBuildC(64, 'e');
const std::string kLayoutC(64, 'f');
const std::string kSource(40, 'a');
const std::string kOtherSource(40, 'f');
const char* kRoleA = "mod_a";
const char* kRoleB = "mod_b";

using covered_value_fn = int (*)(int);

struct ProbeWorld
{
    covered_value_fn fn_a = nullptr;
    covered_value_fn fn_b = nullptr;
};

// load both probe modules; with reversed set, mod-b is loaded before mod-a
bool load_world(ProbeWorld& world, bool reversed)
{
    const char* first_path = reversed ? kModuleB : kModuleA;
    const char* second_path = reversed ? kModuleA : kModuleB;
    void* first = dlopen(first_path, RTLD_NOW | RTLD_LOCAL);
    void* second = dlopen(second_path, RTLD_NOW | RTLD_LOCAL);
    if (first == nullptr || second == nullptr)
    {
        return false;
    }
    void* handle_a = reversed ? second : first;
    void* handle_b = reversed ? first : second;
    world.fn_a = reinterpret_cast<covered_value_fn>(dlsym(handle_a, "covered_value"));
    world.fn_b = reinterpret_cast<covered_value_fn>(dlsym(handle_b, "covered_value"));
    return world.fn_a != nullptr && world.fn_b != nullptr;
}

bool bind_world(const ProbeWorld& world)
{
    return fuzzcover::BindCoverageModule(reinterpret_cast<const void*>(world.fn_a), kRoleA, kBuildA, kLayoutA) &&
           fuzzcover::BindCoverageModule(reinterpret_cast<const void*>(world.fn_b), kRoleB, kBuildB, kLayoutB);
}

class ProbeFixture : public fuzzcover::fuzzcover_interface<unsigned, int>
{
  public:
    const ProbeWorld* world = nullptr;
    unsigned conversions = 0;
    unsigned executions = 0;
    bool cover_b = true;          // call fn_b for odd inputs (result discarded)
    bool throw_on_execution = false;
    unsigned input_shift = 0;     // simulates a changed conversion
    int output_shift = 0;         // simulates a changed normal result

    unsigned value_from_bytes(const std::uint8_t* data, std::size_t size) override
    {
        ++conversions;
        unsigned value = 0;
        for (std::size_t i = 0; i < size; ++i)
        {
            value = value * 31 + data[i];
        }
        return value + input_shift;
    }

    int test_function(const unsigned& value) override
    {
        ++executions;
        if (throw_on_execution)
        {
            throw std::runtime_error("test function failed");
        }
        int result = world->fn_a(static_cast<int>(value % 200));
        if (cover_b && value % 2 != 0)
        {
            const int discarded = world->fn_b(static_cast<int>(value % 200));
            (void)discarded;
        }
        return result + output_shift;
    }
};

// scenario shared with the child process (fork copies it)
struct Scenario
{
    std::string dir;
    std::string corpus;
    std::string snapshot;
    std::string report;
    std::string stats;
    std::string log;
    std::string source = kSource;
    bool reverse_load = false;
    bool bind_a = true;
    bool bind_b = true;
    bool bind_b_after_record = false;
    std::string role_a = kRoleA;
    std::string build_a = kBuildA;
    std::string layout_a = kLayoutA;
    bool cover_b = true;
    bool throw_on_execution = false;
    unsigned input_shift = 0;
    int output_shift = 0;
};

Scenario g;

void reset_scenario(const char* name)
{
    g = Scenario();
    g.dir = std::string(".task-build/coverage-tests/") + name;
    g.corpus = g.dir + "/corpus";
    g.snapshot = g.dir + "/snapshot.json";
    g.report = g.dir + "/report.json";
    g.stats = g.dir + "/stats.txt";
    g.log = g.dir + "/log.txt";
    std::filesystem::remove_all(g.dir);
    std::filesystem::create_directories(g.dir);
}

void write_file(const std::string& path, const std::string& content)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

std::string read_text(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool file_exists(const std::string& path)
{
    return std::filesystem::exists(path);
}

void make_corpus(const std::string& dir, const std::vector<std::pair<std::string, std::string>>& files)
{
    std::filesystem::create_directories(dir);
    for (const auto& file : files)
    {
        write_file(dir + "/" + file.first, file.second);
    }
}

void write_stats(const std::string& path, unsigned conversions, unsigned executions)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "conversions=" << conversions << "\nexecutions=" << executions << "\n";
}

std::pair<long, long> read_stats(const std::string& path)
{
    std::ifstream in(path);
    std::string token;
    long conversions = -1;
    long executions = -1;
    while (in >> token)
    {
        if (token.rfind("conversions=", 0) == 0)
        {
            conversions = std::stol(token.substr(12));
        }
        if (token.rfind("executions=", 0) == 0)
        {
            executions = std::stol(token.substr(11));
        }
    }
    return {conversions, executions};
}

using child_function = int (*)();

int run_child(child_function fn)
{
    const pid_t pid = fork();
    if (pid < 0)
    {
        return 100;
    }
    if (pid == 0)
    {
        _exit(fn());
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
    {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 101;
}

void configure_fixture(ProbeFixture& fixture)
{
    fixture.cover_b = g.cover_b;
    fixture.throw_on_execution = g.throw_on_execution;
    fixture.input_shift = g.input_shift;
    fixture.output_shift = g.output_shift;
}

int bind_from_scenario(const ProbeWorld& world)
{
    if (g.bind_a && !fuzzcover::BindCoverageModule(reinterpret_cast<const void*>(world.fn_a), g.role_a, g.build_a, g.layout_a))
    {
        return 11;
    }
    if (g.bind_b && !fuzzcover::BindCoverageModule(reinterpret_cast<const void*>(world.fn_b), kRoleB, kBuildB, kLayoutB))
    {
        return 12;
    }
    return 0;
}

int child_record()
{
    ProbeWorld world;
    if (!load_world(world, g.reverse_load))
    {
        return 10;
    }
    if (const int bound = bind_from_scenario(world))
    {
        return bound;
    }
    ProbeFixture fixture;
    fixture.world = &world;
    configure_fixture(fixture);
    const int result = fixture.record_snapshot(g.corpus, g.source, g.snapshot);
    if (g.bind_b_after_record && !fuzzcover::BindCoverageModule(reinterpret_cast<const void*>(world.fn_b), kRoleB, kBuildB, kLayoutB))
    {
        return 13; // a leaked collection scope would reject this binding
    }
    write_stats(g.stats, fixture.conversions, fixture.executions);
    return result;
}

int child_verify()
{
    ProbeWorld world;
    if (!load_world(world, g.reverse_load))
    {
        return 10;
    }
    if (const int bound = bind_from_scenario(world))
    {
        return bound;
    }
    ProbeFixture fixture;
    fixture.world = &world;
    configure_fixture(fixture);
    const int result = fixture.verify_snapshot(g.snapshot, g.source, g.report);
    write_stats(g.stats, fixture.conversions, fixture.executions);
    return result;
}

// child-side checklist written to g.log, one "name=0|1" row per check
struct ChildLog
{
    std::vector<std::pair<std::string, bool>> rows;

    void add(const std::string& name, bool ok)
    {
        rows.emplace_back(name, ok);
    }

    int finish(const std::string& path)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        bool all = true;
        for (const auto& row : rows)
        {
            out << row.first << '=' << (row.second ? '1' : '0') << '\n';
            all = all && row.second;
        }
        return all ? 0 : 1;
    }
};

// parent-side: every row of the child log must be ok
int check_log(const std::string& path)
{
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line))
    {
        const auto pos = line.rfind('=');
        TASK_CHECK(pos != std::string::npos && line.substr(pos + 1) == "1");
    }
    return 0;
}

const nlohmann::json* find_entry_by_input(const nlohmann::json& snapshot, unsigned input)
{
    for (const auto& entry : snapshot.at("entries"))
    {
        if (entry.at("input") == input)
        {
            return &entry;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// capture group
// ---------------------------------------------------------------------------

int capture_guard_hits_and_item_isolation()
{
    reset_scenario("guard-hits");
    make_corpus(g.corpus, {{"low", std::string(1, static_cast<char>(10))}, {"high", std::string(1, static_cast<char>(150))}});
    TASK_CHECK(run_child(child_record) == 0);
    const nlohmann::json snapshot = nlohmann::json::parse(read_text(g.snapshot), nullptr, false);
    TASK_CHECK(!snapshot.is_discarded());
    TASK_CHECK(snapshot.at("modules").size() == 2);
    const std::uint64_t guards_a = snapshot.at("modules")[0].at("guards").get<std::uint64_t>();
    TASK_CHECK(guards_a >= 2); // entry plus both branches of covered_value
    const nlohmann::json* low = find_entry_by_input(snapshot, 10);
    const nlohmann::json* high = find_entry_by_input(snapshot, 150);
    TASK_CHECK(low != nullptr && high != nullptr);
    TASK_CHECK(low->at("names") == nlohmann::json::array({"low"}));
    TASK_CHECK(high->at("names") == nlohmann::json::array({"high"}));
    // real guards were recorded for each item
    TASK_CHECK(!low->at("coverage").empty());
    TASK_CHECK(!high->at("coverage").empty());
    for (const auto* entry : {low, high})
    {
        for (const auto& hit : entry->at("coverage"))
        {
            TASK_CHECK(hit.at("module") == "mod_a"); // even inputs never reach mod_b
            TASK_CHECK(hit.at("guard").get<std::uint64_t>() < guards_a);
        }
    }
    // the in-range and the out-of-range input take different branches, and
    // each entry holds only its own guards: no leakage between corpus items
    TASK_CHECK(low->at("coverage") != high->at("coverage"));
    const auto stats = read_stats(g.stats);
    TASK_CHECK(stats.first == 2);
    TASK_CHECK(stats.second == 2);
    return 0;
}

int capture_duplicate_bytes_and_shared_prefix_names()
{
    reset_scenario("duplicates");
    make_corpus(g.corpus, {{"abcdefg-one", std::string(1, 'A')},
                           {"abcdefg-two", std::string(1, 'B')},
                           {"duplicate", std::string(1, 'A')},
                           {"empty", ""}});
    TASK_CHECK(run_child(child_record) == 0);
    const nlohmann::json snapshot = nlohmann::json::parse(read_text(g.snapshot), nullptr, false);
    TASK_CHECK(!snapshot.is_discarded());
    // identical bytes merge into one entry; identical converted values with
    // different bytes would not
    const auto& entries = snapshot.at("entries");
    TASK_CHECK(entries.size() == 3);
    const nlohmann::json* merged = nullptr;
    const nlohmann::json* other = nullptr;
    const nlohmann::json* empty = nullptr;
    for (const auto& entry : entries)
    {
        if (entry.at("bytes") == "41")
        {
            merged = &entry;
        }
        if (entry.at("bytes") == "42")
        {
            other = &entry;
        }
        if (entry.at("bytes") == "")
        {
            empty = &entry;
        }
    }
    TASK_CHECK(merged != nullptr && other != nullptr && empty != nullptr);
    // files sharing the 7-character prefix of the old short hash stay distinct
    TASK_CHECK(merged->at("names") == nlohmann::json::array({"abcdefg-one", "duplicate"}));
    TASK_CHECK(other->at("names") == nlohmann::json::array({"abcdefg-two"}));
    // empty files are legal and carry the well-known empty digest
    TASK_CHECK(empty->at("sha256") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    TASK_CHECK(empty->at("names") == nlohmann::json::array({"empty"}));
    // each distinct byte sequence is converted and executed exactly once
    const auto stats = read_stats(g.stats);
    TASK_CHECK(stats.first == 3);
    TASK_CHECK(stats.second == 3);
    return 0;
}

int capture_unbound_module_fails()
{
    reset_scenario("unbound");
    make_corpus(g.corpus, {{"odd", std::string(1, static_cast<char>(3))}}); // odd input reaches mod_b
    g.bind_b = false;                                                        // mod_b stays unbound on purpose
    TASK_CHECK(run_child(child_record) == 2);
    TASK_CHECK(!file_exists(g.snapshot)); // no partial snapshot was written
    return 0;
}

int child_binding_rules()
{
    ChildLog log;
    ProbeWorld world;
    if (!load_world(world, false))
    {
        return 10;
    }
    const void* anchor_a = reinterpret_cast<const void*>(world.fn_a);
    const void* anchor_b = reinterpret_cast<const void*>(world.fn_b);
    log.add("role_empty", !fuzzcover::BindCoverageModule(anchor_a, "", kBuildA, kLayoutA));
    log.add("role_dash", !fuzzcover::BindCoverageModule(anchor_a, "bad-role", kBuildA, kLayoutA));
    log.add("role_space", !fuzzcover::BindCoverageModule(anchor_a, "bad role", kBuildA, kLayoutA));
    log.add("role_non_ascii", !fuzzcover::BindCoverageModule(anchor_a, "r\xc3\xb6le", kBuildA, kLayoutA));
    log.add("build_short", !fuzzcover::BindCoverageModule(anchor_a, kRoleA, std::string(63, 'a'), kLayoutA));
    log.add("build_long", !fuzzcover::BindCoverageModule(anchor_a, kRoleA, std::string(65, 'a'), kLayoutA));
    log.add("build_upper", !fuzzcover::BindCoverageModule(anchor_a, kRoleA, std::string(64, 'A'), kLayoutA));
    log.add("build_non_hex", !fuzzcover::BindCoverageModule(anchor_a, kRoleA, std::string(64, 'g'), kLayoutA));
    log.add("layout_bad", !fuzzcover::BindCoverageModule(anchor_a, kRoleA, kBuildA, std::string(64, 'G')));
    // an anchor in the main executable (no registered guards) cannot bind
    log.add("anchor_without_guards", !fuzzcover::BindCoverageModule(reinterpret_cast<const void*>(&child_binding_rules), kRoleA, kBuildA, kLayoutA));
    log.add("bind_a", fuzzcover::BindCoverageModule(anchor_a, kRoleA, kBuildA, kLayoutA));
    log.add("bind_a_idempotent", fuzzcover::BindCoverageModule(anchor_a, kRoleA, kBuildA, kLayoutA));
    // the same image must not change identity
    log.add("same_image_new_build", !fuzzcover::BindCoverageModule(anchor_a, kRoleA, kBuildC, kLayoutA));
    log.add("same_image_new_layout", !fuzzcover::BindCoverageModule(anchor_a, kRoleA, kBuildA, kLayoutC));
    log.add("same_image_new_role", !fuzzcover::BindCoverageModule(anchor_a, "renamed", kBuildA, kLayoutA));
    // a role must not move to another image
    log.add("role_reuse_other_image", !fuzzcover::BindCoverageModule(anchor_b, kRoleA, kBuildB, kLayoutB));
    // bindings must not change while a collection scope is active
    {
        fuzzcover::CoverageScope scope;
        log.add("no_bind_during_scope", !fuzzcover::BindCoverageModule(anchor_b, kRoleB, kBuildB, kLayoutB));
    }
    log.add("bind_b", fuzzcover::BindCoverageModule(anchor_b, kRoleB, kBuildB, kLayoutB));
    // the rejected attempts left the original bindings untouched
    const auto manifests = fuzzcover::BoundCoverageModules();
    log.add("manifest_size", manifests.size() == 2);
    log.add("manifest_a_intact", manifests.size() == 2 && manifests[0].role == kRoleA && manifests[0].build == kBuildA && manifests[0].layout == kLayoutA && manifests[0].guards > 0);
    log.add("manifest_b_intact", manifests.size() == 2 && manifests[1].role == kRoleB && manifests[1].build == kBuildB && manifests[1].layout == kLayoutB && manifests[1].guards > 0);
    return log.finish(g.log);
}

int capture_binding_rules()
{
    reset_scenario("binding-rules");
    TASK_CHECK(run_child(child_binding_rules) == 0);
    TASK_CHECK(check_log(g.log) == 0);
    return 0;
}

int child_corpus_rejections()
{
    ChildLog log;
    ProbeWorld world;
    if (!load_world(world, false))
    {
        return 10;
    }
    if (!bind_world(world))
    {
        return 11;
    }
    ProbeFixture fixture;
    fixture.world = &world;
    const std::string out = g.dir + "/out.json";
    log.add("empty_directory", fixture.record_snapshot(g.dir + "/empty", kSource, out) == 2);
    log.add("symlink_entry", fixture.record_snapshot(g.dir + "/links", kSource, out) == 2);
    log.add("subdirectory_entry", fixture.record_snapshot(g.dir + "/nested", kSource, out) == 2);
    log.add("missing_directory", fixture.record_snapshot(g.dir + "/missing", kSource, out) == 2);
    log.add("source_not_hex", fixture.record_snapshot(g.corpus, "not-a-source-id", out) == 2);
    log.add("source_short", fixture.record_snapshot(g.corpus, std::string(39, 'a'), out) == 2);
    log.add("source_long", fixture.record_snapshot(g.corpus, std::string(41, 'a'), out) == 2);
    log.add("source_uppercase", fixture.record_snapshot(g.corpus, std::string(40, 'A'), out) == 2);
    log.add("no_output_written", !file_exists(out));
    return log.finish(g.log);
}

int child_no_binding_record()
{
    ProbeWorld world;
    if (!load_world(world, false))
    {
        return 10;
    }
    ProbeFixture fixture;
    fixture.world = &world;
    // without any valid bound module no successful snapshot may be produced
    if (fixture.record_snapshot(g.corpus, kSource, g.snapshot) != 2)
    {
        return 1;
    }
    return file_exists(g.snapshot) ? 1 : 0;
}

int capture_corpus_and_source_rejections()
{
    reset_scenario("corpus-rejections");
    make_corpus(g.corpus, {{"one", "A"}});
    std::filesystem::create_directories(g.dir + "/empty");
    std::filesystem::create_directories(g.dir + "/links");
    std::filesystem::create_directories(g.dir + "/nested/inner");
    symlink("target", (g.dir + "/links/link").c_str());
    TASK_CHECK(run_child(child_corpus_rejections) == 0);
    TASK_CHECK(check_log(g.log) == 0);
    TASK_CHECK(run_child(child_no_binding_record) == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// archive group
// ---------------------------------------------------------------------------

int archive_snapshot_structure()
{
    reset_scenario("snapshot-structure");
    make_corpus(g.corpus, {{"one", std::string(1, static_cast<char>(1))},
                           {"two", std::string(1, static_cast<char>(2))},
                           {"three", std::string(1, static_cast<char>(3))}});
    TASK_CHECK(run_child(child_record) == 0);
    const nlohmann::json snapshot = nlohmann::json::parse(read_text(g.snapshot), nullptr, false);
    TASK_CHECK(!snapshot.is_discarded());
    TASK_CHECK(snapshot.at("format") == "fuzzcover-regression-1");
    TASK_CHECK(snapshot.at("source") == kSource);
    // module manifest sorted by role with the bound identities
    const auto& modules = snapshot.at("modules");
    TASK_CHECK(modules.size() == 2);
    TASK_CHECK(modules[0].at("role") == kRoleA);
    TASK_CHECK(modules[0].at("build") == kBuildA);
    TASK_CHECK(modules[0].at("layout") == kLayoutA);
    TASK_CHECK(modules[0].at("guards").get<std::uint64_t>() >= 2);
    TASK_CHECK(modules[1].at("role") == kRoleB);
    TASK_CHECK(modules[1].at("build") == kBuildB);
    TASK_CHECK(modules[1].at("layout") == kLayoutB);
    // entries sorted by digest, with recomputable raw digests
    const auto& entries = snapshot.at("entries");
    TASK_CHECK(entries.size() == 3);
    std::string previous;
    for (const auto& entry : entries)
    {
        TASK_CHECK(entry.size() == 6);
        const std::string sha256 = entry.at("sha256").get<std::string>();
        TASK_CHECK(previous.empty() || previous < sha256);
        previous = sha256;
        const auto bytes = fuzzcover::detail::hex_decode(entry.at("bytes").get<std::string>());
        TASK_CHECK(bytes.has_value());
        TASK_CHECK(fuzzcover::detail::sha256_hex(*bytes) == sha256);
        TASK_CHECK(entry.at("names").size() == 1);
        // coverage sorted by module role and guard index
        const auto& coverage = entry.at("coverage");
        for (std::size_t i = 1; i < coverage.size(); ++i)
        {
            const auto& before = coverage[i - 1];
            const auto& here = coverage[i];
            const bool ordered = before.at("module") < here.at("module") ||
                                 (before.at("module") == here.at("module") && before.at("guard") < here.at("guard"));
            TASK_CHECK(ordered);
        }
    }
    // converted inputs and normal outputs of the benign arithmetic
    const nlohmann::json* one = find_entry_by_input(snapshot, 1);
    const nlohmann::json* two = find_entry_by_input(snapshot, 2);
    const nlohmann::json* three = find_entry_by_input(snapshot, 3);
    TASK_CHECK(one != nullptr && two != nullptr && three != nullptr);
    TASK_CHECK(one->at("output") == 3);   // covered_value(1) in mod-a
    TASK_CHECK(two->at("output") == 5);   // covered_value(2) in mod-a
    TASK_CHECK(three->at("output") == 7); // covered_value(3) in mod-a
    // the odd inputs also touched mod_b, the even one did not
    TASK_CHECK(one->at("coverage").size() > two->at("coverage").size());
    const auto stats = read_stats(g.stats);
    TASK_CHECK(stats.first == 3);
    TASK_CHECK(stats.second == 3);
    return 0;
}

int archive_corrupt_or_wrong_version_rejected()
{
    reset_scenario("corrupt-rejected");
    make_corpus(g.corpus, {{"one", std::string(1, static_cast<char>(1))},
                           {"two", std::string(1, static_cast<char>(2))}});
    TASK_CHECK(run_child(child_record) == 0);
    const nlohmann::json good = nlohmann::json::parse(read_text(g.snapshot), nullptr, false);
    TASK_CHECK(!good.is_discarded());

    std::vector<std::pair<std::string, nlohmann::json>> variants;
    auto add_variant = [&variants](std::string name, nlohmann::json document)
    {
        variants.emplace_back(std::move(name), std::move(document));
    };
    {
        nlohmann::json v = good;
        v["format"] = "fuzzcover-regression-2"; // wrong format version
        add_variant("wrong_format_version", v);
    }
    {
        nlohmann::json v = good;
        v["source"] = kOtherSource; // source identity mismatch
        add_variant("wrong_source_field", v);
    }
    {
        nlohmann::json v = good;
        std::string bytes = v["entries"][0]["bytes"].get<std::string>();
        bytes[0] = (bytes[0] == '0') ? '1' : '0'; // digest no longer matches
        v["entries"][0]["bytes"] = bytes;
        add_variant("tampered_bytes", v);
    }
    {
        nlohmann::json v = good;
        std::string digest = v["entries"][0]["sha256"].get<std::string>();
        digest[0] = (digest[0] == '0') ? '1' : '0';
        v["entries"][0]["sha256"] = digest;
        add_variant("tampered_digest", v);
    }
    {
        nlohmann::json v = good;
        std::swap(v["entries"][0], v["entries"][1]); // entries no longer sorted
        add_variant("entries_unsorted", v);
    }
    {
        nlohmann::json v = good;
        v["entries"][0]["coverage"].push_back({{"module", kRoleA}, {"guard", 999}}); // outside the manifest
        add_variant("guard_out_of_range", v);
    }
    {
        nlohmann::json v = good;
        v["entries"][0]["coverage"].push_back({{"module", "nope"}, {"guard", 0}});
        add_variant("coverage_unknown_module", v);
    }
    {
        nlohmann::json v = good;
        v["entries"][0]["names"] = {"zz", "aa"}; // names not sorted
        add_variant("names_unsorted", v);
    }
    {
        nlohmann::json v = good;
        v["modules"].erase(1); // incomplete module manifest
        add_variant("modules_missing_one", v);
    }
    {
        nlohmann::json v = good;
        std::swap(v["modules"][0], v["modules"][1]); // modules not sorted by role
        add_variant("modules_unsorted", v);
    }
    {
        nlohmann::json v = good;
        v["modules"][0]["guards"] = v["modules"][0]["guards"].get<std::uint64_t>() + 1;
        add_variant("guards_count_wrong", v);
    }
    {
        nlohmann::json v = good;
        v["entries"][0]["bytes"] = "abc"; // odd-length hex
        add_variant("bytes_odd_length", v);
    }
    {
        nlohmann::json v = good;
        v["entries"][0]["bytes"] = "AB"; // uppercase hex is not accepted
        add_variant("bytes_uppercase", v);
    }
    {
        nlohmann::json v = good;
        v["entries"][0].erase("output"); // missing required key
        add_variant("missing_output_key", v);
    }
    {
        nlohmann::json v = good;
        v["extra"] = 1; // unexpected top-level key
        add_variant("extra_top_level_key", v);
    }
    {
        nlohmann::json v = good;
        v["entries"] = nlohmann::json::array(); // an empty corpus cannot be a snapshot
        add_variant("empty_entries", v);
    }
    {
        nlohmann::json v = good;
        v["modules"] = nlohmann::json::array();
        add_variant("empty_modules", v);
    }
    {
        // coverage elements must be sorted by module and guard index
        nlohmann::json v = good;
        for (auto& entry : v["entries"])
        {
            if (entry["coverage"].size() > 1)
            {
                std::reverse(entry["coverage"].begin(), entry["coverage"].end());
            }
        }
        add_variant("coverage_unsorted", v);
    }

    for (const auto& variant : variants)
    {
        const std::string variant_path = g.dir + "/variant-" + variant.first + ".json";
        write_file(variant_path, variant.second.dump(2));
        g.snapshot = variant_path;
        g.report = g.dir + "/report-" + variant.first + ".json";
        write_file(g.report, "sentinel");
        // any invalid snapshot is rejected before conversion or execution,
        // and the previous report stays untouched
        TASK_CHECK(run_child(child_verify) == 2);
        TASK_CHECK(read_text(g.report) == "sentinel");
        const auto stats = read_stats(g.stats);
        TASK_CHECK(stats.first == 0);
        TASK_CHECK(stats.second == 0);
    }

    // a mismatched source identity on the command line refuses the good
    // snapshot as well, again without running the corpus
    g.snapshot = g.dir + "/snapshot.json";
    g.report = g.dir + "/report-source.json";
    g.source = kOtherSource;
    write_file(g.report, "sentinel");
    TASK_CHECK(run_child(child_verify) == 2);
    TASK_CHECK(read_text(g.report) == "sentinel");
    // and the same snapshot replays cleanly with the right identity
    g.source = kSource;
    TASK_CHECK(run_child(child_verify) == 0);
    return 0;
}

int archive_manifest_identity_mismatch()
{
    reset_scenario("manifest-mismatch");
    make_corpus(g.corpus, {{"even", std::string(1, static_cast<char>(2))}});
    g.bind_b = false; // record with only mod_a bound
    TASK_CHECK(run_child(child_record) == 0);
    g.bind_b = true;
    // an additionally bound module changes the full manifest
    write_file(g.report, "sentinel");
    TASK_CHECK(run_child(child_verify) == 2);
    TASK_CHECK(read_text(g.report) == "sentinel");
    // a different build identity is rejected
    g.bind_b = false;
    g.build_a = kBuildC;
    write_file(g.report, "sentinel");
    TASK_CHECK(run_child(child_verify) == 2);
    TASK_CHECK(read_text(g.report) == "sentinel");
    // a different layout identity is rejected
    g.build_a = kBuildA;
    g.layout_a = kLayoutC;
    write_file(g.report, "sentinel");
    TASK_CHECK(run_child(child_verify) == 2);
    TASK_CHECK(read_text(g.report) == "sentinel");
    // a renamed role is rejected
    g.layout_a = kLayoutA;
    g.role_a = "renamed";
    write_file(g.report, "sentinel");
    TASK_CHECK(run_child(child_verify) == 2);
    TASK_CHECK(read_text(g.report) == "sentinel");
    // the matching manifest replays cleanly
    g.role_a = kRoleA;
    TASK_CHECK(run_child(child_verify) == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// replay group
// ---------------------------------------------------------------------------

int replay_clean_replay_across_processes_reverse_load()
{
    reset_scenario("clean-replay");
    make_corpus(g.corpus, {{"p1", std::string(1, static_cast<char>(1))},
                           {"p2", std::string(1, static_cast<char>(2))},
                           {"p3", std::string(1, static_cast<char>(200))},
                           {"p4", std::string(1, static_cast<char>(201))}});
    g.reverse_load = false;
    TASK_CHECK(run_child(child_record) == 0);
    // another process, modules loaded in the opposite order
    g.reverse_load = true;
    TASK_CHECK(run_child(child_verify) == 0);
    const nlohmann::json report = nlohmann::json::parse(read_text(g.report), nullptr, false);
    TASK_CHECK(!report.is_discarded());
    TASK_CHECK(report.at("format") == "fuzzcover-comparison-1");
    TASK_CHECK(report.at("source") == kSource);
    TASK_CHECK(report.at("ok") == true);
    TASK_CHECK(report.at("entries").size() == 4);
    std::string previous;
    for (const auto& entry : report.at("entries"))
    {
        const std::string sha256 = entry.at("sha256").get<std::string>();
        TASK_CHECK(previous.empty() || previous < sha256); // sorted by digest
        previous = sha256;
        TASK_CHECK(entry.at("input_changed") == false);
        TASK_CHECK(entry.at("output_changed") == false);
        TASK_CHECK(entry.at("lost").empty());
        TASK_CHECK(entry.at("added").empty());
    }
    // a third process, original load order, same result
    g.reverse_load = false;
    TASK_CHECK(run_child(child_verify) == 0);
    return 0;
}

int replay_value_changes_reported()
{
    reset_scenario("value-changes");
    make_corpus(g.corpus, {{"one", std::string(1, static_cast<char>(1))},
                           {"two", std::string(1, static_cast<char>(2))}});
    TASK_CHECK(run_child(child_record) == 0);
    // changed normal results are reported per entry
    g.output_shift = 1;
    TASK_CHECK(run_child(child_verify) == 1);
    nlohmann::json report = nlohmann::json::parse(read_text(g.report), nullptr, false);
    TASK_CHECK(!report.is_discarded());
    TASK_CHECK(report.at("ok") == false);
    for (const auto& entry : report.at("entries"))
    {
        TASK_CHECK(entry.at("input_changed") == false);
        TASK_CHECK(entry.at("output_changed") == true);
        TASK_CHECK(entry.at("lost").empty());
    }
    g.output_shift = 0;
    // changed conversion results are reported independently
    g.input_shift = 1;
    TASK_CHECK(run_child(child_verify) == 1);
    report = nlohmann::json::parse(read_text(g.report), nullptr, false);
    TASK_CHECK(!report.is_discarded());
    TASK_CHECK(report.at("ok") == false);
    for (const auto& entry : report.at("entries"))
    {
        TASK_CHECK(entry.at("input_changed") == true);
    }
    g.input_shift = 0;
    // the unchanged build replays cleanly again
    TASK_CHECK(run_child(child_verify) == 0);
    return 0;
}

int replay_coverage_loss_and_gain()
{
    reset_scenario("coverage-loss-gain");
    make_corpus(g.corpus, {{"odd", std::string(1, static_cast<char>(1))},
                           {"even", std::string(1, static_cast<char>(2))}});
    g.cover_b = true;
    TASK_CHECK(run_child(child_record) == 0);
    const std::string snapshot_before = read_text(g.snapshot);
    const std::string odd_sha256 = fuzzcover::detail::sha256_hex(std::vector<std::uint8_t>{1});
    // replay without the mod_b calls: coverage is lost, values are unchanged
    g.cover_b = false;
    TASK_CHECK(run_child(child_verify) == 1);
    nlohmann::json report = nlohmann::json::parse(read_text(g.report), nullptr, false);
    TASK_CHECK(!report.is_discarded());
    TASK_CHECK(report.at("ok") == false);
    for (const auto& entry : report.at("entries"))
    {
        TASK_CHECK(entry.at("input_changed") == false);
        TASK_CHECK(entry.at("output_changed") == false);
        TASK_CHECK(entry.at("added").empty());
        if (entry.at("sha256") == odd_sha256)
        {
            TASK_CHECK(!entry.at("lost").empty()); // the mod_b guards are gone
            TASK_CHECK(entry.at("lost")[0].at("module") == kRoleB);
        }
        else
        {
            TASK_CHECK(entry.at("lost").empty());
        }
    }
    // the failed comparison never rewrote the snapshot
    TASK_CHECK(read_text(g.snapshot) == snapshot_before);
    // and the lost coverage did not silently become the new baseline
    g.cover_b = true;
    TASK_CHECK(run_child(child_verify) == 0);
    // the other direction: only new coverage appears, still a clean replay
    g.cover_b = false;
    TASK_CHECK(run_child(child_record) == 0);
    g.cover_b = true;
    TASK_CHECK(run_child(child_verify) == 0);
    report = nlohmann::json::parse(read_text(g.report), nullptr, false);
    TASK_CHECK(!report.is_discarded());
    TASK_CHECK(report.at("ok") == true);
    bool saw_added = false;
    for (const auto& entry : report.at("entries"))
    {
        TASK_CHECK(entry.at("input_changed") == false);
        TASK_CHECK(entry.at("output_changed") == false);
        TASK_CHECK(entry.at("lost").empty());
        if (entry.at("sha256") == odd_sha256)
        {
            TASK_CHECK(!entry.at("added").empty());
            saw_added = true;
        }
    }
    TASK_CHECK(saw_added);
    return 0;
}

int replay_execution_exception()
{
    reset_scenario("execution-exception");
    make_corpus(g.corpus, {{"one", std::string(1, static_cast<char>(1))}});
    // a throwing test function fails the snapshot and closes the collection
    // scope: binding another module right after must still work (child
    // returns 13 if the scope leaked)
    g.throw_on_execution = true;
    g.bind_b = false;
    g.bind_b_after_record = true;
    TASK_CHECK(run_child(child_record) == 2);
    TASK_CHECK(!file_exists(g.snapshot));
    g.throw_on_execution = false;
    g.bind_b = true;
    g.bind_b_after_record = false;
    TASK_CHECK(run_child(child_record) == 0);
    // a throwing replay fails with 2 and keeps the previous report
    g.throw_on_execution = true;
    write_file(g.report, "sentinel");
    TASK_CHECK(run_child(child_verify) == 2);
    TASK_CHECK(read_text(g.report) == "sentinel");
    g.throw_on_execution = false;
    TASK_CHECK(run_child(child_verify) == 0);
    return 0;
}
} // namespace

const struct task_case task_cases[] = {
    {"capture", "guard_hits_and_item_isolation", capture_guard_hits_and_item_isolation},
    {"capture", "duplicate_bytes_and_shared_prefix_names", capture_duplicate_bytes_and_shared_prefix_names},
    {"capture", "unbound_module_fails", capture_unbound_module_fails},
    {"capture", "binding_rules", capture_binding_rules},
    {"capture", "corpus_and_source_rejections", capture_corpus_and_source_rejections},
    {"archive", "snapshot_structure", archive_snapshot_structure},
    {"archive", "corrupt_or_wrong_version_rejected", archive_corrupt_or_wrong_version_rejected},
    {"archive", "manifest_identity_mismatch", archive_manifest_identity_mismatch},
    {"replay", "clean_replay_across_processes_reverse_load", replay_clean_replay_across_processes_reverse_load},
    {"replay", "value_changes_reported", replay_value_changes_reported},
    {"replay", "coverage_loss_and_gain", replay_coverage_loss_and_gain},
    {"replay", "execution_exception", replay_execution_exception},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
