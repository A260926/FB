// Copyright 2018 Adam Tauber
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package colly

import (
	"sync"
)

// successfulVisitTracker keeps the request identities which are currently
// being processed when SuccessfulVisits is enabled. Entries are shared by a
// Collector and its clones, so only one of them can process a given identity
// at a time. A tracker is never shared by independent collectors.
type successfulVisitTracker struct {
	lock   sync.Mutex
	owners map[uint64]*visitClaim
}

// visitClaim is the set of request identities reserved by a single scraping
// chain. A chain consists of the initially requested URL and every URL that
// was followed through redirects.
type visitClaim struct {
	// released marks a claim whose identities have been either committed to
	// the storage or dropped after a failure. It is guarded by the tracker
	// lock.
	released bool
	hashes   map[uint64]struct{}
}

func newSuccessfulVisitTracker() *successfulVisitTracker {
	return &successfulVisitTracker{
		owners: make(map[uint64]*visitClaim),
	}
}

func (t *successfulVisitTracker) newClaim() *visitClaim {
	return &visitClaim{hashes: make(map[uint64]struct{})}
}

// reserve adds hash to claim. It reports false when another in-flight chain
// already owns the identity. Reserving an identity already owned by claim
// itself is allowed, which is required for redirect cycles.
func (t *successfulVisitTracker) reserve(claim *visitClaim, hash uint64) bool {
	t.lock.Lock()
	defer t.lock.Unlock()
	if claim.released {
		return false
	}
	if owner, ok := t.owners[hash]; ok && owner != claim {
		return false
	}
	t.owners[hash] = claim
	claim.hashes[hash] = struct{}{}
	return true
}

// hashes returns a snapshot of the identities reserved by claim.
func (t *successfulVisitTracker) hashes(claim *visitClaim) []uint64 {
	t.lock.Lock()
	defer t.lock.Unlock()
	hashes := make([]uint64, 0, len(claim.hashes))
	for hash := range claim.hashes {
		hashes = append(hashes, hash)
	}
	return hashes
}

// isReleased reports whether the claim has already been committed or dropped.
func (t *successfulVisitTracker) isReleased(claim *visitClaim) bool {
	t.lock.Lock()
	defer t.lock.Unlock()
	return claim.released
}

// release drops every identity still owned by claim. It is idempotent, so a
// request that has already released its identities before invoking an error
// callback cannot clear the reservations of a re-entrant request.
func (t *successfulVisitTracker) release(claim *visitClaim) {
	t.lock.Lock()
	defer t.lock.Unlock()
	if claim.released {
		return
	}
	claim.released = true
	for hash := range claim.hashes {
		if t.owners[hash] == claim {
			delete(t.owners, hash)
		}
	}
}
