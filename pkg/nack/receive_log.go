// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package nack

import (
	"sync"

	"github.com/pion/interceptor/internal/rtpbuffer"
	"github.com/pion/interceptor/internal/validation"
)

// receiveLogSlot records the full 16-bit sequence number that currently owns a
// window slot. Storing the identity prevents an out-of-window or re-wrapped
// sequence number that aliases the same slot from mutating another packet's
// received state.
type receiveLogSlot struct {
	valid bool
	seq   uint16
}

type receiveLog struct {
	slots           []receiveLogSlot
	size            uint16
	end             uint16
	started         bool
	first           uint16
	lastConsecutive uint16
	m               sync.RWMutex
}

func newReceiveLog(size uint16) (*receiveLog, error) {
	if err := validation.IsPowerOfTwo(size); err != nil {
		return nil, err
	}

	return &receiveLog{
		slots: make([]receiveLogSlot, size),
		size:  size,
	}, nil
}

func (s *receiveLog) add(seq uint16) {
	s.m.Lock()
	defer s.m.Unlock()

	if !s.started {
		s.setReceived(seq)
		s.end = seq
		s.started = true
		s.first = seq
		s.lastConsecutive = seq

		return
	}

	diff := seq - s.end
	switch {
	case diff == 0:
		// the current end is always recorded, so a duplicate never rewrites
		// another sequence number's slot
		return
	case diff < rtpbuffer.Uint16SizeHalf:
		// positive diff: seq is ahead of end (counting for rollovers).
		// the half-circle value (diff == 32768) is ambiguous and is ignored.
		for i := s.end + 1; i != seq; i++ {
			// clear packets between end and seq (these may contain packets from a "size" ago)
			s.delReceived(i)
		}
		s.end = seq

		if s.lastConsecutive+1 == seq {
			s.lastConsecutive = seq
		} else if seq-s.lastConsecutive > s.size {
			s.lastConsecutive = seq - s.size
			s.fixLastConsecutive() // there might be valid packets at the beginning of the buffer now
		}

		s.setReceived(seq)
	default:
		// negative diff: seq is an older packet, it may only fill a gap that is
		// still retained in the current window and not older than the first
		// packet of this binding. Out-of-window and pre-binding packets are
		// dropped without touching any slot.
		if !s.inWindowLocked(seq) || !s.withinBindingLocked(seq) {
			return
		}

		if s.lastConsecutive+1 == seq {
			s.lastConsecutive = seq
			s.fixLastConsecutive() // there might be other valid packets after seq
		}

		s.setReceived(seq)
	}
}

func (s *receiveLog) get(seq uint16) bool {
	s.m.RLock()
	defer s.m.RUnlock()

	if !s.inWindowLocked(seq) {
		return false
	}

	return s.getReceived(seq)
}

// trackedMissing reports whether seq is still a live gap of the current
// binding: retained in the window, not older than the binding's first packet
// and not received yet.
func (s *receiveLog) trackedMissing(seq uint16) bool {
	s.m.RLock()
	defer s.m.RUnlock()

	if !s.started || !s.inWindowLocked(seq) || !s.withinBindingLocked(seq) {
		return false
	}

	return !s.getReceived(seq)
}

// inWindowLocked reports whether seq is inside the currently retained window:
// newer than end-size and no newer than end, in RTP 16-bit modulo space.
func (s *receiveLog) inWindowLocked(seq uint16) bool {
	diff := s.end - seq
	if diff == 0 {
		return true
	}
	// diff in [1, size) is still retained; a diff at or past half the circle
	// means seq is actually ahead of end, not behind it.
	return diff < rtpbuffer.Uint16SizeHalf && diff < s.size
}

// withinBindingLocked reports whether seq is not older than the first packet
// observed by the current binding. Once the stream has moved half a circle or
// more away from that first packet, its circular identity is ambiguous and
// every packet still retained in the window is treated as younger.
func (s *receiveLog) withinBindingLocked(seq uint16) bool {
	if s.end-s.first >= rtpbuffer.Uint16SizeHalf {
		return true
	}

	diff := seq - s.first

	return diff < rtpbuffer.Uint16SizeHalf
}

func (s *receiveLog) missingSeqNumbers(skipLastN uint16, missingPacketSeqNums []uint16) []uint16 {
	s.m.RLock()
	defer s.m.RUnlock()

	if !s.started {
		return nil
	}

	until := s.end - skipLastN
	if until-s.lastConsecutive >= rtpbuffer.Uint16SizeHalf {
		// until < s.lastConsecutive (counting for rollover)
		return nil
	}

	// only slots still retained in the window can be meaningful gaps, so the
	// scan can never produce more than size-1 entries.
	start := s.lastConsecutive + 1
	windowHead := s.end - s.size + 1
	if start-windowHead >= rtpbuffer.Uint16SizeHalf {
		// windowHead is ahead of start (counting for rollover)
		start = windowHead
	}
	if until+1-start >= rtpbuffer.Uint16SizeHalf {
		// the scan end lies behind the window head (e.g. a large skipLastN)
		return nil
	}

	c := 0
	for i := start; i != until+1; i++ {
		if !s.getReceived(i) {
			missingPacketSeqNums[c] = i
			c++
		}
	}

	return missingPacketSeqNums[:c]
}

func (s *receiveLog) setReceived(seq uint16) {
	pos := seq % s.size
	s.slots[pos] = receiveLogSlot{valid: true, seq: seq}
}

func (s *receiveLog) delReceived(seq uint16) {
	pos := seq % s.size
	if slot := s.slots[pos]; slot.valid && slot.seq == seq {
		s.slots[pos] = receiveLogSlot{}
	}
}

func (s *receiveLog) getReceived(seq uint16) bool {
	pos := seq % s.size
	slot := s.slots[pos]

	return slot.valid && slot.seq == seq
}

func (s *receiveLog) fixLastConsecutive() {
	i := s.lastConsecutive + 1
	for ; i != s.end+1 && s.getReceived(i); i++ { //nolint:revive
		// find all consecutive packets
	}

	s.lastConsecutive = i - 1
}
