// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package nack

import (
	"errors"
	"fmt"
	"sync"
	"testing"
	"time"

	"github.com/pion/interceptor"
	"github.com/pion/rtcp"
	"github.com/pion/rtp"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// manualTicker is the controllable feedback period source. It replaces the
// time.Ticker but drives the unmodified production loop, so every test still
// exercises the real snapshot/write/commit flow.
type manualTicker struct {
	ch chan time.Time
}

func newManualTicker() *manualTicker {
	return &manualTicker{ch: make(chan time.Time)}
}

func (m *manualTicker) C() <-chan time.Time { return m.ch }
func (m *manualTicker) Stop()               {}

type cycleHarness struct {
	t     *testing.T
	n     *GeneratorInterceptor
	tickr *manualTicker
	done  chan struct{}
}

func newCycleHarness(t *testing.T, opts ...GeneratorOption) *cycleHarness {
	t.Helper()

	f, err := NewGeneratorInterceptor(opts...)
	require.NoError(t, err)
	i, err := f.NewInterceptor("lifecycle")
	require.NoError(t, err)
	n := i.(*GeneratorInterceptor)

	tickr := newManualTicker()
	n.newTicker = func(time.Duration) ticker { return tickr }
	done := make(chan struct{})
	n.onTickDone = func() { done <- struct{}{} }

	t.Cleanup(func() {
		_ = n.Close()
	})

	return &cycleHarness{t: t, n: n, tickr: tickr, done: done}
}

// tick runs exactly one production feedback cycle and returns once the cycle,
// including every RTCP writer call it made, has finished.
func (h *cycleHarness) tick() {
	h.t.Helper()
	h.tickr.ch <- time.Now()
	select {
	case <-h.done:
	case <-time.After(5 * time.Second):
		h.t.Fatal("feedback cycle did not finish")
	}
}

// tickAsync starts a cycle that is expected to block inside the writer.
func (h *cycleHarness) tickAsync() <-chan struct{} {
	h.t.Helper()
	finished := make(chan struct{})
	go func() {
		h.tick()
		close(finished)
	}()

	return finished
}

// rtpFeed pushes real marshaled RTP bytes through BindRemoteStream's returned
// reader, exactly as a remote stream would.
type rtpFeed struct {
	t      *testing.T
	reader interceptor.RTPReader
	next   uint16
}

func bindFeed(t *testing.T, n *GeneratorInterceptor, ssrc uint32, nackEnabled bool) *rtpFeed {
	t.Helper()

	info := &interceptor.StreamInfo{SSRC: ssrc}
	if nackEnabled {
		info.RTCPFeedback = []interceptor.RTCPFeedback{{Type: "nack"}}
	}

	feed := &rtpFeed{t: t}
	source := interceptor.RTPReaderFunc(func(b []byte, a interceptor.Attributes) (int, interceptor.Attributes, error) {
		pkt := rtp.Packet{
			Header:  rtp.Header{Version: 2, SSRC: ssrc, SequenceNumber: feed.next},
			Payload: []byte{1, 2, 3, 4},
		}
		raw, err := pkt.Marshal()
		if err != nil {
			return 0, nil, err
		}

		return copy(b, raw), a, nil
	})
	feed.reader = n.BindRemoteStream(info, source)

	return feed
}

func (f *rtpFeed) send(seqs ...uint16) {
	f.t.Helper()
	for _, s := range seqs {
		f.next = s
		buf := make([]byte, 1500)
		i, _, err := f.reader.Read(buf, nil)
		require.NoError(f.t, err)

		header := &rtp.Header{}
		_, err = header.Unmarshal(buf[:i])
		require.NoError(f.t, err)
		require.Equal(f.t, s, header.SequenceNumber)
	}
}

// recordedNack is an RTCP NACK parsed back from the actual wire bytes handed
// to the bound RTCPWriter.
type recordedNack struct {
	mediaSSRC uint32
	seqs      []uint16
}

type recordingWriter struct {
	mu            sync.Mutex
	recorded      []recordedNack
	successCalls  int
	failureCalls  int
	failRemaining int
	gate          chan struct{} // when non-nil, every Write blocks until release
	entered       chan struct{}
}

func newRecordingWriter() *recordingWriter {
	return &recordingWriter{entered: make(chan struct{}, 16)}
}

func (w *recordingWriter) Write(pkts []rtcp.Packet, _ interceptor.Attributes) (int, error) {
	if w.gate != nil {
		select {
		case w.entered <- struct{}{}:
		default:
		}
		<-w.gate
	}

	// marshal and parse the real wire bytes instead of trusting the structs
	raw, err := rtcp.Marshal(pkts)
	if err != nil {
		return 0, err
	}
	parsed, err := rtcp.Unmarshal(raw)
	if err != nil {
		return 0, err
	}

	w.mu.Lock()
	fail := w.failRemaining > 0
	if fail {
		w.failRemaining--
		w.failureCalls++
	} else {
		w.successCalls++
	}
	if !fail {
		for _, p := range parsed {
			nack, ok := p.(*rtcp.TransportLayerNack)
			if !ok {
				continue
			}
			rec := recordedNack{mediaSSRC: nack.MediaSSRC}
			for _, pair := range nack.Nacks {
				pair.Range(func(s uint16) bool {
					rec.seqs = append(rec.seqs, s)

					return true
				})
			}
			w.recorded = append(w.recorded, rec)
		}
	}
	w.mu.Unlock()

	if fail {
		return 0, errors.New("scripted writer failure")
	}

	return len(raw), nil
}

func (w *recordingWriter) nacksFor(ssrc uint32) []recordedNack {
	w.mu.Lock()
	defer w.mu.Unlock()

	out := []recordedNack{}
	for _, rec := range w.recorded {
		if rec.mediaSSRC == ssrc {
			out = append(out, recordedNack{mediaSSRC: rec.mediaSSRC, seqs: append([]uint16{}, rec.seqs...)})
		}
	}

	return out
}

func (w *recordingWriter) allNacks() []recordedNack {
	w.mu.Lock()
	defer w.mu.Unlock()

	return append([]recordedNack{}, w.recorded...)
}

func (w *recordingWriter) successes() int {
	w.mu.Lock()
	defer w.mu.Unlock()

	return w.successCalls
}

func (w *recordingWriter) failures() int {
	w.mu.Lock()
	defer w.mu.Unlock()

	return w.failureCalls
}

func (w *recordingWriter) waitEntered() {
	select {
	case <-w.entered:
	case <-time.After(5 * time.Second):
		panic("writer never entered")
	}
}

// armGate makes every subsequent Write block until the gate is released, and
// discards entry signals left over from a previous gated cycle.
func (w *recordingWriter) armGate() {
	for {
		select {
		case <-w.entered:
		default:
			w.gate = make(chan struct{})

			return
		}
	}
}

func (h *cycleHarness) currentLog(ssrc uint32) *receiveLog {
	h.n.receiveLogsMu.Lock()
	defer h.n.receiveLogsMu.Unlock()

	return h.n.receiveLogs[ssrc]
}

func (h *cycleHarness) count(ssrc uint32, seq uint16) (uint16, bool) {
	h.n.receiveLogsMu.Lock()
	defer h.n.receiveLogsMu.Unlock()

	counts, ok := h.n.nackCountLogs[ssrc]
	if !ok {
		return 0, false
	}
	v, ok := counts[seq]

	return v, ok
}

func (h *cycleHarness) missing(ssrc uint32) []uint16 {
	rl := h.currentLog(ssrc)
	if rl == nil {
		return nil
	}

	return append([]uint16{}, rl.missingSeqNumbers(h.n.skipLastN, make([]uint16, h.n.size))...)
}

func containsSeq(nacks []recordedNack, want uint16) bool {
	for _, rec := range nacks {
		for _, s := range rec.seqs {
			if s == want {
				return true
			}
		}
	}

	return false
}

func waitFinished(t *testing.T, ch <-chan struct{}) {
	t.Helper()
	select {
	case <-ch:
	case <-time.After(5 * time.Second):
		t.Fatal("feedback cycle hung")
	}
}

const uint16HalfCircle = 1 << 15

// TestNackRetryCommit verifies that only successful writer calls spend the
// per-gap budget, that failed writes are retried, that repaired gaps stop being
// NACKed, and that multiple SSRCs plus non-NACK streams stay independent.
func TestNackRetryCommit(t *testing.T) {
	h := newCycleHarness(t, GeneratorSize(64), GeneratorMaxNacksPerPacket(1))
	writer := newRecordingWriter()
	h.n.BindRTCPWriter(writer)

	feed1 := bindFeed(t, h.n, 1, true)
	feed1.send(100, 102)
	require.Equal(t, []uint16{101}, h.missing(1))

	// first attempt fails: no budget is spent and the gap is retried later.
	writer.failRemaining = 1
	h.tick()
	assert.Equal(t, 1, writer.failures())
	assert.Equal(t, 0, writer.successes(), "failed write must not record a success")
	_, hasCount := h.count(1, 101)
	require.False(t, hasCount, "failed write must not spend the budget")
	require.Equal(t, []uint16{101}, h.missing(1))

	// second attempt succeeds: the NACK is committed once.
	h.tick()
	nacks := writer.nacksFor(1)
	require.Len(t, nacks, 1)
	assert.Equal(t, []uint16{101}, nacks[0].seqs)
	sent, ok := h.count(1, 101)
	require.True(t, ok)
	assert.Equal(t, uint16(1), sent)

	// budget exhausted: further cycles do not write for this gap.
	h.tick()
	assert.Len(t, writer.nacksFor(1), 1)

	// normal gap repair: filling the hole removes it, and a new gap is NACKed.
	feed1.send(101)
	feed1.send(104)
	require.Equal(t, []uint16{103}, h.missing(1))
	h.tick()
	nacks = writer.nacksFor(1)
	require.Len(t, nacks, 2)
	assert.Equal(t, []uint16{103}, nacks[1].seqs)
	feed1.send(103)
	assert.Empty(t, h.missing(1))

	// a second SSRC is tracked independently.
	feed2 := bindFeed(t, h.n, 2, true)
	feed2.send(200, 202)
	h.tick()
	nacks2 := writer.nacksFor(2)
	require.Len(t, nacks2, 1)
	assert.Equal(t, uint32(2), nacks2[0].mediaSSRC)
	assert.Equal(t, []uint16{201}, nacks2[0].seqs)

	// a stream without NACK support is passed through and never generates RTCP.
	passthrough := bindFeed(t, h.n, 9, false)
	passthrough.send(300, 302)
	h.tick()
	for _, rec := range writer.allNacks() {
		assert.NotEqual(t, uint32(9), rec.mediaSSRC)
	}

	// resetting one stream must not touch the other stream's state or budget.
	h.n.UnbindRemoteStream(&interceptor.StreamInfo{SSRC: 1})
	recordsAtUnbind := len(writer.allNacks())
	feed2.send(204) // gap 203
	h.tick()
	newRecords := writer.allNacks()[recordsAtUnbind:]
	for _, rec := range newRecords {
		assert.NotEqual(t, uint32(1), rec.mediaSSRC, "unbound stream must not be re-sent")
	}
	require.NotNil(t, h.currentLog(2), "other SSRC must remain bound")
	sent2, ok := h.count(2, 201)
	require.True(t, ok)
	assert.Equal(t, uint16(1), sent2, "the other SSRC's budget must be preserved")

	// a fresh binding of the same SSRC starts with a fresh budget.
	feed1Reborn := bindFeed(t, h.n, 1, true)
	feed1Reborn.send(400, 402)
	h.tick()
	nacks = writer.nacksFor(1)
	require.NotEmpty(t, nacks)
	assert.Equal(t, []uint16{401}, nacks[len(nacks)-1].seqs)
	sent, ok = h.count(1, 401)
	require.True(t, ok)
	assert.Equal(t, uint16(1), sent)
}

// TestNackWindowIdentity verifies that out-of-window packets and full-circle
// re-wraps cannot alias another sequence number's slot, that the ambiguous
// half-circle value is ignored, and that gaps survive the 65535 -> 0 wrap.
func TestNackWindowIdentity(t *testing.T) {
	h := newCycleHarness(t, GeneratorSize(64), GeneratorMaxNacksPerPacket(0))
	writer := newRecordingWriter()
	h.n.BindRTCPWriter(writer)

	feed := bindFeed(t, h.n, 7, true)
	rl := h.currentLog(7)

	// ordinary gap and repair through real RTP bytes, with a real NACK first.
	feed.send(100, 102)
	assert.Equal(t, []uint16{101}, h.missing(7))
	h.tick()
	sent, ok := h.count(7, 101)
	require.True(t, ok)
	assert.Equal(t, uint16(1), sent)
	feed.send(101)
	assert.Empty(t, h.missing(7))

	// advance so 101 leaves the 64-slot window: window is [103, 166].
	feed.send(166)
	missing := h.missing(7)
	require.Len(t, missing, 63)
	assert.Equal(t, uint16(103), missing[0])
	assert.Equal(t, uint16(165), missing[62])

	// a stale 101 must not alias the slot currently owned by missing 165.
	feed.send(101)
	missing = h.missing(7)
	require.Len(t, missing, 63, "stale packet must not rewrite another slot")
	assert.Equal(t, uint16(165), missing[62])
	assert.False(t, rl.get(165))
	assert.False(t, rl.get(101), "out-of-window packet must not be recorded")

	// repairing the real 165 fills exactly that hole.
	feed.send(165)
	missing = h.missing(7)
	require.Len(t, missing, 62)
	assert.Equal(t, uint16(164), missing[61])

	// a packet exactly half a circle away is ambiguous and ignored.
	rl.m.RLock()
	endBefore := rl.end
	rl.m.RUnlock()
	feed.send(endBefore + uint16HalfCircle)
	rl.m.RLock()
	endAfter := rl.end
	rl.m.RUnlock()
	assert.Equal(t, endBefore, endAfter, "half-circle value must not advance the window")

	// advance in window-safe steps up to the wrap boundary.
	for seq := uint16(167); seq != 65534; seq++ {
		feed.send(seq)
	}
	feed.send(65535) // 65534 is missing
	feed.send(0, 1)  // normal 65535 -> 0 wrap
	missing = h.missing(7)
	assert.Contains(t, missing, uint16(65534), "gap must survive wrap to 0")
	h.tick()
	assert.True(t, containsSeq(writer.nacksFor(7), 65534), "wrapped gap 65534 must be NACKed")
	feed.send(65534)
	assert.NotContains(t, h.missing(7), uint16(65534))

	// full-circle reuse: walk forward in safe steps until the same 16-bit
	// sequence number becomes a brand-new gap with no inherited identity.
	for seq := uint16(2); seq != 101; seq++ {
		feed.send(seq)
	}
	feed.send(102) // window completed a full circle; 101 is missing again
	_, inherited := h.count(7, 101)
	assert.False(t, inherited, "full-circle gap must not inherit the old counter")
	assert.Equal(t, []uint16{101}, h.missing(7))
	h.tick()
	assert.True(t, containsSeq(writer.nacksFor(7), 101))
	sent, ok = h.count(7, 101)
	require.True(t, ok)
	assert.Equal(t, uint16(1), sent, "re-wrapped gap gets a fresh budget")
}

// TestNackRebindCompletion verifies that writer completions only affect the
// binding and gap identity captured when the batch was prepared.
func TestNackRebindCompletion(t *testing.T) {
	h := newCycleHarness(t, GeneratorSize(64), GeneratorMaxNacksPerPacket(2))
	writer := newRecordingWriter()
	h.n.BindRTCPWriter(writer)

	const ssrc = uint32(11)
	feed := bindFeed(t, h.n, ssrc, true)
	feed.send(100, 102) // gap 101

	// scenario 1: the gap is repaired while the writer is blocked.
	writer.armGate()
	cycle := h.tickAsync()
	writer.waitEntered()
	feed.send(101) // repair while Write is in flight
	close(writer.gate)
	waitFinished(t, cycle)
	_, committed := h.count(ssrc, 101)
	assert.False(t, committed, "completion for a repaired gap must be discarded")
	assert.Empty(t, h.missing(ssrc))

	// scenario 2: unbind and rebind the same SSRC while the writer is blocked.
	feed.send(104) // gap 103
	writer.armGate()
	cycle = h.tickAsync()
	writer.waitEntered()

	h.n.UnbindRemoteStream(&interceptor.StreamInfo{SSRC: ssrc})
	newFeed := bindFeed(t, h.n, ssrc, true)
	newFeed.send(200, 202) // new binding, gap 201
	close(writer.gate)
	waitFinished(t, cycle)

	// the stale completion must not create state in the new binding.
	assert.Equal(t, []uint16{201}, h.missing(ssrc))
	_, stale := h.count(ssrc, 103)
	assert.False(t, stale, "old binding's completion must not pollute the new one")

	// the old binding's returned reader must not mutate the new binding either.
	rl := h.currentLog(ssrc)
	rl.m.RLock()
	endBefore := rl.end
	rl.m.RUnlock()
	feed.send(999)
	rl = h.currentLog(ssrc)
	rl.m.RLock()
	endAfter := rl.end
	rl.m.RUnlock()
	assert.Equal(t, endBefore, endAfter, "old reader cannot touch the new binding")

	h.tick()
	rebound := len(writer.nacksFor(ssrc))
	assert.True(t, containsSeq(writer.nacksFor(ssrc)[rebound-1:], 201))
	assert.False(t, containsSeq(writer.nacksFor(ssrc)[rebound-1:], 103),
		"the new binding only reports its own gaps")

	// scenario 3: while one writer call is blocked, another stream queued
	// behind it gets unbound; its stale batch must be dropped without a Write.
	other := bindFeed(t, h.n, 12, true)
	other.send(300, 302) // gap 301
	newFeed.send(204)    // gap 203 on ssrc 11
	writer.armGate()
	callsBefore := writer.successes() + writer.failures()
	cycle = h.tickAsync()
	writer.waitEntered() // ssrc 11's batch enters first (sorted SSRC order)
	h.n.UnbindRemoteStream(&interceptor.StreamInfo{SSRC: 12})
	close(writer.gate)
	waitFinished(t, cycle)
	assert.Equal(t, callsBefore+1, writer.successes()+writer.failures(),
		"queued batch of the unbound stream must be dropped")
	_, ok := h.n.receiveLogs[12]
	assert.False(t, ok)

	// Close waits for an in-flight writer call to return before finishing.
	closer := newCycleHarness(t, GeneratorSize(64))
	closeWriter := newRecordingWriter()
	closer.n.BindRTCPWriter(closeWriter)
	closeFeed := bindFeed(t, closer.n, 21, true)
	closeFeed.send(0, 2)
	closeWriter.armGate()
	closeCycle := closer.tickAsync()
	closeWriter.waitEntered()
	closed := make(chan struct{})
	go func() {
		_ = closer.n.Close()
		close(closed)
	}()
	close(closeWriter.gate)
	waitFinished(t, closeCycle)
	select {
	case <-closed:
	case <-time.After(5 * time.Second):
		t.Fatal("Close did not wait for the in-flight writer")
	}
}

// TestNackRetrySaturation drives more than 65536 feedback cycles while the
// sequence space wraps, verifying that finite budgets are never reopened by
// counter wrap-around and that retained state stays bounded to the window.
func TestNackRetrySaturation(t *testing.T) {
	h := newCycleHarness(t, GeneratorSize(64), GeneratorMaxNacksPerPacket(3))
	writer := newRecordingWriter()
	h.n.BindRTCPWriter(writer)

	feed := bindFeed(t, h.n, 31, true)
	feed.send(100, 102) // gap 101
	for range 5 {
		h.tick()
	}
	assert.Equal(t, 3, countSeq(writer.nacksFor(31), 101),
		"finite limit must hold across repeated cycles")
	feed.send(101)

	// long wrap-around run with a limit of one successful NACK per gap.
	h.n.maxNacksPerPacket = 1
	control := bindFeed(t, h.n, 41, false) // non-NACK control stream

	const cycles = 65540 // strictly more than one full 16-bit circle
	// stay contiguous with the already received packets so every cycle has
	// exactly one outstanding gap.
	feed.send(103) // repair the saturated gap
	feed.send(105) // opens gap 104 next to the received 103
	gap := uint16(104)
	firstSeenAt := map[uint16]int{}
	for cycle := range cycles {
		h.tick()

		// repair the current gap, then skip the next packet to open a new one:
		// with end == gap+1, sending gap repairs and gap+3 makes gap+2 missing.
		feed.send(gap)
		feed.send(gap + 3)
		gap += 2

		if cycle%1024 == 0 {
			control.send(uint16(cycle)) // pass-through traffic keeps flowing
		}
	}

	assert.Equal(t, 3+cycles, writer.successes(), "one successful batch per cycle")
	steady := writer.nacksFor(31)[3:] // drop the three saturated-run NACKs
	require.Len(t, steady, cycles)
	for cycleIdx, rec := range steady {
		require.Len(t, rec.seqs, 1, "each cycle carries exactly one fresh gap")
		seq := rec.seqs[0]
		// gaps advance by two, so the even sequence subspace wraps after
		// 32768 cycles, i.e. 65536 traversed sequence numbers: a full circle.
		if first, seen := firstSeenAt[seq]; seen {
			assert.GreaterOrEqual(t, cycleIdx-first, 32768,
				"same 16-bit seq may only reappear after a full circle")
		} else {
			firstSeenAt[seq] = cycleIdx
		}
	}
	for _, rec := range writer.allNacks() {
		assert.NotEqual(t, uint32(41), rec.mediaSSRC, "non-NACK stream never produces feedback")
	}

	// counters stay at or below the limit and retained state stays window-sized.
	h.n.receiveLogsMu.Lock()
	for ssrc, counts := range h.n.nackCountLogs {
		assert.Less(t, len(counts), int(h.n.size), fmt.Sprintf("ssrc %d retains only window-live counters", ssrc))
		for seq, v := range counts {
			assert.LessOrEqual(t, v, uint16(1), fmt.Sprintf("counter %d/%d overflowed the limit", ssrc, seq))
		}
	}
	h.n.receiveLogsMu.Unlock()
}

func countSeq(nacks []recordedNack, want uint16) int {
	total := 0
	for _, rec := range nacks {
		for _, s := range rec.seqs {
			if s == want {
				total++
			}
		}
	}

	return total
}
