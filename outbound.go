// Copyright 2019 The mqtt-go authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package mqtt

import (
	"errors"
	"io"
	"sync"
	"sync/atomic"
)

// ErrPacketIdentifiersExhausted is returned when every packet identifier
// (1 to 65535) of the transport is already owned by an in-flight operation.
var ErrPacketIdentifiersExhausted = errors.New("packet identifiers exhausted")

// ErrPacketIdentifierInUse is returned when an explicitly specified packet
// identifier is already owned by another in-flight operation.
var ErrPacketIdentifierInUse = errors.New("packet identifier already in use")

const maxPacketIdentifier = 0xFFFF

type outboundKind uint8

const (
	outboundPublish outboundKind = iota
	outboundSubscribe
	outboundUnsubscribe
)

type outboundPhase uint8

const (
	phaseIdle outboundPhase = iota
	phasePubAck
	phasePubRec
	phasePubComp
	phaseSubAck
	phaseUnsubAck
	phaseDone
)

// outboundOp is the ownership record of one logical outgoing operation.
// The packet identifier, the protocol phase and the channels waiting for the
// broker's confirmation all belong to it, and are reused when the same logical
// operation is retried on the same transport generation.
//
// A retry on a different transport generation reserves an identifier from
// that transport's namespace instead; leases are never transferred between
// transports.
type outboundOp struct {
	mu sync.Mutex

	kind      outboundKind
	qos       QoS
	explicit  bool
	requested uint16

	id        uint16
	phase     outboundPhase
	reg       *signaller
	transport io.ReadWriteCloser
	released  bool
	done      bool
	// pubRel records that PUBREC has already been consumed by this logical
	// operation, so a retry resumes at PUBREL instead of restarting PUBLISH.
	pubRel bool

	chPubAck   chan *pktPubAck
	chPubRec   chan *pktPubRec
	chPubComp  chan *pktPubComp
	chSubAck   chan *pktSubAck
	chUnsubAck chan *pktUnsubAck
}

func newOutboundOp(kind outboundKind, qos QoS) *outboundOp {
	return &outboundOp{kind: kind, qos: qos}
}

// initialPhase returns the protocol phase entered by a newly written request.
func (o *outboundOp) initialPhase() outboundPhase {
	switch o.kind {
	case outboundPublish:
		if o.qos == QoS2 {
			return phasePubRec
		}
		return phasePubAck
	case outboundSubscribe:
		return phaseSubAck
	default:
		return phaseUnsubAck
	}
}

// setPhaseLocked prepares the waiting channel of the protocol phase.
// The channel is created before the corresponding packet is written, so there
// is no window in which a confirmation could not be matched.
func (o *outboundOp) setPhaseLocked(p outboundPhase) {
	o.phase = p
	switch p {
	case phasePubAck:
		o.chPubAck = make(chan *pktPubAck, 1)
	case phasePubRec:
		o.chPubRec = make(chan *pktPubRec, 1)
	case phasePubComp:
		o.chPubComp = make(chan *pktPubComp, 1)
	case phaseSubAck:
		o.chSubAck = make(chan *pktSubAck, 1)
	case phaseUnsubAck:
		o.chUnsubAck = make(chan *pktUnsubAck, 1)
	}
}

// acquire reserves a packet identifier for the operation on the transport
// currently installed on c.
//
// If the operation already owns a lease on that same transport generation,
// its identifier, phase and waiting channels are reused. Otherwise an
// identifier is allocated from the new transport's namespace and the operation
// restarts in its initial phase there.
func (o *outboundOp) acquire(c *BaseClient) error {
	sig, err := c.signaller()
	if err != nil {
		return err
	}

	o.mu.Lock()
	if o.reg == sig && !o.released {
		o.mu.Unlock()
		return nil
	}
	if o.done {
		// A terminal acknowledgement already completed this logical
		// operation. It must never re-enter the namespace.
		o.mu.Unlock()
		return nil
	}
	oldReg := o.reg
	o.mu.Unlock()

	// Detach from the previous transport generation before touching the new
	// one so that two transport locks are never held at the same time.
	if oldReg != nil {
		oldReg.releaseOp(o)
	}

	sig.mu.Lock()
	defer sig.mu.Unlock()

	o.mu.Lock()
	defer o.mu.Unlock()

	if o.done {
		return nil
	}
	if o.reg == sig && !o.released {
		return nil
	}

	if sig.used == nil {
		sig.used = make(map[uint16]*outboundOp)
	}

	var id uint16
	if o.explicit {
		if o.requested == 0 {
			return wrapError(ErrPacketIdentifierInUse, "reserving packet identifier 0")
		}
		if _, ok := sig.used[o.requested]; ok {
			return wrapErrorf(
				ErrPacketIdentifierInUse,
				"packet identifier %d is already in use", o.requested,
			)
		}
		id = o.requested
	} else {
		id, err = c.allocIDLocked(sig)
		if err != nil {
			return err
		}
	}

	o.id = id
	o.reg = sig
	o.transport = c.Transport
	o.released = false
	o.done = false
	o.chPubAck = nil
	o.chPubRec = nil
	o.chPubComp = nil
	o.chSubAck = nil
	o.chUnsubAck = nil
	o.setPhaseLocked(o.initialPhase())
	if o.kind == outboundPublish && o.qos == QoS2 {
		// Both channels are prepared up-front. The identifier stays owned
		// while switching from PUBREC to PUBCOMP, so it cannot be handed to
		// an unrelated request in the transition.
		o.chPubComp = make(chan *pktPubComp, 1)
		if o.pubRel {
			// PUBREC was already consumed before this retry; resume at the
			// PUBREL/PUBCOMP phase on the new transport.
			o.phase = phasePubComp
		}
	}
	sig.used[id] = o
	return nil
}

// allocIDLocked scans the namespace of the transport for a free identifier.
// sig.mu must be held. Identifier 0 is never allocated, and every candidate
// is checked against the in-flight set, so the cursor may wrap around freely.
func (c *BaseClient) allocIDLocked(sig *signaller) (uint16, error) {
	if len(sig.used) >= maxPacketIdentifier {
		return 0, wrapError(
			ErrPacketIdentifiersExhausted, "allocating packet identifier")
	}

	for i := 0; i < maxPacketIdentifier; i++ {
		id := uint16(atomic.AddUint32(&c.idLast, 1))
		if id == 0 {
			continue
		}
		if _, ok := sig.used[id]; ok {
			continue
		}
		return id, nil
	}
	return 0, wrapError(
		ErrPacketIdentifiersExhausted, "allocating packet identifier")
}

func (o *outboundOp) currentID() uint16 {
	o.mu.Lock()
	defer o.mu.Unlock()
	return o.id
}

func (o *outboundOp) phaseSnapshot() outboundPhase {
	o.mu.Lock()
	defer o.mu.Unlock()
	return o.phase
}

func (o *outboundOp) isDone() bool {
	o.mu.Lock()
	defer o.mu.Unlock()
	return o.done
}

func (o *outboundOp) pubAckChannel() <-chan *pktPubAck {
	o.mu.Lock()
	defer o.mu.Unlock()
	return o.chPubAck
}

func (o *outboundOp) pubRecChannel() <-chan *pktPubRec {
	o.mu.Lock()
	defer o.mu.Unlock()
	return o.chPubRec
}

func (o *outboundOp) pubCompChannel() <-chan *pktPubComp {
	o.mu.Lock()
	defer o.mu.Unlock()
	return o.chPubComp
}

func (o *outboundOp) subAckChannel() <-chan *pktSubAck {
	o.mu.Lock()
	defer o.mu.Unlock()
	return o.chSubAck
}

func (o *outboundOp) unsubAckChannel() <-chan *pktUnsubAck {
	o.mu.Lock()
	defer o.mu.Unlock()
	return o.chUnsubAck
}

// resumePubComp moves a QoS2 operation that already consumed PUBREC into the
// PUBCOMP phase. It keeps the identifier owned by this operation and is a
// no-op once the operation is already in that phase.
func (o *outboundOp) resumePubComp() {
	o.mu.Lock()
	defer o.mu.Unlock()
	o.pubRel = true
	if o.phase == phasePubRec || o.phase == phaseIdle {
		o.phase = phasePubComp
	}
	if o.chPubComp == nil {
		o.chPubComp = make(chan *pktPubComp, 1)
	}
}

// releaseOp removes the operation from the registry if it is still the owner
// of its identifier. A newer generation's registration is never touched.
func (s *signaller) releaseOp(o *outboundOp) {
	s.mu.Lock()
	defer s.mu.Unlock()

	o.mu.Lock()
	defer o.mu.Unlock()

	if o.reg == s {
		if s.used[o.id] == o {
			delete(s.used, o.id)
		}
		o.reg = nil
		o.released = true
	}
}

// abortTransport closes the transport generation this request was written to
// when its packet boundary can no longer be trusted, then releases all
// remaining leases of that generation. A newer generation's transport and
// leases are untouched.
func (o *outboundOp) abortTransport() {
	o.mu.Lock()
	sig := o.reg
	transport := o.transport
	o.mu.Unlock()
	if transport != nil {
		_ = transport.Close()
	}
	cleanupOutbound(sig)
}

// cleanupOutbound releases every lease registered on the given transport
// generation. Leases belonging to a newer generation are untouched.
func cleanupOutbound(sig *signaller) {
	if sig == nil {
		return
	}

	sig.mu.Lock()
	defer sig.mu.Unlock()

	for id, o := range sig.used {
		o.mu.Lock()
		if o.reg == sig {
			o.reg = nil
			o.released = true
			o.phase = phaseDone
		}
		o.mu.Unlock()
		delete(sig.used, id)
	}
}
