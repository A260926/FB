// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package nack

import (
	"math/rand"
	"slices"
	"sync"
	"time"

	"github.com/pion/interceptor"
	"github.com/pion/interceptor/internal/validation"
	"github.com/pion/logging"
	"github.com/pion/rtcp"
)

// GeneratorInterceptorFactory is a interceptor.Factory for a GeneratorInterceptor.
type GeneratorInterceptorFactory struct {
	opts []GeneratorOption
}

// NewInterceptor constructs a new ReceiverInterceptor.
func (g *GeneratorInterceptorFactory) NewInterceptor(_ string) (interceptor.Interceptor, error) {
	generatorInterceptor := &GeneratorInterceptor{
		streamsFilter:     streamSupportNack,
		size:              512,
		skipLastN:         0,
		maxNacksPerPacket: 0,
		interval:          time.Millisecond * 100,
		receiveLogs:       map[uint32]*receiveLog{},
		nackCountLogs:     map[uint32]map[uint16]uint16{},
		close:             make(chan struct{}),
		newTicker: func(d time.Duration) ticker {
			return timeTicker{time.NewTicker(d)}
		},
	}

	for _, opt := range g.opts {
		if err := opt(generatorInterceptor); err != nil {
			return nil, err
		}
	}

	if generatorInterceptor.loggerFactory == nil {
		generatorInterceptor.loggerFactory = logging.NewDefaultLoggerFactory()
	}
	if generatorInterceptor.log == nil {
		generatorInterceptor.log = generatorInterceptor.loggerFactory.NewLogger("nack_generator")
	}

	if err := validation.IsPowerOfTwo(generatorInterceptor.size); err != nil {
		return nil, err
	}

	return generatorInterceptor, nil
}

// GeneratorInterceptor interceptor generates nack feedback messages.
type GeneratorInterceptor struct {
	interceptor.NoOp
	streamsFilter     func(info *interceptor.StreamInfo) bool
	size              uint16
	skipLastN         uint16
	maxNacksPerPacket uint16
	interval          time.Duration
	m                 sync.Mutex
	wg                sync.WaitGroup
	close             chan struct{}
	log               logging.LeveledLogger
	loggerFactory     logging.LoggerFactory
	// nackCountLogs records, per SSRC and missing sequence number, how many
	// NACK batches were actually handed to the RTCP writer successfully
	// (Write returned err == nil). Failed writes never spend the budget.
	nackCountLogs map[uint32]map[uint16]uint16

	receiveLogs   map[uint32]*receiveLog
	receiveLogsMu sync.Mutex

	newTicker func(time.Duration) ticker
	// onTickDone is invoked after a feedback cycle finished, including every
	// writer call it triggered. It is intended for in-package tests that need
	// deterministic ordering; production code leaves it nil.
	onTickDone func()
}

// ticker abstracts the feedback period source. Production uses time.Ticker,
// tests can inject a controllable one while driving the real send flow.
type ticker interface {
	C() <-chan time.Time
	Stop()
}

type timeTicker struct {
	*time.Ticker
}

func (t timeTicker) C() <-chan time.Time {
	return t.Ticker.C
}

// nackBatch is the snapshot of one feedback cycle for a single SSRC. The
// receiveLog pointer identifies the stream binding the batch was prepared
// for; completions are committed against that identity only.
type nackBatch struct {
	ssrc uint32
	log  *receiveLog
	seqs []uint16
	nack *rtcp.TransportLayerNack
}

// NewGeneratorInterceptor returns a new GeneratorInterceptorFactory.
func NewGeneratorInterceptor(opts ...GeneratorOption) (*GeneratorInterceptorFactory, error) {
	return &GeneratorInterceptorFactory{opts}, nil
}

// BindRTCPWriter lets you modify any outgoing RTCP packets. It is called once per PeerConnection.
// The returned method will be called once per packet batch.
func (n *GeneratorInterceptor) BindRTCPWriter(writer interceptor.RTCPWriter) interceptor.RTCPWriter {
	n.m.Lock()
	defer n.m.Unlock()

	if n.isClosed() {
		return writer
	}

	n.wg.Add(1)

	go n.loop(writer)

	return writer
}

// BindRemoteStream lets you modify any incoming RTP packets. It is called once for per RemoteStream.
// The returned method will be called once per rtp packet.
func (n *GeneratorInterceptor) BindRemoteStream(
	info *interceptor.StreamInfo, reader interceptor.RTPReader,
) interceptor.RTPReader {
	if !n.streamsFilter(info) {
		return reader
	}

	// error is already checked in NewGeneratorInterceptor
	receiveLog, _ := newReceiveLog(n.size)
	n.receiveLogsMu.Lock()
	// a fresh receiveLog means a fresh binding identity: any in-flight batch
	// captured by the previous binding can no longer commit here.
	n.receiveLogs[info.SSRC] = receiveLog
	n.nackCountLogs[info.SSRC] = map[uint16]uint16{}
	n.receiveLogsMu.Unlock()

	return interceptor.RTPReaderFunc(func(b []byte, a interceptor.Attributes) (int, interceptor.Attributes, error) {
		i, attr, err := reader.Read(b, a)
		if err != nil {
			return 0, nil, err
		}

		if attr == nil {
			attr = make(interceptor.Attributes)
		}
		header, err := attr.GetRTPHeader(b[:i])
		if err != nil {
			return 0, nil, err
		}
		receiveLog.add(header.SequenceNumber)

		return i, attr, nil
	})
}

// UnbindRemoteStream is called when the Stream is removed. It can be used to clean up any data related to that track.
func (n *GeneratorInterceptor) UnbindRemoteStream(info *interceptor.StreamInfo) {
	n.receiveLogsMu.Lock()
	delete(n.receiveLogs, info.SSRC)
	// the count logs must also be dropped for the specific SSRC.
	delete(n.nackCountLogs, info.SSRC)
	n.receiveLogsMu.Unlock()
}

// Close closes the interceptor. It waits for feedback cycles already inside
// the RTCP writer to return before finishing.
func (n *GeneratorInterceptor) Close() error {
	defer n.wg.Wait()
	n.m.Lock()
	defer n.m.Unlock()

	if !n.isClosed() {
		close(n.close)
	}

	return nil
}

func (n *GeneratorInterceptor) loop(rtcpWriter interceptor.RTCPWriter) {
	defer n.wg.Done()

	senderSSRC := rand.Uint32() // #nosec

	missingPacketSeqNums := make([]uint16, n.size)

	ticker := n.newTicker(n.interval)
	defer ticker.Stop()
	for {
		select {
		case <-ticker.C():
			n.runFeedbackCycle(rtcpWriter, senderSSRC, missingPacketSeqNums)
			if n.onTickDone != nil {
				n.onTickDone()
			}
		case <-n.close:
			return
		}
	}
}

// nolint:gocognit,cyclop
func (n *GeneratorInterceptor) runFeedbackCycle(
	rtcpWriter interceptor.RTCPWriter, senderSSRC uint32, missingPacketSeqNums []uint16,
) {
	batches := n.snapshotBatches(senderSSRC, missingPacketSeqNums)

	for _, batch := range batches {
		// The stream may have been unbound or rebound while an earlier
		// writer call of this cycle was blocked. Such stale batches must be
		// dropped without ever reaching the writer.
		if !n.bindingAlive(batch) {
			continue
		}

		// Write without holding the receive state lock: the writer is allowed
		// to re-enter BindRemoteStream/UnbindRemoteStream or block for a while.
		if _, err := rtcpWriter.Write([]rtcp.Packet{batch.nack}, interceptor.Attributes{}); err != nil {
			// A failed write spends no budget: the same gaps are retried in
			// the next feedback cycle.
			n.log.Warnf("failed sending nack: %+v", err)

			continue
		}

		n.commitBatch(batch)
	}
}

// snapshotBatches collects the NACK packets for every currently bound stream
// under the state lock, so the actual writes happen lock-free afterwards.
func (n *GeneratorInterceptor) snapshotBatches(
	senderSSRC uint32, missingPacketSeqNums []uint16,
) []nackBatch {
	n.receiveLogsMu.Lock()
	defer n.receiveLogsMu.Unlock()

	ssrcs := make([]uint32, 0, len(n.receiveLogs))
	for ssrc := range n.receiveLogs {
		ssrcs = append(ssrcs, ssrc)
	}
	slices.Sort(ssrcs)

	batches := make([]nackBatch, 0, len(ssrcs))
	for _, ssrc := range ssrcs {
		receiveLog := n.receiveLogs[ssrc]
		missing := receiveLog.missingSeqNumbers(n.skipLastN, missingPacketSeqNums)

		counts := n.nackCountLogs[ssrc]
		if counts == nil {
			counts = map[uint16]uint16{}
			n.nackCountLogs[ssrc] = counts
		}

		// counters only retain entries for gaps currently in the window;
		// repaired, evicted and full-circle re-wrapped sequences are recycled.
		for nackSeq := range counts {
			if !slices.Contains(missing, nackSeq) {
				delete(counts, nackSeq)
			}
		}

		if len(missing) == 0 {
			if len(counts) == 0 {
				delete(n.nackCountLogs, ssrc)
			}

			continue
		}

		chosen := make([]uint16, 0, len(missing))
		if n.maxNacksPerPacket > 0 {
			for _, missingSeq := range missing {
				if counts[missingSeq] < n.maxNacksPerPacket {
					chosen = append(chosen, missingSeq)
				}
			}
			if len(chosen) == 0 {
				continue
			}
		} else {
			chosen = append(chosen, missing...)
		}

		batches = append(batches, nackBatch{
			ssrc: ssrc,
			log:  receiveLog,
			seqs: append([]uint16(nil), chosen...),
			nack: &rtcp.TransportLayerNack{
				SenderSSRC: senderSSRC,
				MediaSSRC:  ssrc,
				Nacks:      rtcp.NackPairsFromSequenceNumbers(chosen),
			},
		})
	}

	return batches
}

func (n *GeneratorInterceptor) bindingAlive(batch nackBatch) bool {
	n.receiveLogsMu.Lock()
	defer n.receiveLogsMu.Unlock()

	current, ok := n.receiveLogs[batch.ssrc]

	return ok && current == batch.log
}

// commitBatch accounts a successful writer call against the binding and gap
// identities that the batch was prepared for. Completions arriving after the
// gap was repaired, evicted, or after an unbind/rebind of the same SSRC are
// discarded.
func (n *GeneratorInterceptor) commitBatch(batch nackBatch) {
	n.receiveLogsMu.Lock()
	defer n.receiveLogsMu.Unlock()

	current, ok := n.receiveLogs[batch.ssrc]
	if !ok || current != batch.log {
		return
	}

	counts := n.nackCountLogs[batch.ssrc]
	if counts == nil {
		counts = map[uint16]uint16{}
		n.nackCountLogs[batch.ssrc] = counts
	}

	for _, seq := range batch.seqs {
		if !batch.log.trackedMissing(seq) {
			continue
		}
		// clamp at the configured limit so a finite budget can never reopen
		// through counter wrap-around.
		if n.maxNacksPerPacket > 0 && counts[seq] >= n.maxNacksPerPacket {
			continue
		}
		counts[seq]++
	}

	if len(counts) == 0 {
		delete(n.nackCountLogs, batch.ssrc)
	}
}

func (n *GeneratorInterceptor) isClosed() bool {
	select {
	case <-n.close:
		return true
	default:
		return false
	}
}
