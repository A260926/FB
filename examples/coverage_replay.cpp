// Example: per-corpus coverage snapshots across independent processes.
//
//   coverage_replay OUTPUT_DIRECTORY
//
// The parent process creates a small benign corpus, then spawns two child
// processes: one records a coverage snapshot of the corpus, the other
// replays it (with the probe modules loaded in the opposite order) and
// writes a comparison report. The module identities, corpus digests, and
// comparison results are collected in OUTPUT_DIRECTORY/transcript.txt.
//
// Hidden child modes (used by the parent through re-exec):
//   coverage_replay --record CORPUS SOURCE SNAPSHOT
//   coverage_replay --verify SNAPSHOT SOURCE REPORT

#define FUZZCOVER_NOFUZZER
#include <fuzzcover/fuzzcover.hpp>

#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace
{
const char* kModuleA = ".task-build/mod-a/probe.so";
const char* kModuleB = ".task-build/mod-b/probe.so";

// source and module identities as a trusted build manifest would provide them
const std::string kSource = "0123456789abcdef0123456789abcdef01234567";
const std::string kBuildA(64, 'a');
const std::string kLayoutA(64, 'b');
const std::string kBuildB(64, 'c');
const std::string kLayoutB(64, 'd');

using covered_value_fn = int (*)(int);

class ReplayFixture : public fuzzcover::fuzzcover_interface<unsigned, int>
{
  public:
    covered_value_fn first = nullptr;
    covered_value_fn second = nullptr;

    unsigned value_from_bytes(const std::uint8_t* data, std::size_t size) override
    {
        unsigned value = 0;
        for (std::size_t i = 0; i < size; ++i)
        {
            value += data[i];
        }
        return value;
    }

    int test_function(const unsigned& value) override
    {
        int result = first(static_cast<int>(value));
        if (value % 2 != 0)
        {
            result += second(static_cast<int>(value));
        }
        return result;
    }
};

// load both probe modules and bind them to their manifest identities; when
// reversed is set, mod-b is loaded before mod-a to show that load order does
// not change module identity or guard indices
bool load_and_bind(ReplayFixture& fixture, bool reversed)
{
    const char* first_path = reversed ? kModuleB : kModuleA;
    const char* second_path = reversed ? kModuleA : kModuleB;
    void* first = dlopen(first_path, RTLD_NOW | RTLD_LOCAL);
    void* second = dlopen(second_path, RTLD_NOW | RTLD_LOCAL);
    if (first == nullptr || second == nullptr)
    {
        std::cerr << "coverage_replay: cannot load probe modules" << std::endl;
        return false;
    }
    void* handle_a = reversed ? second : first;
    void* handle_b = reversed ? first : second;
    fixture.first = reinterpret_cast<covered_value_fn>(dlsym(handle_a, "covered_value"));
    fixture.second = reinterpret_cast<covered_value_fn>(dlsym(handle_b, "covered_value"));
    if (fixture.first == nullptr || fixture.second == nullptr)
    {
        std::cerr << "coverage_replay: cannot resolve covered_value" << std::endl;
        return false;
    }
    const void* anchor_a = reinterpret_cast<const void*>(fixture.first);
    const void* anchor_b = reinterpret_cast<const void*>(fixture.second);
    return fuzzcover::BindCoverageModule(anchor_a, "arith_a", kBuildA, kLayoutA) &&
           fuzzcover::BindCoverageModule(anchor_b, "arith_b", kBuildB, kLayoutB);
}

int child_record(const std::string& corpus, const std::string& source, const std::string& snapshot)
{
    ReplayFixture fixture;
    if (!load_and_bind(fixture, false))
    {
        return 2;
    }
    return fixture.record_snapshot(corpus, source, snapshot);
}

int child_verify(const std::string& snapshot, const std::string& source, const std::string& report)
{
    ReplayFixture fixture;
    if (!load_and_bind(fixture, true)) // reversed load order on purpose
    {
        return 2;
    }
    return fixture.verify_snapshot(snapshot, source, report);
}

int spawn(const std::string& self, const std::vector<std::string>& arguments)
{
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(self.c_str()));
    for (const auto& argument : arguments)
    {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0)
    {
        return -1;
    }
    if (pid == 0)
    {
        execv(self.c_str(), argv.data());
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
    {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string self_path(const char* argv0)
{
    std::vector<char> buffer(4096, '\0');
    if (realpath(argv0, buffer.data()) != nullptr)
    {
        return std::string(buffer.data());
    }
    return argv0;
}

void write_corpus(const std::string& directory)
{
    std::filesystem::create_directories(directory);
    const std::vector<std::pair<std::string, std::string>> files = {
        {"alpha", std::string(1, static_cast<char>(3))},
        {"beta", std::string(1, static_cast<char>(120))},
        {"gamma", std::string(1, static_cast<char>(65))},
        {"delta", std::string(1, static_cast<char>(3))}, // same bytes as alpha
        {"empty", ""},
    };
    for (const auto& file : files)
    {
        std::ofstream out(directory + "/" + file.first, std::ios::binary | std::ios::trunc);
        out << file.second;
    }
}

std::string join_names(const nlohmann::json& names)
{
    std::string out;
    for (const auto& name : names)
    {
        if (!out.empty())
        {
            out += ",";
        }
        out += name.get<std::string>();
    }
    return out;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 5 && std::string(argv[1]) == "--record")
    {
        return child_record(argv[2], argv[3], argv[4]);
    }
    if (argc == 5 && std::string(argv[1]) == "--verify")
    {
        return child_verify(argv[2], argv[3], argv[4]);
    }
    if (argc != 2)
    {
        std::cerr << "usage: " << argv[0] << " OUTPUT_DIRECTORY" << std::endl;
        return 2;
    }

    const std::string output_directory = argv[1];
    const std::string corpus = output_directory + "/corpus";
    const std::string snapshot = output_directory + "/snapshot.json";
    const std::string report = output_directory + "/report.json";
    const std::string transcript = output_directory + "/transcript.txt";
    std::error_code error;
    std::filesystem::create_directories(output_directory, error);
    if (error)
    {
        std::cerr << "coverage_replay: cannot create " << output_directory << std::endl;
        return 2;
    }
    write_corpus(corpus);

    const std::string self = self_path(argv[0]);
    const int record_status = spawn(self, {"--record", corpus, kSource, snapshot});
    const int verify_status = spawn(self, {"--verify", snapshot, kSource, report});

    nlohmann::json snapshot_json;
    nlohmann::json report_json;
    {
        std::ifstream snapshot_file(snapshot);
        std::ifstream report_file(report);
        if (snapshot_file)
        {
            snapshot_file >> snapshot_json;
        }
        if (report_file)
        {
            report_file >> report_json;
        }
    }

    std::ofstream out(transcript, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        std::cerr << "coverage_replay: cannot write " << transcript << std::endl;
        return 2;
    }
    out << "source " << kSource << "\n";
    if (snapshot_json.is_object())
    {
        for (const auto& module : snapshot_json.at("modules"))
        {
            out << "module " << module.at("role").get<std::string>() << " build=" << module.at("build").get<std::string>()
                << " layout=" << module.at("layout").get<std::string>() << " guards=" << module.at("guards").get<std::uint64_t>() << "\n";
        }
        for (const auto& entry : snapshot_json.at("entries"))
        {
            out << "corpus_entry sha256=" << entry.at("sha256").get<std::string>() << " names=" << join_names(entry.at("names"))
                << " input=" << entry.at("input").dump() << " output=" << entry.at("output").dump() << "\n";
        }
    }
    out << "record_exit " << record_status << "\n";
    out << "verify_exit " << verify_status << "\n";
    out << "verify_load_order mod-b,mod-a\n";
    if (report_json.is_object())
    {
        out << "compare ok=" << (report_json.at("ok").get<bool>() ? "true" : "false") << " entries=" << report_json.at("entries").size() << "\n";
        for (const auto& entry : report_json.at("entries"))
        {
            out << "entry " << entry.at("sha256").get<std::string>() << " input_changed=" << (entry.at("input_changed").get<bool>() ? "true" : "false")
                << " output_changed=" << (entry.at("output_changed").get<bool>() ? "true" : "false") << " lost=" << entry.at("lost").size()
                << " added=" << entry.at("added").size() << "\n";
        }
    }
    out.close();

    const bool ok = record_status == 0 && verify_status == 0 && report_json.is_object() && report_json.at("ok").get<bool>();
    return ok ? 0 : 1;
}
