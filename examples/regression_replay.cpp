/* **********************************************************
 * Copyright (c) 2026 Google, Inc.  All rights reserved.
 * **********************************************************
 *
 * Dr. Memory: the memory debugger
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License, and no later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/* Offline regression replay demonstration.
 *
 * Invocation: regression_replay OUTPUT_DIR
 *
 * The driver launches two independent child processes (re-executing this
 * same binary): the first saves a candidate whose rerun slots are not yet
 * all filled; the second restores that unfinished state, declares the
 * module image late, completes the remaining observation and records the
 * final report plus a transcript in OUTPUT_DIR.  Nothing executes a target
 * program; all data is simulated.
 */

#include "drmemory/regression.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace drreg;

namespace {

const char *kStateName = "replay.state";
const char *kReportName = "report.txt";
const char *kTranscriptName = "transcript.txt";

/* Run one child stage via fork/execv (argv form, so spaces in the output
 * directory are passed through untouched).  Returns the child exit code, or
 * -1 if it could not be launched or did not exit normally. */
int RunStage(const char *self_path, const std::string &dir,
             const char *stage)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        char *const argv[] = {const_cast<char *>(self_path),
                              const_cast<char *>(dir.c_str()),
                              const_cast<char *>(stage), nullptr};
        execv(self_path, argv);
        _exit(127); /* only reached if exec fails */
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    if (!WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

int StageOne(const fs::path &state_path)
{
    /* One candidate with two rerun slots; only the first slot is observed,
     * and no image is declared yet, so the stored timeline is deliberately
     * unfinished. */
    Ledger ledger;
    if (!ledger.Begin("replay", 1, 2))
        return 1;
    Trial trial{
        "worker-a", 5, Outcome::Complete,
        {Error{"UNADDRESSABLE ACCESS", std::vector<uint64_t>{4100}}}};
    if (!ledger.Observe("replay", 1, 0, trial))
        return 2;
    if (ledger.Report() != "CASE replay 1 PENDING\n")
        return 3;
    if (!ledger.Save(state_path.string()))
        return 4;
    return 0;
}

int StageTwo(const fs::path &dir, const fs::path &state_path)
{
    /* Restore the unfinished candidate in a fresh process. */
    Ledger ledger;
    if (!ledger.Load(state_path.string()))
        return 10;
    if (ledger.Report() != "CASE replay 1 PENDING\n")
        return 11;

    /* The module declaration arrives late.  Its lifetime [0,10) covers both
     * trial ticks and the image maps 0x1004 to offset 4. */
    Image image{"worker-a", "token-a", "build-one", 1,
                4096, 4096, 0, 10};
    if (!ledger.AddImage(image))
        return 12;

    /* Complete the second slot with the same native error. */
    Trial trial{
        "worker-a", 6, Outcome::Complete,
        {Error{"UNADDRESSABLE ACCESS", std::vector<uint64_t>{4100}}}};
    if (!ledger.Observe("replay", 1, 1, trial))
        return 13;

    const std::string report = ledger.Report();
    const std::string expected =
        "CASE replay 1 STABLE\n"
        "GROUP UNADDR|build-one@4 replay\n";
    if (report != expected)
        return 14;

    /* Persist the now-finished timeline before publishing anything. */
    if (!ledger.Save(state_path.string()))
        return 15;

    std::ofstream report_file(dir / kReportName,
                              std::ios::binary | std::ios::trunc);
    if (!report_file)
        return 16;
    report_file << report;
    report_file.close();
    if (!report_file)
        return 17;

    std::ofstream transcript(dir / kTranscriptName,
                             std::ios::binary | std::ios::trunc);
    if (!transcript)
        return 18;
    transcript << "stage 1: saved unfinished candidate 'replay' (1 of 2 "
                  "slots, no image declared)\n";
    transcript << "stage 2: restored state, late-declared image build-one "
                  "[4096,8192) lifetime [0,10)\n";
    transcript << "stage 2: observed slot 1, both slots map to the same "
                  "signature\n";
    transcript << "final report:\n" << report;
    transcript.close();
    if (!transcript)
        return 19;
    return 0;
}

} /* namespace */

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3) {
        std::fprintf(stderr, "usage: %s OUTPUT_DIR [stage]\n", argv[0]);
        return 2;
    }
    const fs::path dir = argv[1];

    if (argc == 3) {
        const fs::path state_path = dir / kStateName;
        if (std::string(argv[2]) == "stage1")
            return StageOne(state_path);
        if (std::string(argv[2]) == "stage2")
            return StageTwo(dir, state_path);
        std::fprintf(stderr, "unknown stage %s\n", argv[2]);
        return 2;
    }

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        std::fprintf(stderr, "cannot create %s: %s\n", dir.string().c_str(),
                     ec.message().c_str());
        return 2;
    }
    const fs::path state_path = dir / kStateName;
    fs::remove(state_path, ec);

    const int rc1 = RunStage(argv[0], dir.string(), "stage1");
    if (rc1 != 0) {
        std::fprintf(stderr, "replay stage1 failed: %d\n", rc1);
        return 3;
    }
    const int rc2 = RunStage(argv[0], dir.string(), "stage2");
    if (rc2 != 0) {
        std::fprintf(stderr, "replay stage2 failed: %d\n", rc2);
        return 4;
    }

    /* The driver independently confirms the published artifacts. */
    const fs::path transcript = dir / kTranscriptName;
    const fs::path report = dir / kReportName;
    if (!fs::is_regular_file(transcript) || fs::file_size(transcript) == 0) {
        std::fprintf(stderr, "missing or empty transcript\n");
        return 5;
    }
    if (!fs::is_regular_file(report)) {
        std::fprintf(stderr, "missing report\n");
        return 6;
    }
    std::puts("REGRESSION_REPLAY two-process save/restore COMPLETE");
    return 0;
}
