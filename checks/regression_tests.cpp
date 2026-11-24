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

/* Offline regression ledger integration tests.
 *
 * Registered with task_support/check.h in three groups:
 *   timeline  - image lifetimes, address ranges, late declarations, identity
 *   consensus - signature sets, state transitions, generation replacement
 *   resume    - persistence, corruption resistance and the Perl entry point
 *
 * Everything here operates on simulated metadata; no target program runs.
 */

#include "task_support/check.h"
#include "drmemory/regression.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

namespace fs = std::filesystem;
using namespace drreg;

namespace {

const char *kTmpRoot = ".task-build/test-tmp";

Image MakeImage(const std::string &worker, const std::string &token,
                uint64_t generation, const std::string &build,
                uint64_t base, uint64_t size, uint64_t begin, uint64_t end)
{
    return Image{worker, token, build, generation, base, size, begin, end};
}

Error MakeError(const std::string &type, std::vector<uint64_t> pcs)
{
    return Error{type, std::move(pcs)};
}

Trial MakeTrial(const std::string &worker, uint64_t tick, Outcome outcome,
                std::vector<Error> errors = {})
{
    return Trial{worker, tick, outcome, std::move(errors)};
}

void WriteFile(const fs::path &path, const std::string &contents)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
}

std::string ReadFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

/* Runs a shell command; the test target is launched from the repository
 * root by task_support/run.sh, so relative tool paths resolve there. */
int RunShell(const std::string &command)
{
    int rc = std::system(command.c_str());
    return rc == -1 ? -1 : WEXITSTATUS(rc);
}

/* ------------------------------------------------------------------ */
/* timeline group                                                      */
/* ------------------------------------------------------------------ */

int test_unload_boundary()
{
    /* Lifetime [0,10) and address [4096,8192) are half open. */
    Image img = MakeImage("w", "tok", 1, "b1", 4096, 4096, 0, 10);

    Ledger inside;
    TASK_CHECK(inside.AddImage(img));
    TASK_CHECK(inside.Begin("in", 1, 1));
    TASK_CHECK(inside.Observe(
        "in", 1, 0, MakeTrial("w", 9, Outcome::Complete,
                              {MakeError("UNADDRESSABLE ACCESS", {4100})})));
    TASK_CHECK(inside.Report() ==
               "CASE in 1 STABLE\nGROUP UNADDR|b1@4 in\n");

    /* tick == end is outside the lifetime: unresolved, inconclusive. */
    Ledger outside;
    TASK_CHECK(outside.AddImage(img));
    TASK_CHECK(outside.Begin("out", 1, 1));
    TASK_CHECK(outside.Observe(
        "out", 1, 0, MakeTrial("w", 10, Outcome::Complete,
                               {MakeError("UNADDRESSABLE ACCESS", {4100})})));
    TASK_CHECK(outside.Report() == "CASE out 1 INCONCLUSIVE\n");

    /* Return address exactly one past the module end: subtract one lands on
     * the last valid byte (offset 0xfff). */
    Ledger edge;
    TASK_CHECK(edge.AddImage(img));
    TASK_CHECK(edge.Begin("edge", 1, 1));
    TASK_CHECK(edge.Observe(
        "edge", 1, 0,
        MakeTrial("w", 5, Outcome::Complete,
                  {MakeError("INVALID HEAP ARGUMENT", {8192})})));
    TASK_CHECK(edge.Report() ==
               "CASE edge 1 STABLE\nGROUP HEAP|b1@fff edge\n");

    /* An instruction PC one past the end is outside the range. */
    Ledger past;
    TASK_CHECK(past.AddImage(img));
    TASK_CHECK(past.Begin("past", 1, 1));
    TASK_CHECK(past.Observe(
        "past", 1, 0, MakeTrial("w", 5, Outcome::Complete,
                                {MakeError("UNADDRESSABLE ACCESS", {8192})})));
    TASK_CHECK(past.Report() == "CASE past 1 INCONCLUSIVE\n");
    return 0;
}

int test_reload_build_change()
{
    /* The same address range is unloaded and reloaded with another build. */
    TASK_CHECK(
        Ledger().AddImage(MakeImage("w", "tok", 1, "b1", 4096, 4096, 0, 10)));
    Ledger ledger;
    TASK_CHECK(ledger.AddImage(
        MakeImage("w", "tok", 1, "b1", 4096, 4096, 0, 10)));
    TASK_CHECK(ledger.AddImage(
        MakeImage("w", "tok", 2, "b2", 4096, 4096, 10, 20)));

    TASK_CHECK(ledger.Begin("old", 1, 1));
    TASK_CHECK(ledger.Observe(
        "old", 1, 0, MakeTrial("w", 5, Outcome::Complete,
                               {MakeError("UNINITIALIZED READ", {4100})})));
    TASK_CHECK(ledger.Begin("new", 1, 1));
    TASK_CHECK(ledger.Observe(
        "new", 1, 0, MakeTrial("w", 15, Outcome::Complete,
                               {MakeError("UNINITIALIZED READ", {4100})})));

    const std::string report = ledger.Report();
    TASK_CHECK(report.find("GROUP UNINIT|b1@4 old\n") != std::string::npos);
    TASK_CHECK(report.find("GROUP UNINIT|b2@4 new\n") != std::string::npos);
    return 0;
}

int test_late_images()
{
    /* Trials arrive before the module declaration; the later declaration
     * must retroactively produce the correct conclusion. */
    Ledger ledger;
    TASK_CHECK(ledger.Begin("late", 1, 1));
    TASK_CHECK(ledger.Observe(
        "late", 1, 0, MakeTrial("w", 7, Outcome::Complete,
                                {MakeError("UNADDRESSABLE ACCESS", {4100})})));
    TASK_CHECK(ledger.Report() == "CASE late 1 INCONCLUSIVE\n");

    TASK_CHECK(ledger.AddImage(
        MakeImage("w", "tok", 1, "b1", 4096, 4096, 0, 10)));
    TASK_CHECK(ledger.Report() ==
               "CASE late 1 STABLE\nGROUP UNADDR|b1@4 late\n");
    return 0;
}

int test_identity_and_chronology()
{
    Ledger ledger;
    Image a = MakeImage("w", "tok", 1, "b1", 4096, 4096, 0, 10);
    TASK_CHECK(ledger.AddImage(a));
    TASK_CHECK(ledger.AddImage(a)); /* identical record is idempotent */

    /* Same identity, different content: conflict. */
    Image conflict = a;
    conflict.build = "b2";
    TASK_CHECK(!ledger.AddImage(conflict));

    /* Same worker, overlapping time AND address: rejected. */
    TASK_CHECK(!ledger.AddImage(
        MakeImage("w", "tok2", 2, "b9", 5000, 4096, 5, 15)));
    /* Same worker, overlapping time but disjoint address: allowed. */
    TASK_CHECK(ledger.AddImage(
        MakeImage("w", "tok2", 2, "b9", 16384, 4096, 5, 15)));

    /* Same token on another worker may not overlap in time at any base. */
    TASK_CHECK(!ledger.AddImage(
        MakeImage("w2", "tok", 3, "b9", 1 << 20, 4096, 8, 12)));
    TASK_CHECK(ledger.AddImage(
        MakeImage("w2", "tok", 3, "b9", 1 << 20, 4096, 12, 20)));

    /* Generation order must agree with begin order for worker+token.
     * Establish gen 1 (begin 10), then a gen 2 may not begin earlier. */
    TASK_CHECK(ledger.AddImage(
        MakeImage("w", "tokX", 1, "b8", 1ULL << 21, 4096, 10, 20)));
    TASK_CHECK(!ledger.AddImage(
        MakeImage("w", "tokX", 2, "b9", 1ULL << 22, 4096, 0, 5)));
    TASK_CHECK(ledger.AddImage(
        MakeImage("w", "tokX", 2, "b9", 1ULL << 22, 4096, 20, 30)));
    /* An older declaration arriving late is legal when its begin is older
     * than its successor's begin. */
    TASK_CHECK(ledger.AddImage(
        MakeImage("w", "tokY", 1, "b7", 1ULL << 23, 4096, 0, 9)));
    TASK_CHECK(ledger.AddImage(
        MakeImage("w", "tokY", 2, "b7", 1ULL << 23, 4096, 9, 18)));

    /* Overflow and malformed ranges. */
    TASK_CHECK(!ledger.AddImage(
        MakeImage("w", "wrap", 1, "b", 0xfffffffffffff000ULL, 8192, 0, 1)));
    TASK_CHECK(!ledger.AddImage(
        MakeImage("w", "flat", 1, "b", 4096, 4096, 5, 5)));
    return 0;
}

int test_equivalent_aslr_stacks()
{
    /* The same build relocated in another process yields identical
     * signatures. */
    Ledger ledger;
    TASK_CHECK(ledger.AddImage(
        MakeImage("w1", "t1", 1, "build", 4096, 8192, 0, 100)));
    TASK_CHECK(ledger.AddImage(
        MakeImage("w2", "t2", 1, "build", 65536, 8192, 0, 100)));
    TASK_CHECK(ledger.Begin("c1", 1, 1));
    TASK_CHECK(ledger.Observe(
        "c1", 1, 0,
        MakeTrial("w1", 10, Outcome::Complete,
                  {MakeError("UNADDRESSABLE ACCESS", {4200, 5000})})));
    TASK_CHECK(ledger.Begin("c2", 1, 1));
    TASK_CHECK(ledger.Observe(
        "c2", 1, 0,
        MakeTrial("w2", 10, Outcome::Complete,
                  {MakeError("UNADDRESSABLE ACCESS", {65640, 66440})})));
    /* Second frame is a return address in both trials. */
    const std::string report = ledger.Report();
    TASK_CHECK(report.find("GROUP UNADDR|build@68,build@387 c1,c2\n") !=
                std::string::npos);
    return 0;
}

int test_zero_and_unknown_frames()
{
    Ledger ledger;
    TASK_CHECK(ledger.AddImage(
        MakeImage("w", "tok", 1, "b1", 4096, 4096, 0, 10)));

    /* Zero return address can never form a stable signature. */
    TASK_CHECK(ledger.Begin("zero", 1, 1));
    TASK_CHECK(ledger.Observe(
        "zero", 1, 0,
        MakeTrial("w", 5, Outcome::Complete,
                  {MakeError("UNADDRESSABLE ACCESS", {4100, 0})})));
    TASK_CHECK(ledger.Report().find("CASE zero 1 INCONCLUSIVE\n") !=
                std::string::npos);

    /* HEAP first frame is a return address; zero there is unresolved too. */
    TASK_CHECK(ledger.Begin("heapzero", 1, 1));
    TASK_CHECK(ledger.Observe(
        "heapzero", 1, 0,
        MakeTrial("w", 5, Outcome::Complete,
                  {MakeError("INVALID HEAP ARGUMENT", {0})})));
    TASK_CHECK(ledger.Report().find("CASE heapzero 1 INCONCLUSIVE\n") !=
                std::string::npos);

    /* Unknown worker for the trial: no matching image. */
    TASK_CHECK(ledger.Begin("unknown", 1, 1));
    TASK_CHECK(ledger.Observe(
        "unknown", 1, 0,
        MakeTrial("ghost", 5, Outcome::Complete,
                  {MakeError("UNADDRESSABLE ACCESS", {4100})})));
    TASK_CHECK(ledger.Report().find("CASE unknown 1 INCONCLUSIVE\n") !=
                std::string::npos);
    return 0;
}

/* ------------------------------------------------------------------ */
/* consensus group                                                     */
/* ------------------------------------------------------------------ */

Error SigError(uint64_t pc)
{
    return MakeError("UNADDRESSABLE ACCESS", {pc});
}

int test_set_equality_order_and_multiplicity()
{
    Ledger ledger;
    TASK_CHECK(ledger.AddImage(
        MakeImage("w", "tok", 1, "b1", 4096, 4096, 0, 100)));
    TASK_CHECK(ledger.Begin("c", 1, 2));
    /* Slot 0: two distinct errors, plus a duplicate. */
    TASK_CHECK(ledger.Observe(
        "c", 1, 0,
        MakeTrial("w", 10, Outcome::Complete,
                  {SigError(4100), SigError(4200), SigError(4100)})));
    /* Slot 1: same two signatures in the opposite order. */
    TASK_CHECK(ledger.Observe(
        "c", 1, 1,
        MakeTrial("w", 20, Outcome::Complete,
                  {SigError(4200), SigError(4100)})));
    const std::string report = ledger.Report();
    TASK_CHECK(report.find("CASE c 1 STABLE\n") != std::string::npos);
    TASK_CHECK(report.find("GROUP UNADDR|b1@4 c\n") != std::string::npos);
    TASK_CHECK(report.find("GROUP UNADDR|b1@68 c\n") != std::string::npos);
    return 0;
}

int test_pending_clean()
{
    Ledger ledger;
    TASK_CHECK(ledger.Begin("p", 1, 2));
    TASK_CHECK(ledger.Observe("p", 1, 0,
                              MakeTrial("w", 1, Outcome::Complete)));
    TASK_CHECK(ledger.Report() == "CASE p 1 PENDING\n");

    TASK_CHECK(ledger.Observe("p", 1, 1,
                              MakeTrial("w", 2, Outcome::Complete)));
    TASK_CHECK(ledger.Report() == "CASE p 1 CLEAN\n");

    /* Empty ledger reports nothing. */
    TASK_CHECK(Ledger().Report() == "");
    return 0;
}

int test_inconclusive_outcomes()
{
    Ledger ledger;
    TASK_CHECK(ledger.AddImage(
        MakeImage("w", "tok", 1, "b1", 4096, 4096, 0, 100)));
    TASK_CHECK(ledger.Begin("mix", 1, 3));
    TASK_CHECK(ledger.Observe("mix", 1, 0,
                              MakeTrial("w", 1, Outcome::Complete)));
    TASK_CHECK(ledger.Observe("mix", 1, 1,
                              MakeTrial("w", 2, Outcome::Timeout)));
    TASK_CHECK(ledger.Observe("mix", 1, 2,
                              MakeTrial("w", 3, Outcome::ToolError)));
    TASK_CHECK(ledger.Report() == "CASE mix 1 INCONCLUSIVE\n");

    /* Non-complete outcomes must carry an empty error vector. */
    Trial bad = MakeTrial("w", 4, Outcome::Timeout, {SigError(4100)});
    TASK_CHECK(!ledger.Observe("mix", 1, 0, bad));
    return 0;
}

int test_flaky_and_groups()
{
    Ledger ledger;
    TASK_CHECK(ledger.AddImage(
        MakeImage("w", "tok", 1, "b1", 4096, 4096, 0, 100)));

    /* {A,B} vs {A,C}: flaky, contributes to no group. */
    TASK_CHECK(ledger.Begin("f", 1, 2));
    TASK_CHECK(ledger.Observe(
        "f", 1, 0, MakeTrial("w", 1, Outcome::Complete,
                             {SigError(4100), SigError(4200)})));
    TASK_CHECK(ledger.Observe(
        "f", 1, 1, MakeTrial("w", 2, Outcome::Complete,
                             {SigError(4100), SigError(4300)})));
    TASK_CHECK(ledger.Report() == "CASE f 1 FLAKY\n");

    /* One empty and one non-empty set: also flaky. */
    TASK_CHECK(ledger.Begin("g", 1, 2));
    TASK_CHECK(ledger.Observe("g", 1, 0,
                              MakeTrial("w", 1, Outcome::Complete)));
    TASK_CHECK(ledger.Observe(
        "g", 1, 1, MakeTrial("w", 2, Outcome::Complete,
                             {SigError(4100)})));
    TASK_CHECK(ledger.Report().find("CASE g 1 FLAKY\n") !=
                std::string::npos);

    /* A stable {A,B} case contributes once to each signature's group, and a
     * shared error does not make the flaky case stable. */
    TASK_CHECK(ledger.Begin("s", 1, 1));
    TASK_CHECK(ledger.Observe(
        "s", 1, 0, MakeTrial("w", 1, Outcome::Complete,
                             {SigError(4100), SigError(4200)})));
    const std::string report = ledger.Report();
    TASK_CHECK(report.find("GROUP UNADDR|b1@4 s\n") != std::string::npos);
    TASK_CHECK(report.find("GROUP UNADDR|b1@68 s\n") != std::string::npos);
    TASK_CHECK(report.find(" f\n") == std::string::npos);
    return 0;
}

int test_idempotent_reports_and_conflicts()
{
    Ledger ledger;
    TASK_CHECK(ledger.Begin("c", 1, 1));
    Trial trial = MakeTrial("w", 7, Outcome::Complete, {SigError(4100)});
    TASK_CHECK(ledger.Observe("c", 1, 0, trial));
    TASK_CHECK(ledger.Observe("c", 1, 0, trial)); /* idempotent */

    Trial conflict = trial;
    conflict.tick = 8;
    TASK_CHECK(!ledger.Observe("c", 1, 0, conflict));
    Trial conflict2 = trial;
    conflict2.worker = "w2";
    TASK_CHECK(!ledger.Observe("c", 1, 0, conflict2));

    /* Out-of-range slots and unknown generations are refused. */
    TASK_CHECK(!ledger.Observe("c", 1, 1, trial));
    TASK_CHECK(!ledger.Observe("c", 2, 0, trial));
    TASK_CHECK(!ledger.Observe("missing", 1, 0, trial));
    return 0;
}

int test_generation_replacement()
{
    Ledger ledger;
    TASK_CHECK(ledger.AddImage(
        MakeImage("w", "tok", 1, "b1", 4096, 4096, 0, 100)));
    TASK_CHECK(ledger.Begin("c", 1, 1));
    TASK_CHECK(ledger.Observe(
        "c", 1, 0, MakeTrial("w", 5, Outcome::Complete,
                             {SigError(4100)})));
    TASK_CHECK(ledger.Report().find("GROUP UNADDR|b1@4 c\n") !=
                std::string::npos);

    /* Same generation with a different slot count fails. */
    TASK_CHECK(!ledger.Begin("c", 1, 2));
    /* Lower generation fails. */
    TASK_CHECK(!ledger.Begin("c", 0, 1));

    /* Higher generation replaces the trial state and withdraws old groups. */
    TASK_CHECK(ledger.Begin("c", 2, 1));
    TASK_CHECK(ledger.Report() == "CASE c 2 PENDING\n");
    /* Events from the retired generation are rejected. */
    TASK_CHECK(!ledger.Observe(
        "c", 1, 0, MakeTrial("w", 5, Outcome::Complete, {SigError(4100)})));
    TASK_CHECK(ledger.Observe("c", 2, 0,
                              MakeTrial("w", 6, Outcome::Complete)));
    TASK_CHECK(ledger.Report() == "CASE c 2 CLEAN\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* resume group                                                        */
/* ------------------------------------------------------------------ */

int test_resume_unfinished()
{
    const fs::path dir = fs::path(kTmpRoot) / "resume";
    const fs::path state = dir / "state.bin";
    fs::remove_all(dir);
    fs::create_directories(dir);

    Ledger first;
    TASK_CHECK(first.AddImage(
        MakeImage("w", "tok", 1, "b1", 4096, 4096, 0, 100)));
    TASK_CHECK(first.Begin("c", 1, 2));
    TASK_CHECK(first.Observe(
        "c", 1, 0, MakeTrial("w", 5, Outcome::Complete,
                             {SigError(4100)})));
    TASK_CHECK(first.Save(state));

    /* A second ledger resumes the unfinished candidate. */
    Ledger second;
    TASK_CHECK(second.Load(state));
    TASK_CHECK(second.Report() == "CASE c 1 PENDING\n");
    TASK_CHECK(second.Observe(
        "c", 1, 1, MakeTrial("w", 6, Outcome::Complete,
                             {SigError(4100)})));

    /* The uninterrupted equivalent reaches exactly the same conclusion. */
    Ledger direct;
    TASK_CHECK(direct.AddImage(
        MakeImage("w", "tok", 1, "b1", 4096, 4096, 0, 100)));
    TASK_CHECK(direct.Begin("c", 1, 2));
    TASK_CHECK(direct.Observe(
        "c", 1, 0, MakeTrial("w", 5, Outcome::Complete,
                             {SigError(4100)})));
    TASK_CHECK(direct.Observe(
        "c", 1, 1, MakeTrial("w", 6, Outcome::Complete,
                             {SigError(4100)})));
    TASK_CHECK(second.Report() == direct.Report());

    /* A full round trip preserves the finished report byte for byte. */
    const fs::path state2 = fs::path(kTmpRoot) / "resume" / "state2.bin";
    TASK_CHECK(second.Save(state2));
    Ledger third;
    TASK_CHECK(third.Load(state2));
    TASK_CHECK(third.Report() == second.Report());
    return 0;
}

int test_corruption_preserves_object()
{
    const fs::path dir = fs::path(kTmpRoot) / "corrupt";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path state = dir / "state.bin";

    Ledger good;
    TASK_CHECK(good.Begin("keep", 1, 1));
    TASK_CHECK(good.Observe("keep", 1, 0,
                            MakeTrial("w", 1, Outcome::Complete)));
    TASK_CHECK(good.Save(state));
    const std::string before = good.Report();

    /* Flip a payload byte: checksum must reject the load. */
    std::string bytes = ReadFile(state);
    TASK_CHECK(bytes.size() > 40);
    bytes[40] = (bytes[40] == 'A') ? 'B' : 'A';
    WriteFile(state, bytes);
    TASK_CHECK(!good.Load(state));
    TASK_CHECK(good.Report() == before);

    /* Wrong format version / magic. */
    WriteFile(state, "OTHER1\nCHECKSUM 0\nEND\n");
    TASK_CHECK(!good.Load(state));
    TASK_CHECK(good.Report() == before);

    /* Truncated file. */
    WriteFile(state, std::string(bytes.data(), bytes.size() / 2));
    TASK_CHECK(!good.Load(state));
    TASK_CHECK(good.Report() == before);

    /* Trailing junk invalidates the payload. */
    TASK_CHECK(good.Save(state));
    std::string valid = ReadFile(state);
    WriteFile(state, valid + "IMAGE junk\n");
    TASK_CHECK(!good.Load(state));
    TASK_CHECK(good.Report() == before);

    /* A missing file is a failed load, not an empty ledger. */
    TASK_CHECK(!good.Load(dir / "does-not-exist.bin"));
    TASK_CHECK(good.Report() == before);
    return 0;
}

int test_save_failures_keep_outputs()
{
    const fs::path dir = fs::path(kTmpRoot) / "savefail";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path state = dir / "state.bin";

    Ledger ledger;
    TASK_CHECK(ledger.Begin("keep", 1, 1));
    TASK_CHECK(ledger.Observe("keep", 1, 0,
                              MakeTrial("w", 1, Outcome::Complete)));
    TASK_CHECK(ledger.Save(state));
    const std::string original = ReadFile(state);

    /* Saving into a nonexistent directory fails and leaves the old file. */
    const fs::path bad = dir / "no-such-dir" / "state.bin";
    TASK_CHECK(!ledger.Save(bad));
    TASK_CHECK(ReadFile(state) == original);

    /* The old state still loads cleanly. */
    Ledger restored;
    TASK_CHECK(restored.Load(state));
    TASK_CHECK(restored.Report() == "CASE keep 1 CLEAN\n");
    return 0;
}

int test_perl_entry_and_rejections()
{
    const fs::path dir = fs::path(kTmpRoot) / "perl entry";
    fs::remove_all(dir);
    fs::create_directories(dir);

    WriteFile(dir / "empty.log", "LOG END\n");
    WriteFile(dir / "stack.log",
              "CALLSTACK 7\n"
              "# 0 benign!? (0x1004 <benign+0x4>)\n"
              "# 1 benign!? (0x2000 <benign+0x1000>)\n"
              "  error end\n");
    WriteFile(dir / "error.log",
              "Error #1: UNADDRESSABLE ACCESS: reading simulated metadata\n"
              "  info: @0:00:00.001 in thread 1\n"
              "  callstack=7\n"
              "  error end\n"
              "LOG END\n");
    WriteFile(dir / "manifest.txt",
              "CASE mapped 1 1\n"
              "IMAGE worker token 1 artifact 4096 4096 10 20\n"
              "RUN mapped 1 0 worker 12 COMPLETE\n"
              "STACK shared stack.log\n"
              "LOG error.log\n"
              "ENDRUN\n");

    const std::string report = (dir / "report.txt").string();
    const std::string engine = ".task-build/regression-engine";
    std::string cmd =
        "perl drmemory/postprocess.pl -regression_manifest '" +
        (dir / "manifest.txt").string() + "' -regression_engine '" +
        engine + "' -regression_report '" + report + "'";
    TASK_CHECK(RunShell(cmd) == 0);
    TASK_CHECK(ReadFile(report) ==
               "CASE mapped 1 STABLE\n"
               "GROUP UNADDR|artifact@4,artifact@fff mapped\n");

    /* Terminal truncation: an error block without error end must fail. */
    WriteFile(dir / "truncated.log",
              "Error #1: UNADDRESSABLE ACCESS: reading\n"
              "# 0 benign!? (0x1004 <benign+0x4>)\n");
    WriteFile(dir / "manifest-trunc.txt",
              "CASE t 1 1\n"
              "RUN t 1 0 worker 12 COMPLETE\n"
              "LOG truncated.log\n"
              "ENDRUN\n");
    cmd = "perl drmemory/postprocess.pl -regression_manifest '" +
          (dir / "manifest-trunc.txt").string() +
          "' -regression_engine '" + engine + "' -regression_report '" +
          (dir / "trunc-out.txt").string() + "'";
    TASK_CHECK(RunShell(cmd) != 0);
    TASK_CHECK(!fs::exists(dir / "trunc-out.txt"));

    /* Unknown shared stack reference must fail. */
    WriteFile(dir / "badref.log",
              "Error #1: UNADDRESSABLE ACCESS: reading\n"
              "  callstack=9\n"
              "  error end\nLOG END\n");
    WriteFile(dir / "manifest-badref.txt",
              "CASE t 1 1\n"
              "RUN t 1 0 worker 12 COMPLETE\n"
              "LOG badref.log\n"
              "ENDRUN\n");
    cmd = "perl drmemory/postprocess.pl -regression_manifest '" +
          (dir / "manifest-badref.txt").string() +
          "' -regression_engine '" + engine + "' -regression_report '" +
          (dir / "badref-out.txt").string() + "'";
    TASK_CHECK(RunShell(cmd) != 0);

    /* Unsupported error type (a leak) must fail. */
    WriteFile(dir / "leak.log",
              "Error #1: LEAK 12 byte(s)\n"
              "  error end\nLOG END\n");
    WriteFile(dir / "manifest-leak.txt",
              "CASE t 1 1\n"
              "RUN t 1 0 worker 12 COMPLETE\n"
              "LOG leak.log\n"
              "ENDRUN\n");
    cmd = "perl drmemory/postprocess.pl -regression_manifest '" +
          (dir / "manifest-leak.txt").string() +
          "' -regression_engine '" + engine + "' -regression_report '" +
          (dir / "leak-out.txt").string() + "'";
    TASK_CHECK(RunShell(cmd) != 0);

    /* An engine refusal (bad event protocol) propagates nonzero. */
    WriteFile(dir / "bad-events.txt",
              "TRIAL ghost 1 0 worker 12 COMPLETE 0\nENDTRIAL\n");
    cmd = engine + " '" + (dir / "bad-events.txt").string() + "' '" +
          (dir / "engine-out.txt").string() + "'";
    TASK_CHECK(RunShell(cmd) != 0);
    TASK_CHECK(!fs::exists(dir / "engine-out.txt"));

    /* EOF inside a RUN block must fail. */
    WriteFile(dir / "manifest-eof.txt",
              "CASE e 1 1\n"
              "RUN e 1 0 worker 12 COMPLETE\n"
              "LOG empty.log\n");
    cmd = "perl drmemory/postprocess.pl -regression_manifest '" +
          (dir / "manifest-eof.txt").string() +
          "' -regression_engine '" + engine + "' -regression_report '" +
          (dir / "eof-out.txt").string() + "'";
    TASK_CHECK(RunShell(cmd) != 0);
    TASK_CHECK(!fs::exists(dir / "eof-out.txt"));

    /* Numeric overflow in a manifest value must fail. */
    WriteFile(dir / "manifest-overflow.txt",
              "CASE e 18446744073709551616 1\n"
              "RUN e 1 0 worker 12 COMPLETE\n"
              "LOG empty.log\nENDRUN\n");
    cmd = "perl drmemory/postprocess.pl -regression_manifest '" +
          (dir / "manifest-overflow.txt").string() +
          "' -regression_engine '" + engine + "' -regression_report '" +
          (dir / "ovf-out.txt").string() + "'";
    TASK_CHECK(RunShell(cmd) != 0);

    /* An unknown manifest directive must fail. */
    WriteFile(dir / "manifest-unknown.txt",
              "CASE e 1 1\nWIDGET 42\n");
    cmd = "perl drmemory/postprocess.pl -regression_manifest '" +
          (dir / "manifest-unknown.txt").string() +
          "' -regression_engine '" + engine + "' -regression_report '" +
          (dir / "unk-out.txt").string() + "'";
    TASK_CHECK(RunShell(cmd) != 0);

    /* A timeout trial containing errors must fail. */
    WriteFile(dir / "direct-error.log",
              "Error #1: UNADDRESSABLE ACCESS: reading\n"
              "# 0 benign!? (0x1004 <benign+0x4>)\n"
              "  error end\nLOG END\n");
    WriteFile(dir / "manifest-timeout.txt",
              "CASE e 1 1\n"
              "RUN e 1 0 worker 12 TIMEOUT\n"
              "LOG direct-error.log\nENDRUN\n");
    cmd = "perl drmemory/postprocess.pl -regression_manifest '" +
          (dir / "manifest-timeout.txt").string() +
          "' -regression_engine '" + engine + "' -regression_report '" +
          (dir / "to-out.txt").string() + "'";
    TASK_CHECK(RunShell(cmd) != 0);
    return 0;
}

} /* namespace */

const struct task_case task_cases[] = {
    {"timeline", "unload_boundary", test_unload_boundary},
    {"timeline", "reload_build_change", test_reload_build_change},
    {"timeline", "late_images", test_late_images},
    {"timeline", "identity_chronology", test_identity_and_chronology},
    {"timeline", "equivalent_aslr", test_equivalent_aslr_stacks},
    {"timeline", "zero_unknown_frames", test_zero_and_unknown_frames},
    {"consensus", "set_equality", test_set_equality_order_and_multiplicity},
    {"consensus", "pending_clean", test_pending_clean},
    {"consensus", "inconclusive_outcomes", test_inconclusive_outcomes},
    {"consensus", "flaky_groups", test_flaky_and_groups},
    {"consensus", "idempotent_conflict", test_idempotent_reports_and_conflicts},
    {"consensus", "generation_replacement", test_generation_replacement},
    {"resume", "resume_unfinished", test_resume_unfinished},
    {"resume", "corruption", test_corruption_preserves_object},
    {"resume", "save_failures", test_save_failures_keep_outputs},
    {"resume", "perl_entry", test_perl_entry_and_rejections},
};
const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
