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

/* Private implementation details shared by regression.cpp and
 * regression_store.cpp.  Not part of the public API.
 */

#ifndef DRMEMORY_REGRESSION_IMPL_H
#define DRMEMORY_REGRESSION_IMPL_H 1

#include "drmemory/regression.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace drreg {
namespace detail {

constexpr uint64_t kMaxUint64 = ~static_cast<uint64_t>(0);

/* Identifiers are 1..128 ASCII chars from [A-Za-z0-9_.-]. */
inline bool ValidIdentifier(const std::string &s)
{
    if (s.empty() || s.size() > 128)
        return false;
    for (char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
              c == '.' || c == '-'))
            return false;
    }
    return true;
}

/* Strict unsigned decimal parse: no sign, no whitespace, no leading +,
 * no overflow, no trailing junk. */
inline bool ParseDecimal(const std::string &s, uint64_t &out)
{
    if (s.empty())
        return false;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9')
            return false;
        uint64_t d = static_cast<uint64_t>(c - '0');
        if (v > kMaxUint64 / 10 ||
            (v == kMaxUint64 / 10 && d > kMaxUint64 % 10))
            return false;
        v = v * 10 + d;
    }
    out = v;
    return true;
}

inline std::string HexOffset(uint64_t offset)
{
    std::ostringstream os;
    os << std::hex << offset;
    return os.str();
}

inline bool ErrorsEqual(const Error &a, const Error &b)
{
    return a.type == b.type && a.pcs == b.pcs;
}

inline bool TrialsEqual(const Trial &a, const Trial &b)
{
    if (a.worker != b.worker || a.tick != b.tick || a.outcome != b.outcome ||
        a.errors.size() != b.errors.size())
        return false;
    for (size_t i = 0; i < a.errors.size(); ++i) {
        if (!ErrorsEqual(a.errors[i], b.errors[i]))
            return false;
    }
    return true;
}

inline const char *TypeName(const std::string &type)
{
    if (type == "UNADDRESSABLE ACCESS")
        return "UNADDR";
    if (type == "UNINITIALIZED READ")
        return "UNINIT";
    if (type == "INVALID HEAP ARGUMENT")
        return "HEAP";
    return nullptr;
}

/* Half-open overlap: [a_lo, a_hi) vs [b_lo, b_hi). */
inline bool RangesOverlap(uint64_t a_lo, uint64_t a_hi, uint64_t b_lo,
                          uint64_t b_hi)
{
    return a_lo < b_hi && b_lo < a_hi;
}

} /* namespace detail */

struct Ledger::Impl {
    struct Case {
        std::string id;
        uint64_t generation = 0;
        size_t repeats = 0;
        /* One entry per slot; nullptr means the slot is still missing. */
        std::vector<std::unique_ptr<Trial>> slots;
    };

    std::vector<Image> images;
    std::vector<Case> cases;

    Case *FindCase(const std::string &id)
    {
        for (Case &c : cases) {
            if (c.id == id)
                return &c;
        }
        return nullptr;
    }
    const Case *FindCase(const std::string &id) const
    {
        for (const Case &c : cases) {
            if (c.id == id)
                return &c;
        }
        return nullptr;
    }

    const Image *FindIdentity(const std::string &worker,
                              const std::string &token,
                              uint64_t generation) const
    {
        for (const Image &img : images) {
            if (img.worker == worker && img.token == token &&
                img.generation == generation)
                return &img;
        }
        return nullptr;
    }

    static bool SameImage(const Image &a, const Image &b)
    {
        return a.worker == b.worker && a.token == b.token &&
               a.build == b.build && a.generation == b.generation &&
               a.base == b.base && a.size == b.size && a.begin == b.begin &&
               a.end == b.end;
    }

    bool ValidateImage(const Image &img) const
    {
        if (!detail::ValidIdentifier(img.worker) ||
            !detail::ValidIdentifier(img.token) ||
            !detail::ValidIdentifier(img.build))
            return false;
        if (img.generation == 0 || img.size == 0 || img.begin >= img.end)
            return false;
        /* base + size must not wrap. */
        if (img.base > detail::kMaxUint64 - img.size)
            return false;
        return true;
    }

    bool AddImage(const Image &in)
    {
        if (!ValidateImage(in))
            return false;
        if (const Image *existing =
                FindIdentity(in.worker, in.token, in.generation)) {
            /* Identical full record is idempotent; any other content with
             * the same identity is a conflict. */
            return SameImage(*existing, in);
        }
        const uint64_t in_addr_end = in.base + in.size;
        for (const Image &img : images) {
            /* Same worker: no overlap in BOTH time and address. */
            if (img.worker == in.worker) {
                bool time_overlap = detail::RangesOverlap(
                    img.begin, img.end, in.begin, in.end);
                bool addr_overlap = detail::RangesOverlap(
                    img.base, img.base + img.size, in.base, in_addr_end);
                if (time_overlap && addr_overlap)
                    return false;
            }
            /* Same token (any worker): no overlap in time. */
            if (img.token == in.token &&
                detail::RangesOverlap(img.begin, img.end, in.begin, in.end))
                return false;
            /* Same worker/token: generation order must strictly match begin
             * order (an older declaration arriving late is still legal). */
            if (img.worker == in.worker && img.token == in.token) {
                if (img.generation < in.generation &&
                    !(img.begin < in.begin))
                    return false;
                if (in.generation < img.generation &&
                    !(in.begin < img.begin))
                    return false;
            }
        }
        images.push_back(in);
        return true;
    }

    bool ValidateTrial(const Trial &t) const
    {
        if (!detail::ValidIdentifier(t.worker))
            return false;
        if (t.outcome != Outcome::Complete &&
            t.outcome != Outcome::Timeout && t.outcome != Outcome::ToolError)
            return false;
        if (t.outcome != Outcome::Complete)
            return t.errors.empty();
        if (t.errors.size() > 1024)
            return false;
        for (const Error &e : t.errors) {
            const char *code = detail::TypeName(e.type);
            if (code == nullptr || e.pcs.empty() || e.pcs.size() > 64)
                return false;
        }
        return true;
    }

    /* Map one error at (worker, tick) to its stable text signature.
     * std::nullopt means at least one frame could not be resolved. */
    std::optional<std::string> MapError(const Error &e,
                                        const std::string &worker,
                                        uint64_t tick) const
    {
        const char *code = detail::TypeName(e.type);
        if (code == nullptr || e.pcs.empty())
            return std::nullopt;
        const bool heap = (e.type == "INVALID HEAP ARGUMENT");
        std::string sig = code;
        sig.push_back('|');
        for (size_t i = 0; i < e.pcs.size(); ++i) {
            const bool is_return = (i != 0) || heap;
            uint64_t pc = e.pcs[i];
            if (is_return) {
                /* A zero return address (including subtraction underflow)
                 * can never form a stable signature. */
                if (pc == 0)
                    return std::nullopt;
                --pc;
            }
            const Image *match = nullptr;
            for (const Image &img : images) {
                if (img.worker != worker)
                    continue;
                /* Both ranges half-open: the error must occur strictly
                 * within the image lifetime, and the adjusted PC within
                 * its address range. */
                if (!(img.begin <= tick && tick < img.end))
                    continue;
                if (!(img.base <= pc && pc < img.base + img.size))
                    continue;
                if (match != nullptr)
                    return std::nullopt; /* ambiguous mapping */
                match = &img;
            }
            if (match == nullptr)
                return std::nullopt;
            if (i != 0)
                sig.push_back(',');
            sig += match->build;
            sig.push_back('@');
            sig += detail::HexOffset(pc - match->base);
        }
        return sig;
    }

    /* Sorted unique signature set for one complete trial; nullopt if any
     * frame is unresolved. */
    std::optional<std::vector<std::string>>
    SignatureSet(const Trial &t) const
    {
        std::set<std::string> unique;
        for (const Error &e : t.errors) {
            auto sig = MapError(e, t.worker, t.tick);
            if (!sig.has_value())
                return std::nullopt;
            unique.insert(*sig);
        }
        return std::vector<std::string>(unique.begin(), unique.end());
    }

    enum class State { Pending, Clean, Stable, Flaky, Inconclusive };

    static const char *StateName(State s)
    {
        switch (s) {
        case State::Pending: return "PENDING";
        case State::Clean: return "CLEAN";
        case State::Stable: return "STABLE";
        case State::Flaky: return "FLAKY";
        case State::Inconclusive: return "INCONCLUSIVE";
        }
        return "?";
    }

    struct CaseStatus {
        State state;
        /* Valid unless Pending/Inconclusive; the consensus signature set. */
        std::vector<std::string> set;
    };

    CaseStatus StatusOf(const Case &c) const
    {
        for (const auto &slot : c.slots) {
            if (!slot)
                return {State::Pending, {}};
        }
        bool any_nonempty = false;
        std::vector<std::string> consensus;
        bool have_consensus = false;
        for (const auto &slot : c.slots) {
            if (slot->outcome != Outcome::Complete)
                return {State::Inconclusive, {}};
            auto mapped = SignatureSet(*slot);
            if (!mapped.has_value())
                return {State::Inconclusive, {}};
            if (!mapped->empty())
                any_nonempty = true;
            if (!have_consensus) {
                consensus = *mapped;
                have_consensus = true;
            } else if (consensus != *mapped) {
                return {State::Flaky, {}};
            }
        }
        if (!any_nonempty)
            return {State::Clean, {}};
        return {State::Stable, consensus};
    }

    std::string Report() const
    {
        if (cases.empty())
            return {};
        std::vector<const Case *> ordered;
        for (const Case &c : cases)
            ordered.push_back(&c);
        std::sort(ordered.begin(), ordered.end(),
                  [](const Case *a, const Case *b) { return a->id < b->id; });

        std::ostringstream out;
        std::map<std::string, std::set<std::string>> groups;
        for (const Case *c : ordered) {
            CaseStatus st = StatusOf(*c);
            out << "CASE " << c->id << ' ' << c->generation << ' '
                << StateName(st.state) << '\n';
            if (st.state == State::Stable) {
                for (const std::string &sig : st.set)
                    groups[sig].insert(c->id);
            }
        }
        for (const auto &group : groups) {
            out << "GROUP " << group.first;
            bool first = true;
            for (const std::string &id : group.second) {
                out << (first ? ' ' : ',') << id;
                first = false;
            }
            out << '\n';
        }
        return out.str();
    }
};

} /* namespace drreg */

#endif /* DRMEMORY_REGRESSION_IMPL_H */
