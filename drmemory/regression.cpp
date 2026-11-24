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

/* Public Ledger operations: timeline registration, candidate lifecycle,
 * trial observation and reporting.  Persistence lives in
 * regression_store.cpp.
 */

#include "drmemory/regression_impl.h"

namespace drreg {

Ledger::Ledger()
    : impl_(new Impl())
{
}

Ledger::~Ledger() = default;

bool Ledger::AddImage(const Image &in)
{
    /* Validate against a throwaway copy path: Impl::AddImage only appends
     * after every check, so a refusal never mutates state. */
    return impl_->AddImage(in);
}

bool Ledger::Begin(const std::string &id, uint64_t generation, size_t repeats)
{
    if (!detail::ValidIdentifier(id) || generation == 0 || repeats == 0 ||
        repeats > 64)
        return false;

    Impl::Case *existing = impl_->FindCase(id);
    if (existing != nullptr) {
        if (existing->generation == generation) {
            /* Same generation must keep the same slot count; an identical
             * Begin is idempotent. */
            return existing->repeats == repeats;
        }
        if (generation < existing->generation)
            return false;
        /* Higher generation replaces the whole trial state; images stay
         * global and group contributions are recomputed in Report. */
        existing->generation = generation;
        existing->repeats = repeats;
        existing->slots.clear();
        existing->slots.resize(repeats);
        return true;
    }

    Impl::Case fresh;
    fresh.id = id;
    fresh.generation = generation;
    fresh.repeats = repeats;
    fresh.slots.resize(repeats);
    impl_->cases.push_back(std::move(fresh));
    return true;
}

bool Ledger::Observe(const std::string &id, uint64_t generation, size_t slot,
                     const Trial &trial)
{
    if (!detail::ValidIdentifier(id))
        return false;
    Impl::Case *c = impl_->FindCase(id);
    if (c == nullptr || generation != c->generation || slot >= c->repeats)
        return false;
    if (!impl_->ValidateTrial(trial))
        return false;
    /* "valid worker" means a well-formed identifier; a worker need not have
     * declared any image yet (late declarations must still resolve earlier
     * trials), so mapping failures surface later as INCONCLUSIVE. */
    if (c->slots[slot]) {
        /* Repeated reports are idempotent only when identical; any conflict
         * leaves the stored record and the state untouched. */
        return detail::TrialsEqual(*c->slots[slot], trial);
    }

    auto captured = std::unique_ptr<Trial>(new Trial(trial));
    c->slots[slot] = std::move(captured);
    return true;
}

std::string Ledger::Report() const
{
    return impl_->Report();
}

} /* namespace drreg */
