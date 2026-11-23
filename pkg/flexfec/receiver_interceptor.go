// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package flexfec

import (
	"errors"
	"io"
	"sync"

	"github.com/pion/interceptor"
	"github.com/pion/rtp"
)

// ErrRepairPending is returned when a Read did not produce media output: the
// datagram (if any) was a repair packet that only fed the solver, or the
// recovered packets are still waiting for the media window to cover them.
var ErrRepairPending = errors.New("flexfec repair pending: no media packet available")

// Capacity limits shared by the repair window and the equation budget.
const (
	minRepairCapacity = 2
	maxRepairCapacity = 512
)

// RepairReceiverInterceptor is the receive-side FlexFEC-03 interceptor.
//
// It consumes interleaved media and repair datagrams of one bound stream,
// exposes only media RTP (original and recovered) to the downstream reader and
// feeds repaired packets to whatever follows, typically a jitter buffer.
type RepairReceiverInterceptor struct {
	interceptor.NoOp

	window       uint16
	maxEquations uint16

	mu      sync.Mutex
	streams map[uint32]*repairBinding
	closed  bool
}

// repairBinding is one generation of a bound remote stream.
type repairBinding struct {
	generation uint64
	mediaSSRC  uint32
	fecSSRC    uint32
	mediaPT    uint8
	fecPT      uint8
	decoder    *repairDecoder
	closed     bool
}

// closedReader is handed out for conflicting bindings and for readers after
// Unbind. It never touches the upstream reader.
type closedReader struct {
	err error
}

func (r *closedReader) Read([]byte, interceptor.Attributes) (int, interceptor.Attributes, error) {
	return 0, nil, r.err
}

// repairReader is the RTPReader returned by BindRemoteStream. One instance is
// shared by both SSRC identities of the stream, which may be interleaved on
// the same upstream reader.
type repairReader struct {
	parent *RepairReceiverInterceptor
	bind   *repairBinding
	next   interceptor.RTPReader
	readMu sync.Mutex
}

// NewRepairReceiver creates a receive-side FlexFEC interceptor. window limits
// how many extended media sequence numbers are retained, maxEquations bounds
// the independent repair equations kept per stream. Both must be in [2, 512].
func NewRepairReceiver(window uint16, maxEquations uint16) (*RepairReceiverInterceptor, error) {
	if window < minRepairCapacity || window > maxRepairCapacity {
		return nil, errors.New("flexfec repair window must be between 2 and 512")
	}

	if maxEquations < minRepairCapacity || maxEquations > maxRepairCapacity {
		return nil, errors.New("flexfec max equations must be between 2 and 512")
	}

	return &RepairReceiverInterceptor{
		window:       window,
		maxEquations: maxEquations,
		streams:      make(map[uint32]*repairBinding),
	}, nil
}

// BindRemoteStream binds one media/repair identity pair. Streams without a
// complete FEC identity pass through untouched. SSRC/PT conflicts across
// streams return a reader that only errors, reads nothing from upstream and
// leaves existing bindings unchanged.
func (r *RepairReceiverInterceptor) BindRemoteStream(info *interceptor.StreamInfo, reader interceptor.RTPReader) interceptor.RTPReader {
	r.mu.Lock()
	defer r.mu.Unlock()

	if r.closed {
		return &closedReader{err: io.ErrClosedPipe}
	}

	if info.SSRCForwardErrorCorrection == 0 || info.PayloadTypeForwardErrorCorrection == 0 {
		// No complete FEC identity: forward every datagram unchanged.
		return reader
	}

	if info.SSRC == 0 || info.SSRC == info.SSRCForwardErrorCorrection {
		return &closedReader{err: errors.New("flexfec stream requires distinct media and repair SSRCs")}
	}

	// Re-binding the same media SSRC starts a fresh generation: old readers
	// close and none of the old equations, window or queued packets carry over.
	previous, hadPrevious := r.streams[info.SSRC]
	generation := uint64(1)

	if hadPrevious {
		generation = previous.generation + 1
	}

	for key, existing := range r.streams {
		if key == info.SSRC {
			continue
		}

		if existing.mediaSSRC == info.SSRCForwardErrorCorrection ||
			existing.fecSSRC == info.SSRC ||
			existing.fecSSRC == info.SSRCForwardErrorCorrection {
			// Do not mutate any existing binding on a conflicting request.
			return &closedReader{err: errors.New("flexfec stream SSRCs conflict with an existing binding")}
		}
	}

	if hadPrevious {
		previous.closed = true
		delete(r.streams, info.SSRC)
	}

	binding := &repairBinding{
		generation: generation,
		mediaSSRC:  info.SSRC,
		fecSSRC:    info.SSRCForwardErrorCorrection,
		mediaPT:    info.PayloadType,
		fecPT:      info.PayloadTypeForwardErrorCorrection,
		decoder:    newRepairDecoder(info.SSRC, int(r.window), int(r.maxEquations)),
	}
	r.streams[info.SSRC] = binding

	return &repairReader{
		parent: r,
		bind:   binding,
		next:   reader,
	}
}

// UnbindRemoteStream drops all state of one stream generation. Readers of that
// generation subsequently fail with io.ErrClosedPipe and stop reading upstream.
func (r *RepairReceiverInterceptor) UnbindRemoteStream(info *interceptor.StreamInfo) {
	r.mu.Lock()
	defer r.mu.Unlock()

	if binding, ok := r.streams[info.SSRC]; ok {
		binding.closed = true
		delete(r.streams, info.SSRC)
	}
}

// Close removes every binding and all repair state.
func (r *RepairReceiverInterceptor) Close() error {
	r.mu.Lock()
	defer r.mu.Unlock()

	for _, binding := range r.streams {
		binding.closed = true
	}

	r.closed = true
	r.streams = make(map[uint32]*repairBinding)

	return nil
}

// Read first delivers media packets already recovered or queued; otherwise it
// reads the upstream at most once and integrates that single datagram.
func (rr *repairReader) Read(buf []byte, attributes interceptor.Attributes) (int, interceptor.Attributes, error) {
	rr.readMu.Lock()
	defer rr.readMu.Unlock()

	rr.parent.mu.Lock()
	closed := rr.bind.closed
	rr.parent.mu.Unlock()

	if closed {
		return 0, nil, io.ErrClosedPipe
	}

	if rr.bind.decoder.hasQueued() {
		if rr.bind.decoder.queuedSize() > len(buf) {
			// Do not consume an upstream datagram while ready output cannot
			// fit into the caller's buffer.
			return 0, nil, io.ErrShortBuffer
		}

		if out := rr.bind.decoder.popQueued(buf); out != nil {
			return len(out.data), out.attrs, nil
		}
	}

	n, inAttrs, readErr := rr.next.Read(buf, attributes)
	if readErr != nil && n == 0 {
		// Ready data has priority over upstream errors and was drained above.
		return 0, nil, readErr
	}

	if n == 0 {
		return 0, nil, ErrRepairPending
	}

	// Only parse the datagram's own bytes; the caller may reuse buf afterwards.
	raw := append([]byte(nil), buf[:n]...)

	var header rtp.Header
	if _, err := header.Unmarshal(raw); err != nil {
		return 0, nil, err
	}

	var packet rtp.Packet
	if err := packet.Unmarshal(raw); err != nil {
		return 0, nil, err
	}

	attrs := snapshotAttributes(inAttrs)

	switch header.SSRC {
	case rr.bind.mediaSSRC:
		if header.PayloadType != rr.bind.mediaPT {
			return 0, nil, errors.New("flexfec media payload type does not match the binding")
		}

		rr.bind.decoder.ingestMedia(&packet, raw, attrs)

		if rr.bind.decoder.hasQueued() {
			if rr.bind.decoder.queuedSize() > len(buf) {
				return 0, nil, io.ErrShortBuffer
			}

			if out := rr.bind.decoder.popQueued(buf); out != nil {
				return len(out.data), out.attrs, nil
			}
		}

		// Old or duplicate media packet: nothing to deliver for this Read.
		if readErr != nil {
			return 0, nil, readErr
		}

		return 0, nil, ErrRepairPending
	case rr.bind.fecSSRC:
		if header.PayloadType != rr.bind.fecPT {
			return 0, nil, errors.New("flexfec repair payload type does not match the binding")
		}

		if err := rr.bind.decoder.ingestFEC(&packet, attrs); err != nil {
			return 0, nil, err
		}

		if rr.bind.decoder.hasQueued() {
			if rr.bind.decoder.queuedSize() > len(buf) {
				return 0, nil, io.ErrShortBuffer
			}

			if out := rr.bind.decoder.popQueued(buf); out != nil {
				return len(out.data), out.attrs, nil
			}
		}

		if readErr != nil {
			return 0, nil, readErr
		}

		return 0, nil, ErrRepairPending
	default:
		// Datagram of an unrelated SSRC on the shared entry: pass it through
		// without touching repair state.
		n = copy(buf, raw)

		return n, attrs, nil
	}
}

// popQueued removes the oldest queued output packet, copying its independent
// byte snapshot into dst. Returns nil when nothing is ready or when dst is too
// small; in that case the packet stays queued.
func (d *repairDecoder) popQueued(dst []byte) *outPacket {
	if len(d.queue) == 0 {
		return nil
	}

	out := d.queue[0]

	if len(out.data) > len(dst) {
		return nil
	}

	d.queue = d.queue[1:]
	copy(dst, out.data)

	return out
}

func (d *repairDecoder) hasQueued() bool {
	return len(d.queue) > 0
}

func (d *repairDecoder) queuedSize() int {
	return len(d.queue[0].data)
}

// snapshotAttributes returns an independent copy of the input attributes so
// later reuse by the caller cannot mutate the state stored for recovery. A nil
// input stays nil: original packets keep exactly their own attributes.
func snapshotAttributes(in interceptor.Attributes) interceptor.Attributes {
	if in == nil {
		return nil
	}

	out := interceptor.Attributes{}

	for key, value := range in {
		out[key] = value
	}

	return out
}
