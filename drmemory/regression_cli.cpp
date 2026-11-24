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

/* Structured-event driver for the offline regression ledger.
 *
 * Usage: regression_cli EVENTS REPORT [STATE]
 *
 * EVENTS holds only structured records emitted by the Perl adapter (never
 * raw log text): CASE/IMAGE directives and TRIAL/ERROR/ENDTRIAL blocks.
 * Everything is parsed and applied to a temporary ledger first; no output
 * file is touched until the whole invocation succeeds.
 */

#include "drmemory/regression.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

using drreg::Error;
using drreg::Image;
using drreg::Ledger;
using drreg::Outcome;
using drreg::Trial;

std::vector<std::string> Split(const std::string &line)
{
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && line[i] == ' ')
            ++i;
        size_t start = i;
        while (i < line.size() && line[i] != ' ')
            ++i;
        if (i > start)
            tokens.push_back(line.substr(start, i - start));
    }
    return tokens;
}

bool ParseU64(const std::string &s, uint64_t &out)
{
    if (s.empty())
        return false;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9')
            return false;
        uint64_t d = static_cast<uint64_t>(c - '0');
        if (v > (~uint64_t(0)) / 10 ||
            (v == (~uint64_t(0)) / 10 && d > (~uint64_t(0)) % 10))
            return false;
        v = v * 10 + d;
    }
    out = v;
    return true;
}

bool ParseOutcome(const std::string &s, Outcome &out)
{
    if (s == "COMPLETE") {
        out = Outcome::Complete;
        return true;
    }
    if (s == "TIMEOUT") {
        out = Outcome::Timeout;
        return true;
    }
    if (s == "TOOLERROR") {
        out = Outcome::ToolError;
        return true;
    }
    return false;
}

bool CodeToType(const std::string &code, std::string &type)
{
    if (code == "UNADDR") {
        type = "UNADDRESSABLE ACCESS";
        return true;
    }
    if (code == "UNINIT") {
        type = "UNINITIALIZED READ";
        return true;
    }
    if (code == "HEAP") {
        type = "INVALID HEAP ARGUMENT";
        return true;
    }
    return false;
}

/* Publish text atomically: sibling temporary, then rename, so a failed
 * invocation can never truncate or replace the existing report. */
bool AtomicWrite(const std::string &path, const std::string &contents)
{
    std::string tmp_path;
    std::ofstream out;
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        std::ostringstream name;
        name << path << ".tmp." << static_cast<long>(getpid()) << '.'
             << attempt;
        tmp_path = name.str();
        out.open(tmp_path, std::ios::binary | std::ios::out |
                               std::ios::trunc);
        if (out.is_open())
            break;
        if (errno != EEXIST)
            return false;
    }
    if (!out.is_open())
        return false;
    out << contents;
    out.flush();
    bool ok = out.good();
    out.close();
    if (!out)
        ok = false;
    if (!ok) {
        std::remove(tmp_path.c_str());
        return false;
    }
    if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
        std::remove(tmp_path.c_str());
        return false;
    }
    return true;
}

bool ApplyEvents(std::istream &in, Ledger &ledger)
{
    std::string line;
    bool in_trial = false;
    std::string trial_id;
    uint64_t trial_generation = 0;
    size_t trial_slot = 0;
    Trial trial;
    size_t errors_left = 0;

    while (std::getline(in, line)) {
        /* A carried CR is malformed protocol data. */
        if (!line.empty() && line.back() == '\r')
            return false;
        if (line.empty() || line[0] == '#')
            continue;
        std::vector<std::string> tok = Split(line);
        if (tok.empty())
            continue;

        if (in_trial && tok[0] != "ERROR" && tok[0] != "ENDTRIAL")
            return false;

        if (tok[0] == "CASE") {
            if (tok.size() != 4)
                return false;
            uint64_t generation, repeats64;
            if (!ParseU64(tok[2], generation) ||
                !ParseU64(tok[3], repeats64) || repeats64 == 0 ||
                repeats64 > 64)
                return false;
            if (!ledger.Begin(tok[1], generation,
                              static_cast<size_t>(repeats64)))
                return false;
        } else if (tok[0] == "IMAGE") {
            if (tok.size() != 9)
                return false;
            Image img;
            /* Protocol order: worker token generation build base size
             * begin end; the Image struct orders build before generation. */
            img.worker = tok[1];
            img.token = tok[2];
            img.build = tok[4];
            if (!ParseU64(tok[3], img.generation) ||
                !ParseU64(tok[5], img.base) ||
                !ParseU64(tok[6], img.size) ||
                !ParseU64(tok[7], img.begin) ||
                !ParseU64(tok[8], img.end))
                return false;
            if (!ledger.AddImage(img))
                return false;
        } else if (tok[0] == "TRIAL") {
            if (in_trial || tok.size() != 8)
                return false;
            uint64_t generation, slot64, tick, nerrors;
            Outcome outcome;
            if (!ParseU64(tok[2], generation) ||
                !ParseU64(tok[3], slot64) ||
                !ParseU64(tok[5], tick) ||
                !ParseU64(tok[7], nerrors) ||
                !ParseOutcome(tok[6], outcome) || nerrors > 1024)
                return false;
            trial_id = tok[1];
            trial_generation = generation;
            trial_slot = static_cast<size_t>(slot64);
            trial = Trial();
            trial.worker = tok[4];
            trial.tick = tick;
            trial.outcome = outcome;
            errors_left = static_cast<size_t>(nerrors);
            in_trial = true;
        } else if (tok[0] == "ERROR") {
            if (!in_trial || tok.size() < 3 || errors_left == 0)
                return false;
            Error e;
            if (!CodeToType(tok[1], e.type))
                return false;
            uint64_t npc;
            if (!ParseU64(tok[2], npc) || npc == 0 || npc > 64 ||
                tok.size() != 3 + static_cast<size_t>(npc))
                return false;
            for (size_t i = 0; i < npc; ++i) {
                uint64_t pc;
                if (!ParseU64(tok[3 + i], pc))
                    return false;
                e.pcs.push_back(pc);
            }
            trial.errors.push_back(std::move(e));
            --errors_left;
        } else if (tok[0] == "ENDTRIAL") {
            if (!in_trial || tok.size() != 1 || errors_left != 0)
                return false;
            if (!ledger.Observe(trial_id, trial_generation, trial_slot,
                                trial))
                return false;
            in_trial = false;
        } else {
            return false; /* unknown directive / trailing junk */
        }
    }
    return !in_trial; /* EOF inside a trial is a truncation failure */
}

} /* namespace */

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4)
        return 2;
    const std::string events_path = argv[1];
    const std::string report_path = argv[2];
    const bool have_state = (argc == 4);
    const std::string state_path = have_state ? argv[3] : std::string();

    /* Build everything against a fresh ledger first; existing outputs are
     * not touched until all steps succeed. */
    Ledger ledger;
    if (have_state) {
        struct stat st;
        if (stat(state_path.c_str(), &st) == 0) {
            /* Present: a corrupt or unreadable state must fail loudly rather
             * than silently start an empty ledger. */
            if (!ledger.Load(state_path))
                return 1;
        } else if (errno != ENOENT) {
            return 1; /* present but not stat-able */
        }
    }

    {
        std::ifstream events(events_path, std::ios::binary);
        if (!events.is_open() || !ApplyEvents(events, ledger))
            return 1;
        if (events.bad())
            return 1;
    }

    /* State must be durably saved before the report is published. */
    if (have_state && !ledger.Save(state_path))
        return 1;

    if (!AtomicWrite(report_path, ledger.Report()))
        return 1;
    return 0;
}
