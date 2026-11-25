// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package jitterbuffer

import (
	"errors"
	"io"
	"sync"
	"testing"
	"time"

	"github.com/pion/interceptor"
	"github.com/pion/interceptor/task_support/taskcheck"
	"github.com/pion/rtcp"
	"github.com/pion/rtp"
)

type redClock struct {
	mu sync.Mutex
	t  time.Time
}

func newRedClock() *redClock {
	return &redClock{t: time.Unix(1000, 0)}
}

func (c *redClock) Now() time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()

	return c.t
}

func (c *redClock) Advance(d time.Duration) time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.t = c.t.Add(d)

	return c.t
}

func redTestConfig(clock *redClock) RedundantConfig {
	return RedundantConfig{
		OutputSSRC:  300,
		PayloadType: 96,
		Paths: [2]RedundantPath{
			{SSRC: 10, SequenceOffset: 7, RTT: 10 * time.Millisecond},
			{SSRC: 20, SequenceOffset: 99, RTT: 30 * time.Millisecond},
		},
		Window: 16,
		Hold:   20 * time.Millisecond,
		Retry:  50 * time.Millisecond,
		Now:    clock.Now,
	}
}

func redPacket(cfg RedundantConfig, path int, canonical uint16, payload uint16) *rtp.Packet {
	return &rtp.Packet{
		Header: rtp.Header{
			Version:        2,
			Padding:        false,
			Marker:         true,
			PayloadType:    cfg.PayloadType,
			SequenceNumber: canonical + cfg.Paths[path].SequenceOffset,
			Timestamp:      9000 + uint32(canonical),
			SSRC:           cfg.Paths[path].SSRC,
		},
		Payload: []byte{byte(payload)},
	}
}

func redRequirePacket(
	c *taskcheck.C, packet *rtp.Packet, err error, canonical uint16, payload uint16,
) bool {
	if err != nil {
		c.Check(false, "Pop returned unexpected error: %v", err)
		return false
	}
	if packet == nil {
		c.Check(false, "Pop returned nil packet without error")
		return false
	}

	c.Check(packet.SequenceNumber == canonical, "canonical sequence: want %d got %d", canonical, packet.SequenceNumber)
	c.Check(packet.SSRC == 300, "output SSRC: want 300 got %d", packet.SSRC)
	c.Check(packet.PayloadType == 96, "output PT: want 96 got %d", packet.PayloadType)
	c.Check(len(packet.Payload) == 1 && packet.Payload[0] == byte(payload),
		"payload for %d: want [%d] got %v", canonical, byte(payload), packet.Payload)

	return true
}

func TestRedundantMerge(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		clock := newRedClock()
		cfg := redTestConfig(clock)

		invalidMutations := []struct {
			name string
			edit func(*RedundantConfig)
		}{
			{"zero path zero SSRC", func(cfg *RedundantConfig) { cfg.Paths[0].SSRC = 0 }},
			{"equal path SSRCs", func(cfg *RedundantConfig) { cfg.Paths[1].SSRC = cfg.Paths[0].SSRC }},
			{"zero output SSRC", func(cfg *RedundantConfig) { cfg.OutputSSRC = 0 }},
			{"output collides path", func(cfg *RedundantConfig) { cfg.OutputSSRC = cfg.Paths[0].SSRC }},
			{"payload type too large", func(cfg *RedundantConfig) { cfg.PayloadType = 128 }},
			{"window too small", func(cfg *RedundantConfig) { cfg.Window = 3 }},
			{"window too large", func(cfg *RedundantConfig) { cfg.Window = 4097 }},
			{"hold not positive", func(cfg *RedundantConfig) { cfg.Hold = 0 }},
			{"retry not positive", func(cfg *RedundantConfig) { cfg.Retry = -time.Millisecond }},
			{"negative RTT", func(cfg *RedundantConfig) { cfg.Paths[1].RTT = -1 }},
			{"nil Now", func(cfg *RedundantConfig) { cfg.Now = nil }},
		}
		for _, tc := range invalidMutations {
			bad := cfg
			tc.edit(&bad)
			badBuffer, err := NewRedundant(bad)
			c.Check(badBuffer == nil, "%s: expected nil buffer", tc.name)
			c.Check(errors.Is(err, ErrInvalidRedundantConfig), "%s: expected invalid config, got %v", tc.name, err)
		}

		badOptionCfg := cfg
		badOptionCfg.Window = 1
		optionFactory, packageOptionErr := NewInterceptor(WithRedundant(badOptionCfg))
		c.Check(packageOptionErr == nil, "option construction should defer validation: %v", packageOptionErr)
		if optionFactory != nil {
			badInterceptor, factoryErr := optionFactory.NewInterceptor("bad-config")
			c.Check(badInterceptor == nil, "invalid option created an interceptor")
			c.Check(errors.Is(factoryErr, ErrInvalidRedundantConfig), "factory option error: %v", factoryErr)
		}

		b, err := NewRedundant(cfg)
		c.Check(err == nil && b != nil, "valid NewRedundant failed: %v", err)
		if err != nil {
			return
		}

		// Complementary loss plus a conflicting duplicate. The first copy wins.
		first10 := redPacket(cfg, 0, 10, 10)
		b.Push(first10)
		b.Push(redPacket(cfg, 1, 10, 0x99))
		b.Push(redPacket(cfg, 0, 11, 11))
		b.Push(redPacket(cfg, 1, 11, 11))
		b.Push(redPacket(cfg, 1, 12, 12))
		b.Push(redPacket(cfg, 0, 13, 13))
		b.Push(redPacket(cfg, 0, 14, 14))
		b.Push(redPacket(cfg, 1, 14, 14))

		// Mutate the caller's backing payload after Push; the deep copy survives.
		first10.Payload[0] = 0xee
		first10.Marker = false

		b.redundant.mu.Lock()
		canonicalCapacity := len(b.redundant.slots)
		mirroredCapacity := int(b.packets.Length())
		b.redundant.mu.Unlock()
		c.Check(canonicalCapacity == 5, "capacity must count canonical positions only, got %d", canonicalCapacity)
		c.Check(mirroredCapacity == 5, "legacy queue length=%d", mirroredCapacity)

		for seq := uint16(10); seq <= 14; seq++ {
			packet, popErr := b.Pop()
			if redRequirePacket(c, packet, popErr, seq, seq) { //nolint:gosec
				c.Check(packet.Marker, "first-copy marker must be retained at seq %d", seq)
			}
		}

		// Slow/fast arrival order does not change canonical order.
		fast, fastErr := NewRedundant(cfg)
		c.Check(fastErr == nil, "second constructor failed: %v", fastErr)
		if fastErr == nil {
			fast.Push(redPacket(cfg, 1, 100, 100))
			fast.Push(redPacket(cfg, 1, 101, 101))
			fast.Push(redPacket(cfg, 0, 100, 0xee)) // late duplicate
			packet, popErr := fast.Pop()
			redRequirePacket(c, packet, popErr, 100, 100)
			packet, popErr = fast.Pop()
			redRequirePacket(c, packet, popErr, 101, 101)
		}

		// 65535 -> 0 wrap with an offset that itself wraps the path seq.
		wrapCfg := cfg
		wrapCfg.Paths[0].SequenceOffset = 3
		wrap, wrapErr := NewRedundant(wrapCfg)
		c.Check(wrapErr == nil, "wrap constructor failed: %v", wrapErr)
		if wrapErr == nil {
			wrap.Push(redPacket(wrapCfg, 0, 65535, 0xff)) // path seq 2
			wrap.Push(redPacket(wrapCfg, 0, 0, 0x00))     // path seq 3
			c.Check(wrap.PlayoutHead() == 65535, "wrap playout head: %d", wrap.PlayoutHead())
			packet, popErr := wrap.Pop()
			redRequirePacket(c, packet, popErr, 65535, 0xff)
			packet, popErr = wrap.Pop()
			redRequirePacket(c, packet, popErr, 0, 0x00)
		}

		// Window, unknown SSRC and bad PT rejection.
		windowCfg := cfg
		windowCfg.Window = 4
		wb, windowErr := NewRedundant(windowCfg)
		c.Check(windowErr == nil, "window constructor failed: %v", windowErr)
		if windowErr == nil {
			wb.Push(redPacket(windowCfg, 0, 100, 100))
			for seq := uint16(101); seq <= 103; seq++ {
				wb.Push(redPacket(windowCfg, 0, seq, seq)) //nolint:gosec
			}
			wb.Push(redPacket(windowCfg, 0, 104, 104)) // exactly one beyond Window positions

			unknown := redPacket(windowCfg, 0, 101, 1)
			unknown.SSRC = 999
			wb.Push(unknown)
			badPT := redPacket(windowCfg, 0, 101, 1)
			badPT.PayloadType = 97
			wb.Push(badPT)

			wb.redundant.mu.Lock()
			count := len(wb.redundant.slots)
			wb.redundant.mu.Unlock()
			c.Check(count == 4, "window rejection left %d positions, want 4", count)

			packet, popErr := wb.Pop()
			redRequirePacket(c, packet, popErr, 100, 100)

			old := redPacket(windowCfg, 0, 99, 99)
			wb.Push(old)
			wb.redundant.mu.Lock()
			count = len(wb.redundant.slots)
			wb.redundant.mu.Unlock()
			c.Check(count == 3, "old packet changed state, count=%d", count)

			head := wb.PlayoutHead()
			_, errPeek := wb.Peek(true)
			_, errPeekSeq := wb.PeekAtSequence(101)
			_, errPopSeq := wb.PopAtSequence(101)
			_, errPopTS := wb.PopAtTimestamp(9101)
			c.Check(errors.Is(errPeek, ErrRedundantMode), "Peek error=%v", errPeek)
			c.Check(errors.Is(errPeekSeq, ErrRedundantMode), "PeekAtSequence error=%v", errPeekSeq)
			c.Check(errors.Is(errPopSeq, ErrRedundantMode), "PopAtSequence error=%v", errPopSeq)
			c.Check(errors.Is(errPopTS, ErrRedundantMode), "PopAtTimestamp error=%v", errPopTS)
			wb.SetPlayoutHead(head + 1000)
			c.Check(wb.PlayoutHead() == head, "SetPlayoutHead changed redundant head")
		}

		// Hold deadline is armed once and not extended by later queries.
		holdClock := newRedClock()
		holdCfg := redTestConfig(holdClock)
		holdCfg.Hold = 20 * time.Millisecond
		hb, holdErr := NewRedundant(holdCfg)
		c.Check(holdErr == nil, "hold constructor failed: %v", holdErr)
		if holdErr == nil {
			hb.Push(redPacket(holdCfg, 0, 10, 10))
			packet, popErr := hb.Pop()
			redRequirePacket(c, packet, popErr, 10, 10)

			hb.Push(redPacket(holdCfg, 1, 12, 12))
			_, popErr = hb.Pop()
			c.Check(errors.Is(popErr, ErrRedundantPending), "before Hold expiry: %v", popErr)

			holdClock.Advance(15 * time.Millisecond)
			hb.Push(redPacket(holdCfg, 0, 13, 13))
			_, popErr = hb.Pop()
			c.Check(errors.Is(popErr, ErrRedundantPending), "later Push extended Hold: %v", popErr)

			holdClock.Advance(5 * time.Millisecond)
			packet, popErr = hb.Pop()
			if redRequirePacket(c, packet, popErr, 12, 12) {
				c.Check(hb.PlayoutHead() == 13, "head after retirement: %d", hb.PlayoutHead())
			}
			packet, popErr = hb.Pop()
			redRequirePacket(c, packet, popErr, 13, 13)
		}

		// Empty queue is pending and does not re-anchor; Clear semantics.
		eb, emptyErr := NewRedundant(cfg)
		c.Check(emptyErr == nil, "empty constructor failed: %v", emptyErr)
		if emptyErr == nil {
			_, popErr := eb.Pop()
			c.Check(errors.Is(popErr, ErrRedundantPending), "empty Pop=%v", popErr)
			eb.Push(redPacket(cfg, 0, 50, 50))
			eb.Clear(false)
			c.Check(eb.PlayoutHead() == 50, "Clear(false) changed head: %d", eb.PlayoutHead())
			_, popErr = eb.Pop()
			c.Check(errors.Is(popErr, ErrRedundantPending), "cleared Pop=%v", popErr)
			eb.Push(redPacket(cfg, 1, 52, 52))
			c.Check(eb.PlayoutHead() == 50, "successor Push re-anchored head: %d", eb.PlayoutHead())
			eb.Push(redPacket(cfg, 0, 50, 50))
			packet, popErr := eb.Pop()
			redRequirePacket(c, packet, popErr, 50, 50)

			eb.Clear(true)
			c.Check(eb.PlayoutHead() == 0, "Clear(true) did not reset head")
			eb.Push(redPacket(cfg, 0, 77, 77))
			c.Check(eb.PlayoutHead() == 77, "first packet after Clear(true) did not anchor")
		}

		// The default packet-count mode keeps its original behavior.
		db := New(WithMinimumPacketCount(1))
		db.Push(&rtp.Packet{Header: rtp.Header{SequenceNumber: 65535}, Payload: []byte{7}})
		db.Push(&rtp.Packet{Header: rtp.Header{SequenceNumber: 0}, Payload: []byte{8}})
		packet, err := db.Pop()
		c.Check(err == nil && packet != nil && packet.SequenceNumber == 65535, "default pop 65535 failed: %v", err)
		packet, err = db.Pop()
		c.Check(err == nil && packet != nil && packet.SequenceNumber == 0, "default pop 0 failed: %v", err)
	})
}

type redRTCPBatch struct {
	packets    []rtcp.Packet
	attributes interceptor.Attributes
}

type redMockRTCP struct {
	mu      sync.Mutex
	batches []redRTCPBatch
	errs    []error
	before  func(index int, packets []rtcp.Packet)
}

func (m *redMockRTCP) Write(packets []rtcp.Packet, attributes interceptor.Attributes) (int, error) {
	m.mu.Lock()
	index := len(m.batches)
	copied := append([]rtcp.Packet(nil), packets...)
	m.batches = append(m.batches, redRTCPBatch{packets: copied, attributes: attributes})
	var err error
	if index < len(m.errs) {
		err = m.errs[index]
	}
	before := m.before
	m.mu.Unlock()

	if before != nil {
		before(index, packets)
	}
	if err != nil {
		return 0, err
	}

	return len(packets), nil
}

func (m *redMockRTCP) callCount() int {
	m.mu.Lock()
	defer m.mu.Unlock()

	return len(m.batches)
}

func (m *redMockRTCP) batch(index int) redRTCPBatch {
	m.mu.Lock()
	defer m.mu.Unlock()

	return m.batches[index]
}

func redNewInterceptorHarness(
	c *taskcheck.C, cfg RedundantConfig,
) (*ReceiverInterceptor, *JitterBuffer) {
	factory, err := NewInterceptor(WithRedundant(cfg))
	c.Check(err == nil, "NewInterceptor factory option construction: %v", err)
	if err != nil {
		return nil, nil
	}
	inter, err := factory.NewInterceptor("redundant-test")
	c.Check(err == nil, "factory.NewInterceptor rejected valid config: %v", err)
	if err != nil {
		return nil, nil
	}
	ri, ok := inter.(*ReceiverInterceptor)
	c.Check(ok, "interceptor type %T", inter)
	if !ok {
		return nil, nil
	}

	return ri, ri.buffer
}

func redNACKFromBatch(c *taskcheck.C, batch redRTCPBatch, index int) *rtcp.TransportLayerNack {
	c.Check(index < len(batch.packets), "batch has no packet %d (len=%d)", index, len(batch.packets))
	if index >= len(batch.packets) {
		return nil
	}
	nack, ok := batch.packets[index].(*rtcp.TransportLayerNack)
	c.Check(ok, "packet %d is %T, not NACK", index, batch.packets[index])

	return nack
}

func TestRedundantFeedback(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		clock := newRedClock()
		cfg := redTestConfig(clock)
		ri, b := redNewInterceptorHarness(c, cfg)
		if ri == nil {
			return
		}
		defer func() { _ = ri.Close() }()

		upstream := &redMockRTCP{}
		writer := ri.BindRTCPWriter(upstream)

		// Canonical 21 and 22 are revealed by received 20 and 23.
		b.Push(redPacket(cfg, 0, 20, 20))
		b.Push(redPacket(cfg, 1, 23, 23))
		nack := &rtcp.TransportLayerNack{
			SenderSSRC: 0x12345678,
			MediaSSRC:  cfg.OutputSSRC,
			Nacks:      rtcp.NackPairsFromSequenceNumbers([]uint16{21, 22}),
		}

		// A failed first send does not commit path/retry state; immediate retry
		// therefore uses the same RTT-selected path.
		upstream.errs = []error{errors.New("simulated send failure")}
		n, err := writer.Write([]rtcp.Packet{nack}, interceptor.Attributes{"trace": "feedback"})
		c.Check(err != nil, "expected simulated first failure")
		c.Check(n == 0, "failed Write n=%d", n)
		c.Check(upstream.callCount() == 1, "upstream calls after failure=%d", upstream.callCount())
		first := redNACKFromBatch(c, upstream.batch(0), 0)
		if first != nil {
			c.Check(first.MediaSSRC == cfg.Paths[0].SSRC, "failed first path SSRC=%d", first.MediaSSRC)
			c.Check(first.SenderSSRC == nack.SenderSSRC, "SenderSSRC not preserved")
			c.Check(len(first.Nacks) == 1 && first.Nacks[0].PacketID == 28,
				"path0 PID want 28 got %+v", first.Nacks)
			c.Check(len(first.Nacks) == 1 && first.Nacks[0].LostPackets == 1,
				"path0 BLP want 1 got %+v", first.Nacks)
		}
		b.redundant.mu.Lock()
		state21 := b.redundant.feedback[21]
		b.redundant.mu.Unlock()
		c.Check(state21 != nil && !state21.hasLast, "failure committed path state: %+v", state21)

		n, err = writer.Write([]rtcp.Packet{nack}, nil)
		c.Check(err == nil, "same-path retry failed: %v", err)
		c.Check(n == 1, "successful transformed batch n=%d", n)
		c.Check(upstream.callCount() == 2, "upstream calls after retry=%d", upstream.callCount())
		retryNACK := redNACKFromBatch(c, upstream.batch(1), 0)
		if retryNACK != nil {
			c.Check(retryNACK.MediaSSRC == cfg.Paths[0].SSRC, "retry changed path: %d", retryNACK.MediaSSRC)
		}

		// Retry suppression.
		n, err = writer.Write([]rtcp.Packet{nack}, nil)
		c.Check(err == nil && n == 0, "suppressed write n=%d err=%v", n, err)
		c.Check(upstream.callCount() == 2, "suppressed NACK called upstream")
		clock.Advance(49 * time.Millisecond)
		_, _ = writer.Write([]rtcp.Packet{nack}, nil)
		c.Check(upstream.callCount() == 2, "NACK sent before Retry deadline")

		// At Retry, rotate to the other path.
		clock.Advance(time.Millisecond)
		_, err = writer.Write([]rtcp.Packet{nack}, nil)
		c.Check(err == nil, "rotated write: %v", err)
		c.Check(upstream.callCount() == 3, "rotation call count=%d", upstream.callCount())
		rotated := redNACKFromBatch(c, upstream.batch(2), 0)
		if rotated != nil {
			c.Check(rotated.MediaSSRC == cfg.Paths[1].SSRC, "rotation SSRC=%d", rotated.MediaSSRC)
			c.Check(len(rotated.Nacks) == 1 && rotated.Nacks[0].PacketID == 120,
				"path1 PID want 120 got %+v", rotated.Nacks)
			c.Check(rotated.Nacks[0].LostPackets == 1, "path1 BLP=%d", rotated.Nacks[0].LostPackets)
		}

		// PID/BLP packing for contiguous and sparse canonical gaps.
		packRI, packB := redNewInterceptorHarness(c, cfg)
		if packRI != nil {
			defer func() { _ = packRI.Close() }()
			packUp := &redMockRTCP{}
			packWriter := packRI.BindRTCPWriter(packUp)
			packB.Push(redPacket(cfg, 0, 40, 40))
			packB.Push(redPacket(cfg, 0, 44, 44))
			packB.Push(redPacket(cfg, 1, 50, 50))
			packB.Push(redPacket(cfg, 1, 55, 55))
			batch := []rtcp.Packet{
				&rtcp.TransportLayerNack{
					SenderSSRC: 7, MediaSSRC: cfg.OutputSSRC,
					Nacks: rtcp.NackPairsFromSequenceNumbers([]uint16{41, 42, 43}),
				},
				&rtcp.TransportLayerNack{
					SenderSSRC: 7, MediaSSRC: cfg.OutputSSRC,
					Nacks: rtcp.NackPairsFromSequenceNumbers([]uint16{51, 52, 54}),
				},
			}
			_, err = packWriter.Write(batch, nil)
			c.Check(err == nil, "PID/BLP write: %v", err)
			c.Check(packUp.callCount() == 1, "PID/BLP upstream calls=%d", packUp.callCount())
			if packUp.callCount() == 1 {
				out := packUp.batch(0)
				c.Check(len(out.packets) == 2, "expanded packet count=%d", len(out.packets))
				contiguous := redNACKFromBatch(c, out, 0)
				if contiguous != nil && len(contiguous.Nacks) == 1 {
					c.Check(contiguous.Nacks[0].PacketID == 48, "contig PID=%d", contiguous.Nacks[0].PacketID)
					c.Check(contiguous.Nacks[0].LostPackets == 3, "contig BLP=%b", contiguous.Nacks[0].LostPackets)
				}
				sparse := redNACKFromBatch(c, out, 1)
				if sparse != nil && len(sparse.Nacks) == 1 {
					c.Check(sparse.Nacks[0].PacketID == 58, "sparse PID=%d", sparse.Nacks[0].PacketID)
					c.Check(sparse.Nacks[0].LostPackets == 5, "sparse BLP=%b", sparse.Nacks[0].LostPackets)
				}
			}
		}

		// Wrapping canonical PID/BLP.
		wrapCfg := cfg
		wrapCfg.Paths[0].SequenceOffset = 0
		wrapRI, wrapB := redNewInterceptorHarness(c, wrapCfg)
		if wrapRI != nil {
			defer func() { _ = wrapRI.Close() }()
			wrapUp := &redMockRTCP{}
			wrapWriter := wrapRI.BindRTCPWriter(wrapUp)
			wrapB.Push(redPacket(wrapCfg, 0, 65533, 1))
			wrapB.Push(redPacket(wrapCfg, 1, 1, 2))
			_, err = wrapWriter.Write([]rtcp.Packet{&rtcp.TransportLayerNack{
				SenderSSRC: 9, MediaSSRC: wrapCfg.OutputSSRC,
				Nacks: rtcp.NackPairsFromSequenceNumbers([]uint16{65534, 65535, 0}),
			}}, nil)
			c.Check(err == nil, "wrap feedback write: %v", err)
			wrapNACK := redNACKFromBatch(c, wrapUp.batch(0), 0)
			if wrapNACK != nil && len(wrapNACK.Nacks) == 1 {
				c.Check(wrapNACK.MediaSSRC == wrapCfg.Paths[0].SSRC, "wrap path SSRC")
				c.Check(wrapNACK.Nacks[0].PacketID == 65534, "wrap PID=%d", wrapNACK.Nacks[0].PacketID)
				c.Check(wrapNACK.Nacks[0].LostPackets == 3, "wrap BLP=%b", wrapNACK.Nacks[0].LostPackets)
			}
		}

		// Filling a gap cancels its feedback even after Retry.
		cancelRI, cancelB := redNewInterceptorHarness(c, cfg)
		if cancelRI != nil {
			defer func() { _ = cancelRI.Close() }()
			cancelUp := &redMockRTCP{}
			cancelWriter := cancelRI.BindRTCPWriter(cancelUp)
			cancelB.Push(redPacket(cfg, 0, 60, 60))
			cancelB.Push(redPacket(cfg, 1, 63, 63))
			cancelNACK := &rtcp.TransportLayerNack{
				MediaSSRC: cfg.OutputSSRC,
				Nacks:     rtcp.NackPairsFromSequenceNumbers([]uint16{61, 62}),
			}
			_, err = cancelWriter.Write([]rtcp.Packet{cancelNACK}, nil)
			c.Check(err == nil && cancelUp.callCount() == 1, "cancel initial send err=%v calls=%d", err, cancelUp.callCount())
			cancelB.Push(redPacket(cfg, 0, 61, 61))
			cancelB.Push(redPacket(cfg, 1, 62, 62))
			clock.Advance(cfg.Retry)
			n, err = cancelWriter.Write([]rtcp.Packet{cancelNACK}, nil)
			c.Check(err == nil && n == 0, "filled gap write n=%d err=%v", n, err)
			c.Check(cancelUp.callCount() == 1, "filled gap still generated feedback")
		}

		// Retired gaps are filtered after Hold expiry/Pop.
		retireClock := newRedClock()
		retireCfg := redTestConfig(retireClock)
		retireCfg.Retry = time.Millisecond
		retireRI, retireB := redNewInterceptorHarness(c, retireCfg)
		if retireRI != nil {
			defer func() { _ = retireRI.Close() }()
			retireUp := &redMockRTCP{}
			retireWriter := retireRI.BindRTCPWriter(retireUp)
			retireB.Push(redPacket(retireCfg, 0, 70, 70))
			_, _ = retireB.Pop()
			retireB.Push(redPacket(retireCfg, 1, 73, 73))
			retireNACK := &rtcp.TransportLayerNack{
				MediaSSRC: retireCfg.OutputSSRC,
				Nacks:     rtcp.NackPairsFromSequenceNumbers([]uint16{71, 72}),
			}
			_, err = retireWriter.Write([]rtcp.Packet{retireNACK}, nil)
			c.Check(err == nil && retireUp.callCount() == 1, "pre-retire NACK err=%v", err)
			retireClock.Advance(retireCfg.Hold)
			packet, popErr := retireB.Pop()
			redRequirePacket(c, packet, popErr, 73, 73)
			retireClock.Advance(retireCfg.Retry)
			n, err = retireWriter.Write([]rtcp.Packet{retireNACK}, nil)
			c.Check(err == nil && n == 0, "retired NACK n=%d err=%v", n, err)
			c.Check(retireUp.callCount() == 1, "retired gap generated feedback")
		}

		// RTT tie goes to Paths[0]; a strictly smaller RTT wins.
		tieCfg := cfg
		tieCfg.Paths[0].RTT = time.Millisecond
		tieCfg.Paths[1].RTT = time.Millisecond
		tieRI, tieB := redNewInterceptorHarness(c, tieCfg)
		if tieRI != nil {
			defer func() { _ = tieRI.Close() }()
			tieUp := &redMockRTCP{}
			tieWriter := tieRI.BindRTCPWriter(tieUp)
			tieB.Push(redPacket(tieCfg, 0, 10, 10))
			tieB.Push(redPacket(tieCfg, 0, 12, 12))
			_, _ = tieWriter.Write([]rtcp.Packet{&rtcp.TransportLayerNack{
				MediaSSRC: tieCfg.OutputSSRC,
				Nacks:     rtcp.NackPairsFromSequenceNumbers([]uint16{11}),
			}}, nil)
			tieNACK := redNACKFromBatch(c, tieUp.batch(0), 0)
			c.Check(tieNACK != nil && tieNACK.MediaSSRC == tieCfg.Paths[0].SSRC, "RTT tie did not choose Paths[0]")
		}

		fastCfg := cfg
		fastCfg.Paths[0].RTT = 100 * time.Millisecond
		fastCfg.Paths[1].RTT = time.Millisecond
		fastRI, fastB := redNewInterceptorHarness(c, fastCfg)
		if fastRI != nil {
			defer func() { _ = fastRI.Close() }()
			fastUp := &redMockRTCP{}
			fastWriter := fastRI.BindRTCPWriter(fastUp)
			fastB.Push(redPacket(fastCfg, 0, 10, 10))
			fastB.Push(redPacket(fastCfg, 0, 12, 12))
			_, _ = fastWriter.Write([]rtcp.Packet{&rtcp.TransportLayerNack{
				MediaSSRC: fastCfg.OutputSSRC,
				Nacks:     rtcp.NackPairsFromSequenceNumbers([]uint16{11}),
			}}, nil)
			fastNACK := redNACKFromBatch(c, fastUp.batch(0), 0)
			c.Check(fastNACK != nil && fastNACK.MediaSSRC == fastCfg.Paths[1].SSRC, "smaller RTT path did not win")
		}

		// Other RTCP packets and non-output NACKs pass through in order.
		passRI, passB := redNewInterceptorHarness(c, cfg)
		if passRI != nil {
			defer func() { _ = passRI.Close() }()
			passUp := &redMockRTCP{}
			passWriter := passRI.BindRTCPWriter(passUp)
			passB.Push(redPacket(cfg, 0, 80, 80))
			passB.Push(redPacket(cfg, 0, 82, 82))
			pli := &rtcp.PictureLossIndication{SenderSSRC: 1, MediaSSRC: 2}
			otherNACK := &rtcp.TransportLayerNack{SenderSSRC: 3, MediaSSRC: 999}
			canonicalNACK := &rtcp.TransportLayerNack{
				SenderSSRC: 4, MediaSSRC: cfg.OutputSSRC,
				Nacks: rtcp.NackPairsFromSequenceNumbers([]uint16{81}),
			}
			n, err := passWriter.Write([]rtcp.Packet{pli, otherNACK, canonicalNACK}, nil)
			c.Check(err == nil && n == 3, "passthrough Write n=%d err=%v", n, err)
			out := passUp.batch(0).packets
			c.Check(len(out) == 3 && out[0] == pli, "PLI order/content changed")
			c.Check(len(out) == 3 && out[1] == otherNACK, "other NACK order/content changed")
			transformed := redNACKFromBatch(c, redRTCPBatch{packets: out}, 2)
			c.Check(transformed != nil && transformed.MediaSSRC == cfg.Paths[0].SSRC, "canonical NACK not transformed in place")

			n, err = passWriter.Write(nil, nil)
			c.Check(err == nil && n == 0 && passUp.callCount() == 1, "empty RTCP batch called upstream")
		}
	})
}

type redMockRTP struct {
	mu        sync.Mutex
	reads     int
	packets   []*rtp.Packet
	attrs     interceptor.Attributes
	readAttrs []interceptor.Attributes
}

func (m *redMockRTP) Read(b []byte, attrs interceptor.Attributes) (int, interceptor.Attributes, error) {
	m.mu.Lock()
	m.reads++
	m.readAttrs = append(m.readAttrs, attrs)
	var packet *rtp.Packet
	if len(m.packets) > 0 {
		packet = m.packets[0]
		m.packets = m.packets[1:]
	}
	returnAttrs := m.attrs
	m.mu.Unlock()

	if packet == nil {
		return 0, returnAttrs, nil
	}
	n, err := packet.MarshalTo(b)
	if err != nil {
		return 0, nil, err
	}

	return n, returnAttrs, nil
}

func (m *redMockRTP) add(packets ...*rtp.Packet) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.packets = append(m.packets, packets...)
}

func (m *redMockRTP) count() int {
	m.mu.Lock()
	defer m.mu.Unlock()

	return m.reads
}

func redBindMock(
	ri *ReceiverInterceptor, cfg RedundantConfig, path int, mock interceptor.RTPReader,
) interceptor.RTPReader {
	return ri.BindRemoteStream(&interceptor.StreamInfo{SSRC: cfg.Paths[path].SSRC}, mock)
}

func redReadCanonical(c *taskcheck.C, reader interceptor.RTPReader, size int) (*rtp.Packet, interceptor.Attributes, error) {
	buf := make([]byte, size)
	n, attrs, err := reader.Read(buf, interceptor.Attributes{"request": "read"})
	if err != nil {
		return nil, attrs, err
	}
	packet := &rtp.Packet{}
	if err := packet.Unmarshal(buf[:n]); err != nil {
		c.Check(false, "unmarshal output: %v", err)
		return nil, attrs, err
	}

	return packet, attrs, nil
}

func TestRedundantLifecycle(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		clock := newRedClock()
		cfg := redTestConfig(clock)
		cfg.Hold = time.Hour
		ri, b := redNewInterceptorHarness(c, cfg)
		if ri == nil {
			return
		}
		defer func() { _ = ri.Close() }()

		// Unconfigured SSRC readers are returned unchanged.
		unknownUp := &redMockRTP{}
		unknownReader := ri.BindRemoteStream(&interceptor.StreamInfo{SSRC: 999}, unknownUp)
		c.Check(unknownReader == unknownUp, "unknown SSRC reader was wrapped")
		_, _, err := unknownReader.Read(make([]byte, 1200), nil)
		c.Check(err == nil, "passthrough reader error: %v", err)
		c.Check(unknownUp.count() == 1, "passthrough upstream call count=%d", unknownUp.count())

		r0 := &redMockRTP{attrs: interceptor.Attributes{"trace": "path0-100"}}
		r1 := &redMockRTP{attrs: interceptor.Attributes{"trace": "path1-102"}}
		reader0 := redBindMock(ri, cfg, 0, r0)
		reader1 := redBindMock(ri, cfg, 1, r1)

		r0.add(redPacket(cfg, 0, 100, 100))
		packet, attrs, err := redReadCanonical(c, reader0, 1200)
		if redRequirePacket(c, packet, err, 100, 100) {
			c.Check(attrs.Get("trace") == "path0-100", "winner attrs=%v", attrs)
		}
		c.Check(r0.count() == 1, "r0 upstream calls=%d", r0.count())

		_, attrs, err = reader1.Read(make([]byte, 1200), nil)
		c.Check(errors.Is(err, ErrRedundantPending), "empty interleaved read=%v", err)
		c.Check(attrs == nil, "pending attrs=%v", attrs)

		r1.add(redPacket(cfg, 1, 102, 102))
		_, _, err = redReadCanonical(c, reader1, 1200)
		c.Check(errors.Is(err, ErrRedundantPending), "successor-only read=%v", err)

		r0.add(redPacket(cfg, 0, 101, 101))
		packet, _, err = redReadCanonical(c, reader0, 1200)
		redRequirePacket(c, packet, err, 101, 101)

		// Existing deliverable 102 is popped by reader1 without another upstream read.
		packet, attrs, err = redReadCanonical(c, reader1, 1200)
		if redRequirePacket(c, packet, err, 102, 102) {
			c.Check(attrs.Get("trace") == "path1-102", "winning attrs not returned: %v", attrs)
		}
		c.Check(r1.count() == 2, "r1 upstream calls after cached pop=%d", r1.count())

		// Either reader can pop a packet won by the other path and gets winner attrs.
		b.redundant.push(
			redPacket(cfg, 1, 103, 103),
			interceptor.Attributes{"winner": "path1-103"},
		)
		packet, attrs, err = redReadCanonical(c, reader0, 1200)
		if redRequirePacket(c, packet, err, 103, 103) {
			c.Check(attrs.Get("winner") == "path1-103", "cross-reader winner attrs=%v", attrs)
		}
		c.Check(r0.count() == 2, "cross-reader pop read upstream unexpectedly")

		// The accepted Attributes map is a shallow copy isolated from source mutation.
		sourceAttrs := interceptor.Attributes{"trace": "first-104"}
		r0.attrs = sourceAttrs
		r0.add(redPacket(cfg, 0, 104, 104))
		packet, attrs, err = redReadCanonical(c, reader0, 1200)
		redRequirePacket(c, packet, err, 104, 104)
		sourceAttrs["trace"] = "mutated-source"
		b.redundant.mu.Lock()
		// 104 was consumed, so test the saved-copy behavior on a buffered duplicate.
		b.redundant.mu.Unlock()
		c.Check(attrs.Get("trace") == "first-104", "returned attrs changed with source")

		b.redundant.push(redPacket(cfg, 0, 105, 105), interceptor.Attributes{"trace": "saved-first"})
		b.redundant.push(redPacket(cfg, 1, 105, 105), interceptor.Attributes{"trace": "ignored-second"})
		b.redundant.mu.Lock()
		saved105 := b.redundant.slots[105]
		savedTrace, _ := saved105.attrs["trace"].(string)
		b.redundant.mu.Unlock()
		c.Check(savedTrace == "saved-first", "duplicate overwrote winner attrs: %s", savedTrace)

		// Short output buffer does not consume the current packet.
		_, _, err = reader0.Read(make([]byte, 1), nil)
		c.Check(errors.Is(err, io.ErrShortBuffer), "short buffer error=%v", err)
		c.Check(r0.count() == 3, "short buffer read upstream unexpectedly: %d", r0.count())
		b.redundant.mu.Lock()
		_, stillPresent := b.redundant.slots[105]
		b.redundant.mu.Unlock()
		c.Check(stillPresent, "short buffer consumed packet")
		packet, attrs, err = redReadCanonical(c, reader1, 1200)
		if redRequirePacket(c, packet, err, 105, 105) {
			c.Check(attrs.Get("trace") == "saved-first", "short retry attrs=%v", attrs)
		}

		// One path unbinds; accepted successor remains and the other path continues.
		unbindRI, unbindB := redNewInterceptorHarness(c, cfg)
		if unbindRI == nil {
			return
		}
		defer func() { _ = unbindRI.Close() }()
		ur0 := &redMockRTP{}
		ur1 := &redMockRTP{}
		ureader0 := redBindMock(unbindRI, cfg, 0, ur0)
		ureader1 := redBindMock(unbindRI, cfg, 1, ur1)
		ur0.add(redPacket(cfg, 0, 200, 200))
		_, _, err = redReadCanonical(c, ureader0, 1200)
		c.Check(err == nil, "unbind setup pop: %v", err)
		ur1.add(redPacket(cfg, 1, 202, 202))
		_, _, err = redReadCanonical(c, ureader1, 1200)
		c.Check(errors.Is(err, ErrRedundantPending), "unbind setup pending: %v", err)
		unbindB.redundant.mu.Lock()
		retainedBefore := len(unbindB.redundant.slots)
		unbindB.redundant.mu.Unlock()
		c.Check(retainedBefore == 1, "setup successor count=%d", retainedBefore)

		unbindRI.UnbindRemoteStream(&interceptor.StreamInfo{SSRC: cfg.Paths[0].SSRC})
		_, _, err = ureader0.Read(make([]byte, 1200), nil)
		c.Check(errors.Is(err, io.ErrClosedPipe), "unbound reader error=%v", err)
		unbindB.redundant.mu.Lock()
		retainedAfter := len(unbindB.redundant.slots)
		unbindB.redundant.mu.Unlock()
		c.Check(retainedAfter == 1, "unbind removed accepted copies")

		rebound0 := &redMockRTP{}
		newReader0 := redBindMock(unbindRI, cfg, 0, rebound0)
		_, _, err = ureader0.Read(make([]byte, 1200), nil)
		c.Check(errors.Is(err, io.ErrClosedPipe), "old reader survived rebind")
		ur1.add(redPacket(cfg, 1, 201, 201))
		packet, _, err = redReadCanonical(c, ureader1, 1200)
		redRequirePacket(c, packet, err, 201, 201)
		packet, _, err = redReadCanonical(c, ureader1, 1200)
		redRequirePacket(c, packet, err, 202, 202)
		rebound0.add(redPacket(cfg, 0, 203, 203))
		packet, _, err = redReadCanonical(c, newReader0, 1200)
		redRequirePacket(c, packet, err, 203, 203)

		// Unbinding both paths clears the shared window but retains the head.
		clearRI, clearB := redNewInterceptorHarness(c, cfg)
		if clearRI == nil {
			return
		}
		defer func() { _ = clearRI.Close() }()
		cr0 := &redMockRTP{}
		cr1 := &redMockRTP{}
		creader0 := redBindMock(clearRI, cfg, 0, cr0)
		creader1 := redBindMock(clearRI, cfg, 1, cr1)
		cr0.add(redPacket(cfg, 0, 300, 300))
		_, _, _ = redReadCanonical(c, creader0, 1200)
		cr1.add(redPacket(cfg, 1, 302, 302))
		_, _, _ = redReadCanonical(c, creader1, 1200)
		clearRI.UnbindRemoteStream(&interceptor.StreamInfo{SSRC: cfg.Paths[0].SSRC})
		clearRI.UnbindRemoteStream(&interceptor.StreamInfo{SSRC: cfg.Paths[1].SSRC})
		clearB.redundant.mu.Lock()
		clearedCount := len(clearB.redundant.slots)
		clearB.redundant.mu.Unlock()
		c.Check(clearedCount == 0, "both-unbind window count=%d", clearedCount)
		c.Check(clearB.PlayoutHead() == 301, "both-unbind head=%d", clearB.PlayoutHead())
		_, _, err = creader1.Read(make([]byte, 1200), nil)
		c.Check(errors.Is(err, io.ErrClosedPipe), "last unbound reader error=%v", err)

		// A synchronous upstream RTCP callback can unbind and rebind the path.
		callbackRI, callbackB := redNewInterceptorHarness(c, cfg)
		if callbackRI == nil {
			return
		}
		defer func() { _ = callbackRI.Close() }()
		cbUp := &redMockRTCP{}
		cbWriter := callbackRI.BindRTCPWriter(cbUp)
		callbackB.Push(redPacket(cfg, 0, 400, 400))
		callbackB.Push(redPacket(cfg, 0, 402, 402))
		cbUp.before = func(int, []rtcp.Packet) {
			callbackRI.UnbindRemoteStream(&interceptor.StreamInfo{SSRC: cfg.Paths[0].SSRC})
			callbackRI.BindRemoteStream(
				&interceptor.StreamInfo{SSRC: cfg.Paths[0].SSRC}, &redMockRTP{},
			)
		}
		cbNACK := &rtcp.TransportLayerNack{
			SenderSSRC: 42, MediaSSRC: cfg.OutputSSRC,
			Nacks: rtcp.NackPairsFromSequenceNumbers([]uint16{401}),
		}
		_, err = cbWriter.Write([]rtcp.Packet{cbNACK}, nil)
		c.Check(err == nil, "reentrant unbind/rebind write: %v", err)
		c.Check(cbUp.callCount() == 1, "reentrant callback upstream calls=%d", cbUp.callCount())
		callbackB.redundant.mu.Lock()
		cbState := callbackB.redundant.feedback[401]
		callbackB.redundant.mu.Unlock()
		c.Check(cbState != nil && !cbState.hasLast, "stale write committed rebound generation")

		// Immediate retry sends again on the same RTT-selected path.
		cbUp.before = nil
		_, err = cbWriter.Write([]rtcp.Packet{cbNACK}, nil)
		c.Check(err == nil, "post-rebind retry: %v", err)
		c.Check(cbUp.callCount() == 2, "post-rebind call count=%d", cbUp.callCount())

		// Close invalidates RTP readers and RTCP feedback entry.
		closeRI, _ := redNewInterceptorHarness(c, cfg)
		if closeRI != nil {
			closeR0 := &redMockRTP{}
			closeReader := redBindMock(closeRI, cfg, 0, closeR0)
			closeUp := &redMockRTCP{}
			closeWriter := closeRI.BindRTCPWriter(closeUp)
			c.Check(closeRI.Close() == nil, "Close returned error")
			_, _, err = closeReader.Read(make([]byte, 1200), nil)
			c.Check(errors.Is(err, io.ErrClosedPipe), "closed reader error=%v", err)
			_, err = closeWriter.Write([]rtcp.Packet{&rtcp.PictureLossIndication{}}, nil)
			c.Check(errors.Is(err, io.ErrClosedPipe), "closed feedback writer error=%v", err)
			c.Check(closeUp.callCount() == 0, "Close still called upstream RTCP")
			c.Check(closeRI.Close() == nil, "second Close returned error")
		}
	})
}
