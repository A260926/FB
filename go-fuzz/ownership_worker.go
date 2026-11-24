// Copyright 2015 go-fuzz project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package main

import (
	"log"
	"time"
)

// ownershipStepTimeout bounds how long a step waits for its confirmation
// to be admitted. The normal Hub loop admits asynchronously; embedded
// users (tests, the example) have no loop and are admitted inline.
const ownershipStepTimeout = 10 * time.Second

func (hub *Hub) ownershipEnabled() bool {
	return hub.ownershipState() != nil
}

// OwnershipStep performs exactly one ownership iteration: claim a slot,
// apply one ordinary mutation to the owner's bytes, make one target call,
// then run the existing three-run triage and confirm the ticket through
// Hub admission. Sonar, versifier, smashing and minimization are never
// used. A step with no new coverage still completes and keeps its parent
// edge; there is no analogue of the old novelty early exit.
func (w *Worker) OwnershipStep() (OwnershipTicket, error) {
	ticket, err := w.hub.NextOwnership()
	if err != nil {
		return OwnershipTicket{}, err
	}

	ro := w.hub.ro.Load().(*ROData)
	// Exactly one ordinary mutation of the ticket's parent bytes.
	data := w.mutator.mutate(ticket.Data, ro)

	// One real target call through the production TestBinary.
	w.execs[execFuzz]++
	res, _, cover, _, output, crashed, hanged := w.coverBin.test(data)
	if crashed {
		// Benign targets do not crash; keep the existing crasher
		// bookkeeping and confirm via diagnostic-only feedback, which
		// records node/parent edges but never contributes coverage.
		w.noteCrasher(data, output, hanged)
		supp := extractSuppression(output)
		if err := w.hub.commitOwnership(ticket.ID, data, nil, supp, res); err != nil {
			return ticket, err
		}
		return ticket, w.waitOwnership(ticket.ID)
	}

	// Production novelty path. It may enqueue local triage via the old
	// weighted-mode queue, which ownership never drains; drop that entry
	// because triage is driven explicitly below for every step.
	w.noteNewInput(data, cover, res, 0, execFuzz)
	w.triageQueue = w.triageQueue[:0]

	// Existing three-run observation, with minimization and smashing
	// disabled (the mode performs neither). The result is tagged with
	// this ticket and sent through the ordinary new-input channel.
	w.ownershipCurrent = ticket.ID
	w.triageInput(CoordinatorInput{
		Data:      makeCopy(data),
		Type:      execFuzz,
		Minimized: true,
		Smashed:   true,
	})
	w.ownershipCurrent = 0

	// Either the Hub coordinator loop drains newInputC and commits via
	// the shared admission branch, or nobody is running the loop
	// (embedded users) and we drain it inline. Both paths enter through
	// Hub.commitOwnership, the single publish entry point.
	if !w.hub.ownershipTicketCompleted(ticket.ID) {
		deadline := time.Now().Add(ownershipStepTimeout)
		for time.Now().Before(deadline) {
			select {
			case inp := <-w.hub.newInputC:
				if inp.ownershipTicket == 0 {
					continue
				}
				if err := w.hub.commitOwnership(inp.ownershipTicket, inp.data, inp.cover, nil, inp.res); err != nil {
					log.Printf("ownership: inline admission rejected: %v", err)
				}
			default:
				if w.hub.ownershipTicketCompleted(ticket.ID) {
					return ticket, nil
				}
				time.Sleep(time.Millisecond)
			}
			if w.hub.ownershipTicketCompleted(ticket.ID) {
				return ticket, nil
			}
		}
	}

	// Triage can legitimately produce nothing (e.g. flaky crash):
	// confirm directly so the node/parent edge is never lost.
	if !w.hub.ownershipTicketCompleted(ticket.ID) {
		if err := w.hub.commitOwnership(ticket.ID, data, cover, nil, res); err != nil {
			return ticket, err
		}
	}
	return ticket, w.waitOwnership(ticket.ID)
}

// waitOwnership blocks until the ticket is confirmed or the timeout fires.
func (w *Worker) waitOwnership(id uint64) error {
	deadline := time.Now().Add(ownershipStepTimeout)
	for time.Now().Before(deadline) {
		if w.hub.ownershipTicketCompleted(id) {
			return nil
		}
		time.Sleep(time.Millisecond)
	}
	if w.hub.ownershipTicketCompleted(id) {
		return nil
	}
	return errOwnershipTimeout
}
