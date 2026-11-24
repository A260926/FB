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

/* Ledger persistence: a versioned, checksummed, line-oriented format with
 * atomic sibling-temporary saves and all-or-nothing loads.  No process
 * addresses are serialized; only contract data fields.
 */

#include "drmemory/regression_impl.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <unistd.h>

namespace drreg {
namespace {

constexpr const char *kFormatMagic = "DRREG1";
constexpr const char *kFormatEnd = "END";

/* CRC-64-ECMA (poly 0x42F0E1EBA9EA3693), table-driven, init/xorout zero.
 * Self-consistent is all the format requires; the table is deterministic. */
struct Crc64 {
    uint64_t table[256];
    Crc64()
    {
        constexpr uint64_t poly = 0x42F0E1EBA9EA3693ULL;
        for (uint64_t i = 0; i < 256; ++i) {
            uint64_t crc = i << 56;
            for (int b = 0; b < 8; ++b)
                crc = (crc & (1ULL << 63)) ? (crc << 1) ^ poly : (crc << 1);
            table[i] = crc;
        }
    }
    uint64_t Compute(const std::string &data) const
    {
        uint64_t crc = 0;
        for (unsigned char c : data)
            crc = table[((crc >> 56) ^ c) & 0xff] ^ (crc << 8);
        return crc;
    }
};
const Crc64 kCrc64;

const char *OutcomeName(Outcome o)
{
    switch (o) {
    case Outcome::Complete: return "COMPLETE";
    case Outcome::Timeout: return "TIMEOUT";
    case Outcome::ToolError: return "TOOLERROR";
    }
    return "";
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

/* Reverse of detail::TypeName. */
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

/* Strict token splitter; identifiers and decimal numbers never contain
 * spaces, and error types are stored as space-free codes. */
std::vector<std::string> Tokenize(const std::string &line)
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

/* Parse the payload into a fresh ledger through the public mutating API, so
 * every identifier, range, conflict and idempotency rule is enforced exactly
 * once.  Returns false on any structural or semantic problem. */
bool ParsePayload(const std::string &payload, Ledger &target)
{
    std::istringstream is(payload);
    std::string line;
    bool saw_end = false;
    /* State of the trial currently being parsed. */
    bool in_trial = false;
    std::string trial_case;
    uint64_t trial_generation = 0;
    size_t trial_repeats = 0;
    size_t trial_slot = 0;
    Trial trial;
    size_t errors_left = 0;

    while (std::getline(is, line)) {
        /* Serialized lines never carry CR; one would mean corruption. */
        if (!line.empty() && line.back() == '\r')
            return false;
        if (saw_end)
            return false; /* junk after END */
        if (line.empty())
            return false;
        std::vector<std::string> tok = Tokenize(line);

        if (in_trial && tok[0] != "ERROR" && tok[0] != "ENDTRIAL")
            return false;

        if (tok[0] == "IMAGE") {
            if (tok.size() != 9)
                return false;
            Image img;
            img.worker = tok[1];
            img.token = tok[2];
            img.build = tok[3];
            if (!detail::ParseDecimal(tok[4], img.generation) ||
                !detail::ParseDecimal(tok[5], img.base) ||
                !detail::ParseDecimal(tok[6], img.size) ||
                !detail::ParseDecimal(tok[7], img.begin) ||
                !detail::ParseDecimal(tok[8], img.end))
                return false;
            if (!target.AddImage(img))
                return false;
        } else if (tok[0] == "CASE") {
            if (tok.size() != 4 || in_trial)
                return false;
            uint64_t generation, repeats64;
            if (!detail::ParseDecimal(tok[2], generation) ||
                !detail::ParseDecimal(tok[3], repeats64) ||
                repeats64 == 0 || repeats64 > 64)
                return false;
            if (!target.Begin(tok[1], generation,
                              static_cast<size_t>(repeats64)))
                return false;
            trial_case = tok[1];
            trial_generation = generation;
            trial_repeats = static_cast<size_t>(repeats64);
        } else if (tok[0] == "TRIAL") {
            if (in_trial || tok.size() != 6 || trial_case.empty())
                return false;
            uint64_t slot64, tick, nerrors;
            Outcome outcome;
            if (!detail::ParseDecimal(tok[1], slot64) ||
                !detail::ParseDecimal(tok[3], tick) ||
                !detail::ParseDecimal(tok[5], nerrors) ||
                !ParseOutcome(tok[4], outcome) || nerrors > 1024)
                return false;
            if (slot64 >= trial_repeats)
                return false;
            trial_slot = static_cast<size_t>(slot64);
            trial = Trial();
            trial.worker = tok[2];
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
            if (!detail::ParseDecimal(tok[2], npc) || npc == 0 || npc > 64 ||
                tok.size() != 3 + static_cast<size_t>(npc))
                return false;
            for (size_t i = 0; i < npc; ++i) {
                uint64_t pc;
                if (!detail::ParseDecimal(tok[3 + i], pc))
                    return false;
                e.pcs.push_back(pc);
            }
            trial.errors.push_back(std::move(e));
            --errors_left;
        } else if (tok[0] == "ENDTRIAL") {
            if (!in_trial || tok.size() != 1 || errors_left != 0)
                return false;
            if (!target.Observe(trial_case, trial_generation, trial_slot,
                                trial))
                return false;
            in_trial = false;
        } else if (tok[0] == "ENDCASE") {
            if (in_trial || tok.size() != 1)
                return false;
            trial_case.clear();
        } else if (tok[0] == kFormatEnd) {
            if (in_trial || tok.size() != 1)
                return false;
            saw_end = true;
        } else {
            return false; /* unknown directive / extra data */
        }
    }
    return saw_end && !in_trial;
}

} /* namespace */

bool Ledger::Save(const std::string &path) const
{
    /* Build the payload while walking the private implementation. */
    std::ostringstream payload_os;
    for (const Image &img : impl_->images) {
        payload_os << "IMAGE " << img.worker << ' ' << img.token << ' '
                   << img.build << ' ' << img.generation << ' ' << img.base
                   << ' ' << img.size << ' ' << img.begin << ' ' << img.end
                   << '\n';
    }
    for (const Impl::Case &c : impl_->cases) {
        payload_os << "CASE " << c.id << ' ' << c.generation << ' '
                   << c.repeats << '\n';
        for (size_t slot = 0; slot < c.slots.size(); ++slot) {
            const Trial *t = c.slots[slot].get();
            if (t == nullptr)
                continue;
            payload_os << "TRIAL " << slot << ' ' << t->worker << ' '
                       << t->tick << ' ' << OutcomeName(t->outcome) << ' '
                       << t->errors.size() << '\n';
            for (const Error &e : t->errors) {
                const char *code = detail::TypeName(e.type);
                payload_os << "ERROR " << code << ' ' << e.pcs.size();
                for (uint64_t pc : e.pcs)
                    payload_os << ' ' << pc;
                payload_os << '\n';
            }
            payload_os << "ENDTRIAL\n";
        }
        payload_os << "ENDCASE\n";
    }
    payload_os << kFormatEnd << '\n';
    const std::string payload = payload_os.str();
    const uint64_t checksum = kCrc64.Compute(payload);

    std::ostringstream head;
    head << kFormatMagic << '\n'
         << "CHECKSUM " << std::hex << checksum << '\n';
    const std::string header = head.str();

    /* Sibling temporary in the same directory so the final rename is
     * atomic.  Never open the destination itself. */
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

    bool ok = true;
    out << header << payload;
    out.flush();
    if (!out.good())
        ok = false;
    out.close();
    if (!out)
        ok = false;

    if (!ok) {
        std::remove(tmp_path.c_str()); /* only our own temporary */
        return false;
    }
    if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
        std::remove(tmp_path.c_str());
        return false; /* destination bytes untouched */
    }
    return true;
}

bool Ledger::Load(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
        return false; /* old object retained */
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string contents = buffer.str();
    if (!in.good() && !in.eof())
        return false;

    /* Split header/magic, checksum line and payload precisely. */
    size_t pos = 0;
    auto next_line = [&](std::string &line) -> bool {
        size_t nl = contents.find('\n', pos);
        if (nl == std::string::npos)
            return false;
        line = contents.substr(pos, nl - pos);
        pos = nl + 1;
        return true;
    };

    std::string magic;
    std::string checksum_line;
    if (!next_line(magic) || magic != kFormatMagic ||
        !next_line(checksum_line))
        return false;
    const std::string prefix = "CHECKSUM ";
    if (checksum_line.compare(0, prefix.size(), prefix) != 0)
        return false;
    const std::string hex = checksum_line.substr(prefix.size());
    if (hex.empty() || hex.size() > 16)
        return false;
    uint64_t stored = 0;
    for (char c : hex) {
        int d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else
            return false;
        stored = stored * 16 + static_cast<uint64_t>(d);
    }

    const std::string payload = contents.substr(pos);
    if (payload.empty() || payload.back() != '\n')
        return false; /* truncated */
    if (kCrc64.Compute(payload) != stored)
        return false; /* corrupted */

    /* Build the replacement in a separate object; only publish after every
     * record validates, so a refused load leaves this object intact. */
    Ledger parsed;
    if (!ParsePayload(payload, parsed))
        return false;

    std::swap(impl_, parsed.impl_);
    return true;
}

} /* namespace drreg */
