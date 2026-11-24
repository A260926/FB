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

/* Offline memory-safety regression ledger.  See
 * task_support/regression-contract.md for the full contract.  All inputs are
 * inert simulated log text and metadata; this module never executes a target.
 */

#ifndef DRMEMORY_REGRESSION_H
#define DRMEMORY_REGRESSION_H 1

#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>

namespace drreg {

/* Outcome of one whole trial run. */
enum class Outcome { Complete, Timeout, ToolError };

/* One loaded module image: who loaded it (worker), its token/generation
 * identity, the trusted build artifact, the half-open address range
 * [base, base+size) and the half-open lifetime [begin, end).
 */
struct Image {
    std::string worker;
    std::string token;
    std::string build;
    uint64_t generation;
    uint64_t base;
    uint64_t size;
    uint64_t begin;
    uint64_t end;
};

/* One native error with its raw PCs in native frame order. */
struct Error {
    std::string type;
    std::vector<uint64_t> pcs;
};

/* One complete trial result for a candidate slot. */
struct Trial {
    std::string worker;
    uint64_t tick;
    Outcome outcome;
    std::vector<Error> errors;
};

/* Owns the whole timeline (images), candidates, raw trials and derived
 * consensus.  Not copyable; every mutating bool operation is all-or-nothing
 * and leaves the object unchanged on failure.
 */
class Ledger {
public:
    Ledger();
    ~Ledger();
    Ledger(const Ledger &) = delete;
    Ledger &operator=(const Ledger &) = delete;

    bool AddImage(const Image &);
    bool Begin(const std::string &id, uint64_t generation, size_t repeats);
    bool Observe(const std::string &id, uint64_t generation, size_t slot,
                 const Trial &);
    std::string Report() const;
    bool Save(const std::string &path) const;
    bool Load(const std::string &path);

private:
    struct Impl;
    Impl *impl_;
};

} /* namespace drreg */

#endif /* DRMEMORY_REGRESSION_H */
