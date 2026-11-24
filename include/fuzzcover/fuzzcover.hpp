#pragma once

#include "FuzzedDataProvider.h" // (convenience to clients)
#include "coverage.hpp"         // coverage snapshots (convenience to clients)
#include <algorithm>            // sort, set_difference
#include <cstddef>              // size_t
#include <cstdint>              // uint8_t
#include <cstdlib>              // EXIT_SUCCESS, EXIT_FAILURE, exit
#include <dirent.h>             // readdir, opendir
#include <fstream>              // istream_iterator
#include <iostream>             // cerr, endl
#include <iterator>             // back_inserter
#include <map>                  // map
#include <string>               // string
#include <vector>               // vector
#include <nlohmann/json.hpp>    // nlohmann::json

#ifndef FUZZCOVER_NODOCTEST
#define DOCTEST_CONFIG_IMPLEMENT
#define DOCTEST_CONFIG_SUPER_FAST_ASSERTS
#include <doctest/doctest.h> // TEST_CASE, CAPTURE, CHECK_EQ, CHECK_NOTHROW, doctest::Context
#else
// switch off doctest macros
#define TEST_CASE(f) void bogus_function1() { #f; } void bogus_function2()
#define CAPTURE(x)
#define CHECK_EQ(x)
#endif

#ifndef FUZZCOVER_NOFUZZER
// entry point for libfuzzer
extern "C" int LLVMFuzzerRunDriver(int* argc, char*** argv, int (*UserCb)(const uint8_t* Data, size_t Size));
#endif

namespace fuzzcover {

// function to be defined via MAKE_MAIN later to glue the client code with fuzzcover
int fuzz_wrapper(const std::uint8_t* data, std::size_t size);

// function to be defined via MAKE_MAIN later to glue doctest with fuzzcover
void doctest_wrapper();

/*!
 * @brief interface for fuzzcover
 * @tparam TestInput type of the test input
 */
template <class TestInput, class TestOutput = bool>
class fuzzcover_interface
{
  public:
    using test_input_t = TestInput;
    using test_output_t = TestOutput;

    ///////////////////////////////////////////////////////////////////////////

    /*!
     * @brief create a test input from some bytes
     * @param[in] data input bytes
     * @param[in] size number of bytes in @a data
     * @return test input
     */
    virtual test_input_t value_from_bytes(const std::uint8_t* data, std::size_t size) = 0;

    /*!
     * @brief execute a test with a given input
     * @param[in] value test input
     * @return test output (unless test_output_t is void)
     */
    virtual test_output_t test_function(const test_input_t& value) = 0;

    ///////////////////////////////////////////////////////////////////////////

    virtual ~fuzzcover_interface() = default;

    /*!
     * @brief function to call from libfuzzer
     * @param[in] data input bytes
     * @param[in] size number of bytes in @a data
     */
    void fuzz(const std::uint8_t* data, std::size_t size)
    {
        test_function(value_from_bytes(data, size));
    }

    /*!
     * @brief handle the command line arguments
     * @param argc the number of arguments
     * @param argv the arguments
     * @return 0 in case of success, any other value otherwise
     */
    int handle_arguments(int argc, char** argv)
    {
        if (argc >= 2)
        {
            std::string current = argv[1];

#ifndef FUZZCOVER_NOFUZZER
            if (current == "--fuzz")
            {
                return LLVMFuzzerRunDriver(&argc, &argv, fuzz_wrapper);
            }
#endif

            if (current == "--test" && argc >= 3)
            {
                test(get_files(argv[2]));
                return EXIT_SUCCESS;
            }

#ifndef FUZZCOVER_NODOCTEST
            if (current == "--check")
            {
                // if no argument is given or it is a doctest option, default the filename
                if (argc == 2 || (argc >= 3 && std::strlen(argv[2]) > 0 && argv[2][0] == '-'))
                {
                    tests = nlohmann::json::parse(std::ifstream(std::string(argv[0]) + ".json"));
                }
                else
                {
                    tests = nlohmann::json::parse(std::ifstream(argv[2]));
                }
                doctest::Context context;
                context.applyCommandLine(argc, argv);
                return context.run();
            }
#endif

            if (current == "--dump" && argc >= 3)
            {
                if (argc == 3)
                {
                    dump(get_files(argv[2]), std::cout);
                }
                else
                {
                    std::ofstream outfile(argv[3]);
                    dump(get_files(argv[2]), outfile);
                }
                return EXIT_SUCCESS;
            }

            if (current == "--coverage-snapshot" && argc == 5)
            {
                return record_snapshot(argv[2], argv[3], argv[4]);
            }

            if (current == "--coverage-compare" && argc == 5)
            {
                return verify_snapshot(argv[2], argv[3], argv[4]);
            }

            if (current == "--help")
            {
                std::cerr << "usage: " << argv[0] << " ARGUMENTS\n\n";
                std::cerr << "Fuzzcover - test suite generation for C++\n\n"
                          << "arguments:\n"
                             "  --help                                     show this help message and exit\n"
#ifndef FUZZCOVER_NOFUZZER
                             "  --fuzz [LIBFUZZER_OPTION...]               perform fuzzing\n"
#endif
                             "  --dump CORPUS_DIRECTORY [CORPUS_FILE]      dump the corpus files as JSON\n"
                             "  --test CORPUS_DIRECTORY                    run the test function on the corpus\n"
                             "  --coverage-snapshot CORPUS SOURCE OUTPUT   record a per-corpus coverage snapshot\n"
                             "  --coverage-compare SNAPSHOT SOURCE REPORT  replay a snapshot and write a comparison report\n"
#ifndef FUZZCOVER_NODOCTEST
                             "  --check [CORPUS_FILE] [DOCTEST_OPTION...]  execute test suite\n"
#endif
                             "\n"
                             "  CORPUS_DIRECTORY  a corpus directory\n"
                             "  CORPUS_FILE       a corpus file in JSON format as created by --dump\n"
                             "  SOURCE            a 40-digit lowercase hex identifier of the source being run\n"
                             "  OUTPUT            snapshot file to write (format fuzzcover-regression-1)\n"
                             "  SNAPSHOT          snapshot file to replay (format fuzzcover-regression-1)\n"
                             "  REPORT            comparison report to write (format fuzzcover-comparison-1)\n"
#ifndef FUZZCOVER_NOFUZZER
                             "  LIBFUZZER_OPTION  an option for LibFuzzer (e.g., '-help=1')\n"
#endif
#ifndef FUZZCOVER_NODOCTEST
                             "  DOCTEST_OPTION    an option for doctest (e.g., '--help')"
#endif
                          << std::endl;
                return EXIT_SUCCESS;
            }
        }

        std::cerr << "Fuzzcover: unknown or missing argument; call '" << argv[0] << " --help' for more information." << std::endl;
        return EXIT_FAILURE;
    }

    // variable to store parsed tests for the --check option
    static nlohmann::json tests;

    void check()
    {
        for (const auto& entry : tests)
        {
            test_input_t input = entry.at("input");
            test_output_t output = entry.at("output");
            CAPTURE(entry.at("input"));
            CAPTURE(entry.at("output"));
            CAPTURE(entry.at("hash"));
            CHECK_EQ(test_function(input), output);
        }
    }

    /*!
     * @brief record a per-corpus coverage snapshot
     *
     * Every distinct byte sequence in the corpus is converted and executed
     * exactly once, using the bytes of a single read; files with identical
     * bytes share one entry. Coverage is collected per item while the test
     * function runs. The snapshot is written atomically; on any failure the
     * previous file is kept.
     *
     * @param corpus directory with the corpus files (regular files only)
     * @param source 40-digit lowercase hex source identifier
     * @param output snapshot file to write
     * @return 0 on success, 2 on any failure
     */
    int record_snapshot(const std::string& corpus, const std::string& source, const std::string& output)
    {
        if (!detail::is_lower_hex(source, 40))
        {
            return 2; // unknown source identity: refuse to run the corpus
        }
        const std::vector<CoverageModuleManifest> manifests = BoundCoverageModules();
        if (manifests.empty())
        {
            return 2; // no valid bound module: no successful snapshot
        }
        const auto files = detail::list_corpus_files(corpus);
        if (!files || files->empty())
        {
            return 2; // unreadable directory, non-regular entry, or empty corpus
        }
        // group files by their exact bytes; each file is read exactly once
        std::map<std::vector<std::uint8_t>, std::vector<std::string>> groups;
        for (const auto& file : *files)
        {
            auto bytes = detail::read_file_bytes(file.first);
            if (!bytes)
            {
                return 2;
            }
            groups[*bytes].push_back(file.second);
        }
        struct RecordedEntry
        {
            std::string sha256;
            std::vector<std::uint8_t> bytes;
            std::vector<std::string> names;
            nlohmann::json input;
            nlohmann::json output;
            std::vector<CoverageHit> coverage;
        };
        std::vector<RecordedEntry> entries;
        for (const auto& group : groups)
        {
            const std::vector<std::uint8_t>& bytes = group.first;
            RecordedEntry entry;
            entry.sha256 = detail::sha256_hex(bytes);
            entry.bytes = bytes;
            entry.names = group.second;
            std::sort(entry.names.begin(), entry.names.end());
            entry.names.erase(std::unique(entry.names.begin(), entry.names.end()), entry.names.end());
            try
            {
                const test_input_t input = value_from_bytes(bytes.data(), bytes.size());
                entry.input = nlohmann::json(input);
                test_output_t test_output{};
                bool scope_ok = false;
                {
                    CoverageScope scope;
                    test_output = test_function(input);
                    scope_ok = scope.ok();
                    entry.coverage.assign(scope.hits().begin(), scope.hits().end());
                }
                if (!scope_ok)
                {
                    return 2; // an unbound module was hit: fail loudly
                }
                entry.output = nlohmann::json(test_output);
            }
            catch (...)
            {
                return 2; // conversion or execution failed
            }
            entries.push_back(std::move(entry));
        }
        std::sort(entries.begin(), entries.end(), [](const RecordedEntry& lhs, const RecordedEntry& rhs)
        {
            return lhs.sha256 < rhs.sha256;
        });
        nlohmann::json snapshot;
        snapshot["format"] = "fuzzcover-regression-1";
        snapshot["source"] = source;
        snapshot["modules"] = detail::manifests_to_json(manifests);
        snapshot["entries"] = nlohmann::json::array();
        for (const auto& entry : entries)
        {
            snapshot["entries"].push_back(nlohmann::json{{"sha256", entry.sha256},
                                                         {"bytes", detail::hex_encode(entry.bytes.data(), entry.bytes.size())},
                                                         {"names", entry.names},
                                                         {"input", entry.input},
                                                         {"output", entry.output},
                                                         {"coverage", detail::coverage_hits_to_json(entry.coverage)}});
        }
        if (!detail::write_file_atomic(output, snapshot.dump(2) + "\n"))
        {
            return 2;
        }
        return 0;
    }

    /*!
     * @brief replay a coverage snapshot and write a comparison report
     *
     * The snapshot is fully validated first (structure, ordering, raw
     * digests, coverage indices, source and module manifest identity); if
     * anything is invalid, nothing is executed and an existing report is left
     * untouched. Each entry is then re-converted from its archived bytes and
     * re-run; inputs, outputs, and coverage sets are compared separately.
     *
     * @param snapshot snapshot file to replay
     * @param source 40-digit lowercase hex source identifier
     * @param report comparison report to write
     * @return 0 if everything matches or only new coverage appeared, 1 if
     *         values changed or coverage was lost, 2 on format, identity,
     *         I/O, or execution failures
     */
    int verify_snapshot(const std::string& snapshot, const std::string& source, const std::string& report)
    {
        if (!detail::is_lower_hex(source, 40))
        {
            return 2; // unknown source identity: refuse to run the corpus
        }
        const auto text = detail::read_file_text(snapshot);
        if (!text)
        {
            return 2;
        }
        const auto parsed = detail::validate_snapshot_text(*text, source);
        if (!parsed)
        {
            return 2; // invalid snapshot: do not execute anything
        }
        if (!(parsed->modules == BoundCoverageModules()))
        {
            return 2; // the bound modules do not match the archived manifest
        }
        nlohmann::json report_entries = nlohmann::json::array();
        bool any_difference = false;
        for (const auto& entry : parsed->entries)
        {
            nlohmann::json input_json;
            nlohmann::json output_json;
            std::vector<CoverageHit> hits;
            try
            {
                // re-convert from the archived bytes; the archived input is
                // never fed to the test function directly
                const test_input_t input = value_from_bytes(entry.bytes.data(), entry.bytes.size());
                input_json = nlohmann::json(input);
                test_output_t test_output{};
                bool scope_ok = false;
                {
                    CoverageScope scope;
                    test_output = test_function(input);
                    scope_ok = scope.ok();
                    hits.assign(scope.hits().begin(), scope.hits().end());
                }
                if (!scope_ok)
                {
                    return 2; // an unbound module was hit: fail loudly
                }
                output_json = nlohmann::json(test_output);
            }
            catch (...)
            {
                return 2; // conversion or execution failed
            }
            const bool input_changed = (input_json != entry.input);
            const bool output_changed = (output_json != entry.output);
            std::vector<CoverageHit> lost;
            std::vector<CoverageHit> added;
            std::set_difference(entry.coverage.begin(), entry.coverage.end(), hits.begin(), hits.end(), std::back_inserter(lost));
            std::set_difference(hits.begin(), hits.end(), entry.coverage.begin(), entry.coverage.end(), std::back_inserter(added));
            any_difference = any_difference || input_changed || output_changed || !lost.empty();
            report_entries.push_back(nlohmann::json{{"sha256", entry.sha256},
                                                    {"input_changed", input_changed},
                                                    {"output_changed", output_changed},
                                                    {"lost", detail::coverage_hits_to_json(lost)},
                                                    {"added", detail::coverage_hits_to_json(added)}});
        }
        nlohmann::json report_json;
        report_json["format"] = "fuzzcover-comparison-1";
        report_json["source"] = source;
        report_json["ok"] = !any_difference;
        report_json["entries"] = std::move(report_entries);
        if (!detail::write_file_atomic(report, report_json.dump(2) + "\n"))
        {
            return 2;
        }
        return any_difference ? 1 : 0;
    }

  private:
    /*!
     * @brief execute the test function for the corpus
     * @param[in] filenames names of the files to read from
     */
    void test(const std::vector<std::string>& filenames)
    {
        for (const auto& filename : filenames)
        {
            static_cast<void>(test_function(value_from_file(filename)));
        }
    }

    /*!
     * @brief dump the content of the corpus
     * @param[in] filenames names of the files to read from
     * @param[in] os stream to dump the corpus to
     */
    void dump(const std::vector<std::string>& filenames, std::ostream& os)
    {
        auto file_count = filenames.size();
        os << "[\n";

        for (const auto& filename : filenames)
        {
            nlohmann::json input = value_from_file(filename);
            nlohmann::json output = test_function(value_from_file(filename));
            nlohmann::json tuple = {{"input", std::move(input)}, {"output", std::move(output)}, {"hash", short_hash(filename)}};
            os << "  " << tuple.dump(-1, ' ', false, nlohmann::json::error_handler_t::ignore);
            if (--file_count != 0)
            {
                os << ",\n";
            }
            else
            {
                os << "\n";
            }
        }
        os << "]" << std::endl;
    }

    /*!
     * @brief abbreviate a file name by a 7-digit hash just as short git commit hashes
     * @param filename filename to abbreviate
     * @return first 7 characters of the basename of @a filename
     */
    static std::string short_hash(const std::string& filename)
    {
        auto slash = filename.find_last_of('/');
        slash = (slash == std::string::npos) ? 0 : slash + 1;
        return filename.substr(slash, 7);
    }

    /*!
     * @brief collect all files names in a given directory
     * @param directory directory to read
     * @return list of file names
     */
    std::vector<std::string> get_files(const char* directory)
    {
        std::vector<std::string> result;

        struct dirent* dir;
        DIR* d = opendir(directory);
        if (d != nullptr)
        {
            while ((dir = readdir(d)) != nullptr)
            {
                if (dir->d_type == DT_REG)
                {
                    result.push_back(std::string(directory) + "/" + dir->d_name);
                }
            }
            closedir(d);
        }

        return result;
    }

    /*!
     * @brief read input from a file (usually from the corpus)
     * @param[in] filename name of the file to read from
     * @return test input
     */
    test_input_t value_from_file(const std::string& filename)
    {
        std::ifstream file(filename, std::ios::binary);
        if (!file)
        {
            std::cerr << "Cannot open file '" << filename << "', aborting." << std::endl;
            std::exit(EXIT_FAILURE);
        }

        file.unsetf(std::ios::skipws);

        file.seekg(0, std::ios::end);
        const auto file_size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<std::uint8_t> bytes;
        bytes.reserve(static_cast<std::size_t>(file_size));
        bytes.insert(bytes.begin(), std::istream_iterator<std::uint8_t>(file),
                     std::istream_iterator<std::uint8_t>());

        return value_from_bytes(bytes.data(), bytes.size());
    }
};

template <class TestInput, class TestOutput>
nlohmann::json fuzzcover_interface<TestInput, TestOutput>::tests;

} // namespace fuzzcover

#define MAKE_MAIN(CLASS_NAME)                                    \
    namespace fuzzcover {                                        \
    int fuzz_wrapper(const std::uint8_t* data, std::size_t size) \
    {                                                            \
        CLASS_NAME instance;                                     \
        instance.fuzz(data, size);                               \
        return 0;                                                \
    }                                                            \
                                                                 \
    void doctest_wrapper()                                       \
    {                                                            \
        CLASS_NAME instance;                                     \
        instance.check();                                        \
    }                                                            \
    }                                                            \
                                                                 \
    TEST_CASE(#CLASS_NAME)                                       \
    {                                                            \
        fuzzcover::doctest_wrapper();                            \
    }                                                            \
                                                                 \
    int main(int argc, char** argv)                              \
    {                                                            \
        CLASS_NAME instance;                                     \
        return instance.handle_arguments(argc, argv);            \
    }

// clean up
#ifdef DOCTEST_CONFIG_IMPLEMENT
#undef DOCTEST_CONFIG_IMPLEMENT
#endif
#ifdef DOCTEST_CONFIG_SUPER_FAST_ASSERTS
#undef DOCTEST_CONFIG_SUPER_FAST_ASSERTS
#endif
