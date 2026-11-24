// Copyright 2015 go-fuzz project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package main

import (
	"bytes"
	"encoding/hex"
	"errors"
	"math"
	"sort"
	"sync"

	. "github.com/dvyukov/go-fuzz/go-fuzz-defs"
)

// Ownership mode: coverage feedback, instead of random weighted selection,
// decides which corpus seed is the parent of the next mutation. Every
// coverage slot has a single owner seed and a uint64 debt that drives
// scheduling. All state lives in the Hub; there is no persistence and no
// RPC surface for it.

// OwnershipTicket is a single claim on one coverage slot.
type OwnershipTicket struct {
	ID     uint64
	Parent string
	Data   []byte
	Slot   int
	Bucket byte
}

// OwnershipOwner describes the current owner of one coverage slot.
type OwnershipOwner struct {
	Slot   int
	Bucket byte
	Seed   string
	Debt   uint64
}

// OwnershipNode is a seed in the ownership lineage graph.
type OwnershipNode struct {
	Seed    string
	Parents []string
	Active  bool
}

// OwnershipDiagnostic is the representative seed for one suppression class.
type OwnershipDiagnostic struct {
	Key  string
	Seed string
}

// OwnershipView is an immutable, sorted copy of the ownership state.
type OwnershipView struct {
	Issued      uint64
	Completed   uint64
	Owners      []OwnershipOwner
	Nodes       []OwnershipNode
	Diagnostics []OwnershipDiagnostic
}

var (
	errOwnershipNotEnabled  = errors.New("ownership: mode is not enabled")
	errOwnershipEnabled     = errors.New("ownership: mode is already enabled")
	errOwnershipAfterClaim  = errors.New("ownership: mode cannot be enabled after the first claim")
	errOwnershipEmptyCorpus = errors.New("ownership: cannot enable without a triaged corpus")
	errOwnershipNoOwner     = errors.New("ownership: no seed currently owns a coverage slot")
	errOwnershipUnknownID   = errors.New("ownership: unknown ticket id")
	errOwnershipBadCover    = errors.New("ownership: cover must have CoverSize bytes")
	errOwnershipConflict    = errors.New("ownership: conflicting feedback for a completed ticket id")
	errOwnershipOverflow    = errors.New("ownership: ticket id or slot debt overflow")
	errOwnershipTimeout     = errors.New("ownership: confirmation was not admitted in time")
	errOwnershipExampleBin  = errors.New("ownership: GOFUZZ_BENIGN_BIN is not set")
)

// ownershipRecord is one seed (node) known to the scheduler. Nodes and
// parent edges are never removed, even after the seed loses all slots.
type ownershipRecord struct {
	sig      string
	data     []byte
	cover    []byte // raw per-slot counters, quantized only when compared
	parents  map[string]struct{}
	active   bool
	execTime uint64
}

type ownershipSlot struct {
	owner  string // hex SHA1 of the owner seed, "" when the slot is unowned
	bucket byte   // quantized bucket (roundUpCover) of the current owner
	debt   uint64
}

type ownershipTicketState struct {
	id     uint64
	parent string
	data   []byte
	slot   int
	bucket byte
	done   bool

	// Feedback fingerprint, used for idempotent duplicate confirmation.
	fpData    []byte
	fpCover   []byte
	fpDiag    []byte
	fpRes     int
	fpHasDiag bool
}

type ownershipDiagState struct {
	key  string
	seed string
	data []byte
}

type ownershipState struct {
	mu        sync.Mutex
	issued    uint64
	completed uint64

	records map[string]*ownershipRecord
	slots   [CoverSize]ownershipSlot
	tickets map[uint64]*ownershipTicketState
	diags   map[string]*ownershipDiagState
}

// seedLess implements the deterministic total order between seeds:
// shorter bytes win, ties break on the lowercase SHA1.
func seedLess(lenA int, sigA string, lenB int, sigB string) bool {
	if lenA != lenB {
		return lenA < lenB
	}
	return sigA < sigB
}

// sigHex returns the lowercase hex SHA1 of data as a string.
func sigHex(data []byte) string {
	sig := hash(data)
	return hex.EncodeToString(sig[:])
}

// newOwnershipState seeds the scheduler from an already triaged ROData.
// Ownership of every covered slot is derived purely from coverage and
// seed identity, so the result does not depend on corpus ordering.
func newOwnershipState(ro *ROData) *ownershipState {
	st := &ownershipState{
		records: make(map[string]*ownershipRecord),
		tickets: make(map[uint64]*ownershipTicketState),
		diags:   make(map[string]*ownershipDiagState),
	}
	for _, inp := range ro.corpus {
		data := makeCopy(inp.data)
		sig := sigHex(data)
		if _, ok := st.records[sig]; ok {
			continue
		}
		cover := make([]byte, CoverSize)
		copy(cover, inp.cover)
		execTime := inp.execTime
		if execTime == 0 {
			execTime = 1
		}
		st.records[sig] = &ownershipRecord{
			sig:      sig,
			data:     data,
			cover:    cover,
			parents:  make(map[string]struct{}),
			execTime: execTime,
		}
	}
	for i := 0; i < CoverSize; i++ {
		bestSig := ""
		bestBucket := byte(0)
		bestLen := 0
		for sig, rec := range st.records {
			v := rec.cover[i]
			if v == 0 {
				continue
			}
			b := roundUpCover(v)
			if b > bestBucket || b == bestBucket &&
				(bestSig == "" || seedLess(len(rec.data), sig, bestLen, bestSig)) {
				bestSig = sig
				bestBucket = b
				bestLen = len(rec.data)
			}
		}
		if bestSig != "" {
			st.slots[i] = ownershipSlot{owner: bestSig, bucket: bestBucket}
		}
	}
	st.recomputeActive()
	return st
}

// EnableOwnership turns on ownership scheduling. It is allowed exactly
// once, before any ticket has been issued, and fails without leaving
// partially initialized state behind.
func (hub *Hub) EnableOwnership() error {
	hub.ownershipMu.Lock()
	defer hub.ownershipMu.Unlock()
	if hub.ownership != nil {
		st := hub.ownership
		st.mu.Lock()
		issued := st.issued
		st.mu.Unlock()
		if issued != 0 {
			return errOwnershipAfterClaim
		}
		return errOwnershipEnabled
	}
	ro := hub.ro.Load().(*ROData)
	if len(ro.corpus) == 0 {
		return errOwnershipEmptyCorpus
	}
	st := newOwnershipState(ro)
	owned := 0
	for i := range st.slots {
		if st.slots[i].owner != "" {
			owned++
		}
	}
	if owned == 0 {
		return errOwnershipNoOwner
	}
	hub.ownership = st
	return nil
}

func (hub *Hub) ownershipState() *ownershipState {
	hub.ownershipMu.Lock()
	st := hub.ownership
	hub.ownershipMu.Unlock()
	return st
}

// NextOwnership claims the slot with the smallest debt (slot index breaks
// ties), increments that debt and returns its current owner as parent.
// Already issued parent identities stay stable even if ownership later
// migrates, because the ticket snapshots the parent.
func (hub *Hub) NextOwnership() (OwnershipTicket, error) {
	st := hub.ownershipState()
	if st == nil {
		return OwnershipTicket{}, errOwnershipNotEnabled
	}
	st.mu.Lock()
	defer st.mu.Unlock()

	slot := -1
	for i := range st.slots {
		if st.slots[i].owner == "" {
			continue
		}
		if slot == -1 || st.slots[i].debt < st.slots[slot].debt {
			slot = i
		}
	}
	if slot == -1 {
		return OwnershipTicket{}, errOwnershipNoOwner
	}
	if st.issued == math.MaxUint64 || st.slots[slot].debt == math.MaxUint64 {
		return OwnershipTicket{}, errOwnershipOverflow
	}

	st.issued++
	st.slots[slot].debt++
	rec := st.records[st.slots[slot].owner]
	id := st.issued
	st.tickets[id] = &ownershipTicketState{
		id:     id,
		parent: rec.sig,
		data:   makeCopy(rec.data),
		slot:   slot,
		bucket: st.slots[slot].bucket,
	}
	return OwnershipTicket{
		ID:     id,
		Parent: rec.sig,
		Data:   makeCopy(rec.data),
		Slot:   slot,
		Bucket: st.slots[slot].bucket,
	}, nil
}

// ObserveOwnership commits feedback for a previously issued ticket.
// Normal feedback requires a CoverSize cover; negative results and
// diagnostics only record the node/parent edge. Duplicate feedback with
// identical content is idempotent, conflicts and unknown ids error out
// without touching state.
func (hub *Hub) ObserveOwnership(id uint64, data, cover, diagnostic []byte, result int) error {
	return hub.commitOwnership(id, data, cover, diagnostic, result)
}

// commitOwnership is the single admission entry point shared by
// ObserveOwnership, the worker step and the Hub.loop new-input branch.
// It applies the transaction and, on a first-time completion, republishes
// the live ROData corpus and cover tables atomically.
func (hub *Hub) commitOwnership(id uint64, data, cover, diagnostic []byte, result int) error {
	st := hub.ownershipState()
	if st == nil {
		return errOwnershipNotEnabled
	}
	st.mu.Lock()
	defer st.mu.Unlock()
	before := st.completed
	if err := st.observe(id, data, cover, diagnostic, result); err != nil {
		return err
	}
	if st.completed != before {
		st.republish(hub)
	}
	return nil
}

func (hub *Hub) ownershipTicketCompleted(id uint64) bool {
	st := hub.ownershipState()
	if st == nil {
		return false
	}
	st.mu.Lock()
	defer st.mu.Unlock()
	tk, ok := st.tickets[id]
	return ok && tk.done
}

// observe applies one feedback transaction. Caller holds st.mu.
func (st *ownershipState) observe(id uint64, data, cover, diagnostic []byte, result int) error {
	tk, ok := st.tickets[id]
	if !ok {
		return errOwnershipUnknownID
	}
	hasDiag := len(diagnostic) > 0
	normal := result >= 0 && !hasDiag

	if tk.done {
		if tk.fingerprintMatches(data, cover, diagnostic, result, hasDiag) {
			return nil
		}
		return errOwnershipConflict
	}
	if normal && len(cover) != CoverSize {
		return errOwnershipBadCover
	}

	dataCopy := makeCopy(data)
	var coverCopy []byte
	if cover != nil {
		coverCopy = makeCopy(cover)
	}
	diagCopy := makeCopy(diagnostic)
	sig := sigHex(dataCopy)

	rec := st.records[sig]
	if rec == nil {
		rec = &ownershipRecord{
			sig:      sig,
			data:     dataCopy,
			cover:    make([]byte, CoverSize),
			parents:  make(map[string]struct{}),
			execTime: 1,
		}
		st.records[sig] = rec
	}

	// Lineage: union of parent edges, self edges are ignored. Nodes and
	// edges are retained even when the seed later loses every slot.
	if tk.parent != "" && tk.parent != sig {
		rec.parents[tk.parent] = struct{}{}
	}

	if normal {
		// Same-SHA1 results merge per slot at the highest raw counter.
		for i, v := range coverCopy {
			if v != 0 && v > rec.cover[i] {
				rec.cover[i] = v
			}
		}
		for i, v := range coverCopy {
			if v == 0 {
				continue
			}
			b := roundUpCover(v)
			slot := &st.slots[i]
			switch {
			case slot.owner == "":
				// A slot seen for the first time starts at the smallest
				// debt among the slots already active; zero if none.
				slot.debt = st.minActiveDebt()
				slot.owner = sig
				slot.bucket = b
			case b > slot.bucket:
				// Bucket upgrade: debt is deliberately preserved.
				slot.bucket = b
				slot.owner = sig
			case b == slot.bucket:
				cur := st.records[slot.owner]
				if seedLess(len(dataCopy), sig, len(cur.data), cur.sig) {
					slot.owner = sig
				}
			}
		}
	}

	if hasDiag {
		// Diagnostics never contribute coverage; the classification is
		// whatever extractSuppression derives from the supplied text.
		key := string(extractSuppression(diagCopy))
		d := st.diags[key]
		if d == nil {
			st.diags[key] = &ownershipDiagState{key: key, seed: sig, data: dataCopy}
		} else if seedLess(len(dataCopy), sig, len(d.data), d.seed) {
			d.seed = sig
			d.data = dataCopy
		}
	}

	st.recomputeActive()
	st.completed++
	tk.done = true
	tk.fpData = dataCopy
	tk.fpCover = coverCopy
	tk.fpDiag = diagCopy
	tk.fpRes = result
	tk.fpHasDiag = hasDiag
	return nil
}

func (tk *ownershipTicketState) fingerprintMatches(data, cover, diagnostic []byte, result int, hasDiag bool) bool {
	if tk.fpRes != result || tk.fpHasDiag != hasDiag {
		return false
	}
	if !bytes.Equal(tk.fpData, data) {
		return false
	}
	if !bytes.Equal(tk.fpCover, cover) {
		return false
	}
	return bytes.Equal(tk.fpDiag, diagnostic)
}

// minActiveDebt returns the smallest debt among currently owned slots.
// The empty set yields 0.
func (st *ownershipState) minActiveDebt() uint64 {
	min := uint64(0)
	found := false
	for i := range st.slots {
		if st.slots[i].owner == "" {
			continue
		}
		if !found || st.slots[i].debt < min {
			min = st.slots[i].debt
			found = true
		}
	}
	return min
}

// recomputeActive derives the active flag purely from current slot
// ownership, so a seed that lost all its slots stops being selectable
// while its node and parent edges remain.
func (st *ownershipState) recomputeActive() {
	for _, rec := range st.records {
		rec.active = false
	}
	for i := range st.slots {
		if owner := st.slots[i].owner; owner != "" {
			st.records[owner].active = true
		}
	}
}

// republish publishes a fresh ROData whose corpus contains exactly the
// active seeds, plus the quantized bucket table as both corpusCover and
// maxCover. Old ROData values and their byte/cover slices stay untouched;
// every slice handed out is newly allocated.
func (st *ownershipState) republish(hub *Hub) {
	active := make([]*ownershipRecord, 0, len(st.records))
	for _, rec := range st.records {
		if rec.active {
			active = append(active, rec)
		}
	}
	sort.Slice(active, func(i, j int) bool { return active[i].sig < active[j].sig })

	ro := hub.ro.Load().(*ROData)
	ro1 := new(ROData)
	*ro1 = *ro

	corpus := make([]Input, 0, len(active))
	scoreSum := 0
	for _, rec := range active {
		coverSize := 0
		for _, v := range rec.cover {
			if v != 0 {
				coverSize++
			}
		}
		scoreSum += defScore
		corpus = append(corpus, Input{
			data:            makeCopy(rec.data),
			cover:           makeCopy(rec.cover),
			coverSize:       coverSize,
			typ:             execFuzz,
			execTime:        rec.execTime,
			score:           defScore,
			runningScoreSum: scoreSum,
		})
	}

	corpusCover := make([]byte, CoverSize)
	for i := range st.slots {
		if st.slots[i].owner != "" {
			corpusCover[i] = st.slots[i].bucket
		}
	}
	ro1.corpus = corpus
	ro1.corpusCover = corpusCover
	hub.ro.Store(ro1)
	hub.maxCover.Store(makeCopy(corpusCover))
}

// OwnershipSnapshot returns a fully detached, sorted copy of the state.
// Owners are ordered by slot, nodes by SHA1 and diagnostics by key.
func (hub *Hub) OwnershipSnapshot() OwnershipView {
	st := hub.ownershipState()
	if st == nil {
		return OwnershipView{Owners: []OwnershipOwner{}, Nodes: []OwnershipNode{}, Diagnostics: []OwnershipDiagnostic{}}
	}
	st.mu.Lock()
	defer st.mu.Unlock()

	view := OwnershipView{
		Issued:      st.issued,
		Completed:   st.completed,
		Owners:      make([]OwnershipOwner, 0, CoverSize),
		Nodes:       make([]OwnershipNode, 0, len(st.records)),
		Diagnostics: make([]OwnershipDiagnostic, 0, len(st.diags)),
	}
	for i := range st.slots {
		if st.slots[i].owner == "" {
			continue
		}
		view.Owners = append(view.Owners, OwnershipOwner{
			Slot:   i,
			Bucket: st.slots[i].bucket,
			Seed:   st.slots[i].owner,
			Debt:   st.slots[i].debt,
		})
	}
	sigs := make([]string, 0, len(st.records))
	for sig := range st.records {
		sigs = append(sigs, sig)
	}
	sort.Strings(sigs)
	for _, sig := range sigs {
		rec := st.records[sig]
		parents := make([]string, 0, len(rec.parents))
		for p := range rec.parents {
			parents = append(parents, p)
		}
		sort.Strings(parents)
		view.Nodes = append(view.Nodes, OwnershipNode{
			Seed:    sig,
			Parents: parents,
			Active:  rec.active,
		})
	}
	keys := make([]string, 0, len(st.diags))
	for key := range st.diags {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	for _, key := range keys {
		view.Diagnostics = append(view.Diagnostics, OwnershipDiagnostic{
			Key:  key,
			Seed: st.diags[key].seed,
		})
	}
	return view
}
