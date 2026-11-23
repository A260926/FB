// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package flexfec_test

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"testing"

	"github.com/pion/interceptor"
	"github.com/pion/interceptor/pkg/flexfec"
	"github.com/pion/interceptor/task_support/taskcheck"
	"github.com/pion/rtp"
)

const (
	rankMediaSSRC = uint32(4242)
	rankFecSSRC   = uint32(4343)
	rankMediaPT   = uint8(96)
	rankFecPT     = uint8(49)
)

// mediaOptions describes the optional RTP features of a generated packet.
type mediaOptions struct {
	csrc          []uint32
	extensionID   uint8
	extensionData []byte
	paddingSize   byte
	marker        bool
}

func buildMediaPacket(t *testing.T, ssrc uint32, pt uint8, seq uint16, payload []byte, opts mediaOptions) *rtp.Packet {
	t.Helper()

	packet := &rtp.Packet{
		Header: rtp.Header{
			Version:        2,
			PayloadType:    pt,
			SequenceNumber: seq,
			Timestamp:      90000 + uint32(seq)*900,
			SSRC:           ssrc,
			Marker:         opts.marker,
			CSRC:           append([]uint32(nil), opts.csrc...),
		},
		Payload: append([]byte(nil), payload...),
	}

	if len(opts.extensionData) > 0 {
		if err := packet.SetExtension(opts.extensionID, opts.extensionData); err != nil {
			t.Fatalf("set rtp extension: %v", err)
		}
	}

	if opts.paddingSize > 0 {
		packet.Header.Padding = true
		packet.Header.PaddingSize = opts.paddingSize
		packet.PaddingSize = opts.paddingSize
	}

	return packet
}

func mustMarshal(t *testing.T, packet *rtp.Packet) []byte {
	t.Helper()

	raw, err := packet.Marshal()
	if err != nil {
		t.Fatalf("marshal rtp packet: %v", err)
	}

	return raw
}

// buildRepairPacket hand-constructs a FlexFEC-03 repair RTP packet for an
// arbitrary set of protected media sequence numbers. This deliberately does
// not use the production encoder so that rank, duplicate and underdetermined
// scenarios can be shaped directly.
func buildRepairPacket(
	t *testing.T,
	fecSSRC uint32,
	fecPT uint8,
	fecSeq uint16,
	protectedSSRC uint32,
	base uint16,
	members []uint16,
	media map[uint16]*rtp.Packet,
) *rtp.Packet {
	t.Helper()

	maxOffset := uint16(0)

	for _, seq := range members {
		if off := seq - base; off > maxOffset {
			maxOffset = off
		}
	}

	headerSize := flexfec.BaseFec03HeaderSize
	if maxOffset >= 15 {
		headerSize += 4
	}

	if maxOffset >= 46 {
		headerSize += 8
	}

	maxBody := 0
	rawMedia := make(map[uint16][]byte, len(members))

	for _, seq := range members {
		raw := mustMarshal(t, media[seq])
		rawMedia[seq] = raw

		if bodyLen := len(raw) - flexfec.BaseRTPHeaderSize; bodyLen > maxBody {
			maxBody = bodyLen
		}
	}

	fecPayload := make([]byte, headerSize+maxBody)
	header := fecPayload[:headerSize]

	for _, seq := range members {
		raw := rawMedia[seq]
		header[0] ^= raw[0]
		header[1] ^= raw[1]
		lengthRecovery := uint16(len(raw) - flexfec.BaseRTPHeaderSize) //nolint:gosec
		header[2] ^= uint8(lengthRecovery >> 8)                        //nolint:gosec
		header[3] ^= uint8(lengthRecovery)                             //nolint:gosec

		for i := 4; i < 8; i++ {
			header[i] ^= raw[i]
		}

		for i := flexfec.BaseRTPHeaderSize; i < len(raw); i++ {
			fecPayload[headerSize+i-flexfec.BaseRTPHeaderSize] ^= raw[i]
		}
	}

	header[0] &= 0x3f
	header[8] = 1
	binary.BigEndian.PutUint32(header[12:16], protectedSSRC)
	binary.BigEndian.PutUint16(header[16:18], base)

	var mask0, mask1, mask2 uint64

	for _, seq := range members {
		off := uint64(seq - base)
		switch {
		case off < 15:
			mask0 |= 1 << (14 - off)
		case off < 46:
			mask1 |= 1 << (30 - (off - 15))
		default:
			mask2 |= 1 << (62 - (off - 46))
		}
	}

	switch {
	case maxOffset < 15:
		binary.BigEndian.PutUint16(header[18:20], uint16(mask0)|0x8000) //nolint:gosec
	case maxOffset < 46:
		binary.BigEndian.PutUint16(header[18:20], uint16(mask0))            //nolint:gosec
		binary.BigEndian.PutUint32(header[20:24], uint32(mask1)|0x80000000) //nolint:gosec
	default:
		binary.BigEndian.PutUint16(header[18:20], uint16(mask0)) //nolint:gosec
		binary.BigEndian.PutUint32(header[20:24], uint32(mask1)) //nolint:gosec
		binary.BigEndian.PutUint64(header[24:32], mask2|0x8000000000000000)
	}

	return &rtp.Packet{
		Header: rtp.Header{
			Version:        2,
			PayloadType:    fecPT,
			SequenceNumber: fecSeq,
			SSRC:           fecSSRC,
		},
		Payload: fecPayload,
	}
}

type scriptedDatagram struct {
	raw   []byte
	attrs interceptor.Attributes
	err   error
}

type scriptedReader struct {
	items []scriptedDatagram
	reads int
}

func (s *scriptedReader) Read(buf []byte, _ interceptor.Attributes) (int, interceptor.Attributes, error) {
	s.reads++

	if len(s.items) == 0 {
		return 0, nil, io.EOF
	}

	item := s.items[0]
	s.items = s.items[1:]

	if item.err != nil {
		return 0, nil, item.err
	}

	copy(buf, item.raw)

	return len(item.raw), item.attrs, nil
}

func streamInfo() *interceptor.StreamInfo {
	return &interceptor.StreamInfo{
		SSRC:                              rankMediaSSRC,
		PayloadType:                       rankMediaPT,
		SSRCForwardErrorCorrection:        rankFecSSRC,
		PayloadTypeForwardErrorCorrection: rankFecPT,
	}
}

func datagram(t *testing.T, packet *rtp.Packet, attrs interceptor.Attributes) scriptedDatagram {
	t.Helper()

	return scriptedDatagram{raw: mustMarshal(t, packet), attrs: attrs}
}

// drainRead keeps reading until the upstream is exhausted and returns the
// delivered media packets.
func drainRead(t *testing.T, c *taskcheck.C, reader interceptor.RTPReader) ([]*rtp.Packet, []interceptor.Attributes, []error) {
	t.Helper()

	packets := make([]*rtp.Packet, 0)
	attrsOut := make([]interceptor.Attributes, 0)
	errs := make([]error, 0)
	buf := make([]byte, 1500)

	for range 64 {
		n, attrs, err := reader.Read(buf, interceptor.Attributes{})
		if errors.Is(err, io.EOF) {
			return packets, attrsOut, errs
		}

		if err != nil {
			if errors.Is(err, flexfec.ErrRepairPending) {
				continue
			}

			errs = append(errs, err)

			continue
		}

		c.Check(n > 0, "delivered datagram must carry bytes")

		packet := &rtp.Packet{}
		if err := packet.Unmarshal(append([]byte(nil), buf[:n]...)); err != nil {
			t.Fatalf("unmarshal delivered packet: %v", err)
		}

		packets = append(packets, packet)
		attrsOut = append(attrsOut, attrs)
	}

	t.Fatal("drainRead did not reach EOF within the iteration budget")

	return nil, nil, nil
}

func makeMediaSet(t *testing.T, seqs []uint16, opts mediaOptions) map[uint16]*rtp.Packet {
	t.Helper()

	media := make(map[uint16]*rtp.Packet, len(seqs))

	for i, seq := range seqs {
		payload := []byte{0x61, 0x62, 0x63, byte(i), byte(seq >> 8), byte(seq)}
		media[seq] = buildMediaPacket(t, rankMediaSSRC, rankMediaPT, seq, payload, opts)
	}

	return media
}

// TestRepairRank focuses on GF(2) rank behavior: multi-row simultaneous
// solving, underdetermined systems, duplicates and encoder-based single loss.
func TestRepairRank(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t.Run("three independent rows recover three missing packets", func(t *testing.T) {
			inter, err := flexfec.NewRepairReceiver(32, 32)
			c.Check(err == nil, "constructor error: %v", err)

			seqs := []uint16{100, 101, 102}
			media := makeMediaSet(t, seqs, mediaOptions{marker: true})

			upstream := &scriptedReader{}
			reader := inter.BindRemoteStream(streamInfo(), upstream)

			// All three originals are missing: only repair packets arrive.
			upstream.items = append(upstream.items,
				datagram(t, buildRepairPacket(t, rankFecSSRC, rankFecPT, 1000, rankMediaSSRC, 100, []uint16{100, 101}, media), interceptor.Attributes{"fec": 1}),
				datagram(t, buildRepairPacket(t, rankFecSSRC, rankFecPT, 1001, rankMediaSSRC, 100, []uint16{101, 102}, media), interceptor.Attributes{"fec": 2}),
				datagram(t, buildRepairPacket(t, rankFecSSRC, rankFecPT, 1002, rankMediaSSRC, 100, []uint16{100, 101, 102}, media), interceptor.Attributes{"fec": 3}),
			)

			buf := make([]byte, 1500)
			for range 3 {
				_, _, err := reader.Read(buf, interceptor.Attributes{})
				c.Check(errors.Is(err, flexfec.ErrRepairPending), "fec-only reads must be pending, got %v", err)
			}

			// A later media packet opens the window; the held 3x3 system solves
			// all three missing packets at once.
			opener := buildMediaPacket(t, rankMediaSSRC, rankMediaPT, 103, []byte{0x78}, mediaOptions{})
			upstream.items = append(upstream.items, datagram(t, opener, interceptor.Attributes{"media": 103}))

			packets, attrs, errs := drainRead(t, c, reader)
			c.Check(len(errs) == 0, "unexpected errors: %v", errs)
			c.Check(len(packets) == 4, "want opener plus 3 recovered, got %d", len(packets))

			for i, seq := range seqs {
				// First three outputs are the recovered packets in ESN order.
				c.Check(packets[i].SequenceNumber == seq, "recovered order %d: got seq %d want %d", i, packets[i].SequenceNumber, seq)
				c.Check(bytes.Equal(mustMarshal(t, packets[i]), mustMarshal(t, media[seq])),
					"recovered bytes for seq %d do not match the original", seq)
				recovered, ok := attrs[i].Get(flexfec.AttributeKeyRecovered).(bool)
				c.Check(ok && recovered, "seq %d attributes must mark flexfec.recovered=true, got %#v", seq, attrs[i])
			}

			c.Check(packets[3].SequenceNumber == 103, "window opener delivered last")
			_, openerRecovered := attrs[3].Get(flexfec.AttributeKeyRecovered).(bool)
			c.Check(!openerRecovered, "original media must not carry the recovered attribute")
		})

		t.Run("underdetermined equations never guess packets", func(t *testing.T) {
			inter, err := flexfec.NewRepairReceiver(32, 32)
			c.Check(err == nil, "constructor error: %v", err)

			seqs := []uint16{200, 201, 202}
			media := makeMediaSet(t, seqs, mediaOptions{})
			upstream := &scriptedReader{}
			reader := inter.BindRemoteStream(streamInfo(), upstream)

			upstream.items = append(upstream.items,
				datagram(t, buildRepairPacket(t, rankFecSSRC, rankFecPT, 2000, rankMediaSSRC, 200, []uint16{200, 201}, media), nil),
				datagram(t, buildRepairPacket(t, rankFecSSRC, rankFecPT, 2001, rankMediaSSRC, 200, []uint16{201, 202}, media), nil),
				datagram(t, buildMediaPacket(t, rankMediaSSRC, rankMediaPT, 203, []byte{0x79}, mediaOptions{}), nil),
			)

			packets, _, errs := drainRead(t, c, reader)
			c.Check(len(errs) == 0, "unexpected errors: %v", errs)
			c.Check(len(packets) == 1 && packets[0].SequenceNumber == 203,
				"underdetermined system must not invent packets, got %d", len(packets))

			// Completing the rank now solves the remaining unknowns.
			upstream.items = append(upstream.items, datagram(t, media[200], nil))
			packets, _, errs = drainRead(t, c, reader)
			c.Check(len(errs) == 0, "unexpected errors: %v", errs)
			c.Check(len(packets) == 3, "want original plus 2 recovered packets, got %d", len(packets))
			c.Check(packets[0].SequenceNumber == 200 && packets[1].SequenceNumber == 201 && packets[2].SequenceNumber == 202,
				"delivery order mismatch: %d %d %d", packets[0].SequenceNumber, packets[1].SequenceNumber, packets[2].SequenceNumber)
			c.Check(bytes.Equal(mustMarshal(t, packets[1]), mustMarshal(t, media[201])), "seq 201 bytes mismatch")
			c.Check(bytes.Equal(mustMarshal(t, packets[2]), mustMarshal(t, media[202])), "seq 202 bytes mismatch")
		})

		t.Run("duplicate and dependent equations produce no duplicate output", func(t *testing.T) {
			inter, err := flexfec.NewRepairReceiver(32, 32)
			c.Check(err == nil, "constructor error: %v", err)

			seqs := []uint16{300, 301}
			media := makeMediaSet(t, seqs, mediaOptions{})
			upstream := &scriptedReader{}
			reader := inter.BindRemoteStream(streamInfo(), upstream)

			dup := buildRepairPacket(t, rankFecSSRC, rankFecPT, 3000, rankMediaSSRC, 300, []uint16{300, 301}, media)
			upstream.items = append(upstream.items,
				datagram(t, dup, nil),
				datagram(t, dup, nil), // identical wire repeat
				datagram(t, buildRepairPacket(t, rankFecSSRC, rankFecPT, 3001, rankMediaSSRC, 300, []uint16{300, 301}, media), nil),
			)

			buf := make([]byte, 1500)
			for range 3 {
				_, _, err := reader.Read(buf, interceptor.Attributes{})
				c.Check(errors.Is(err, flexfec.ErrRepairPending), "duplicate rows stay pending, got %v", err)
			}

			upstream.items = append(upstream.items, datagram(t, media[301], nil))
			packets, _, errs := drainRead(t, c, reader)
			c.Check(len(errs) == 0, "unexpected errors: %v", errs)
			c.Check(len(packets) == 2, "original plus exactly one recovery, got %d", len(packets))

			delivered := map[uint16]int{}
			for _, packet := range packets {
				delivered[packet.SequenceNumber]++
			}

			c.Check(delivered[300] == 1 && delivered[301] == 1, "each sequence delivered once: %v", delivered)
		})

		t.Run("production encoder single loss round trip stays compatible", func(t *testing.T) {
			inter, err := flexfec.NewRepairReceiver(32, 32)
			c.Check(err == nil, "constructor error: %v", err)

			encoder := flexfec.NewFlexEncoder03(rankFecPT, rankFecSSRC)
			seqs := []uint16{400, 401, 402, 403, 404}
			media := make([]rtp.Packet, 0, len(seqs))

			for _, seq := range seqs {
				media = append(media, *buildMediaPacket(t, rankMediaSSRC, rankMediaPT, seq,
					[]byte{1, 2, 3, 4, 5, byte(seq)}, mediaOptions{marker: true}))
			}

			fecPackets := encoder.EncodeFec(media, 2)
			c.Check(len(fecPackets) == 2, "encoder should emit two repair packets")

			upstream := &scriptedReader{}
			reader := inter.BindRemoteStream(streamInfo(), upstream)

			// Lose seq 402: deliver the other originals and both FEC packets.
			for _, packet := range media {
				if packet.SequenceNumber == 402 {
					continue
				}

				upstream.items = append(upstream.items, datagram(t, &packet, nil))
			}

			for i := range fecPackets {
				upstream.items = append(upstream.items, datagram(t, &fecPackets[i], nil))
			}

			packets, attrs, errs := drainRead(t, c, reader)
			c.Check(len(errs) == 0, "unexpected errors: %v", errs)

			var recovered *rtp.Packet

			var recoveredAttrs interceptor.Attributes

			for i, packet := range packets {
				if packet.SequenceNumber == 402 {
					recovered = packet
					recoveredAttrs = attrs[i]
				}
			}

			c.Check(recovered != nil, "lost packet 402 was not recovered")
			c.Check(bytes.Equal(mustMarshal(t, recovered), mustMarshal(t, &media[2])), "recovered bytes differ from encoder input")
			flag, ok := recoveredAttrs.Get(flexfec.AttributeKeyRecovered).(bool)
			c.Check(ok && flag, "recovered attribute missing on encoder-driven recovery")
		})

		t.Run("variable length extensions and padding survive recovery", func(t *testing.T) {
			inter, err := flexfec.NewRepairReceiver(32, 32)
			c.Check(err == nil, "constructor error: %v", err)

			media := map[uint16]*rtp.Packet{
				500: buildMediaPacket(t, rankMediaSSRC, rankMediaPT, 500, []byte{0xa1, 0xa2},
					mediaOptions{extensionID: 1, extensionData: []byte{0x10}, paddingSize: 4}),
				501: buildMediaPacket(t, rankMediaSSRC, rankMediaPT, 501, []byte{0xb1, 0xb2, 0xb3, 0xb4},
					mediaOptions{extensionID: 1, extensionData: []byte{0x20, 0x21, 0x22}, csrc: []uint32{7, 8}}),
				502: buildMediaPacket(t, rankMediaSSRC, rankMediaPT, 502, []byte{0xc1},
					mediaOptions{extensionID: 2, extensionData: []byte{0x30}, paddingSize: 8, marker: true}),
			}

			upstream := &scriptedReader{}
			reader := inter.BindRemoteStream(streamInfo(), upstream)
			upstream.items = append(upstream.items,
				datagram(t, buildRepairPacket(t, rankFecSSRC, rankFecPT, 5000, rankMediaSSRC, 500, []uint16{500, 501}, media), nil),
				datagram(t, buildRepairPacket(t, rankFecSSRC, rankFecPT, 5001, rankMediaSSRC, 500, []uint16{501, 502}, media), nil),
				datagram(t, buildRepairPacket(t, rankFecSSRC, rankFecPT, 5002, rankMediaSSRC, 500, []uint16{500, 501, 502}, media), nil),
				datagram(t, buildMediaPacket(t, rankMediaSSRC, rankMediaPT, 503, []byte{0xdd}, mediaOptions{}), nil),
			)

			packets, _, errs := drainRead(t, c, reader)
			c.Check(len(errs) == 0, "unexpected errors: %v", errs)

			bySeq := map[uint16]*rtp.Packet{}
			for _, packet := range packets {
				bySeq[packet.SequenceNumber] = packet
			}

			for _, seq := range []uint16{500, 501, 502} {
				c.Check(bytes.Equal(mustMarshal(t, bySeq[seq]), mustMarshal(t, media[seq])),
					"seq %d extension/padding bytes do not match", seq)
				c.Check(bySeq[seq].Padding == media[seq].Padding, "seq %d padding flag mismatch", seq)
				c.Check(len(bySeq[seq].CSRC) == len(media[seq].CSRC), "seq %d csrc mismatch", seq)
				c.Check(bySeq[seq].Extension == media[seq].Extension, "seq %d extension flag mismatch", seq)
			}
		})

		t.Run("long mask spanning two mask sections still peels a single loss", func(t *testing.T) {
			inter, err := flexfec.NewRepairReceiver(64, 64)
			c.Check(err == nil, "constructor error: %v", err)

			const count = 20
			seqs := make([]uint16, count)
			media := make(map[uint16]*rtp.Packet, count)

			for i := range count {
				seqs[i] = uint16(600 + i) //nolint:gosec
				media[seqs[i]] = buildMediaPacket(t, rankMediaSSRC, rankMediaPT, seqs[i], []byte{byte(i), 0x55}, mediaOptions{})
			}

			upstream := &scriptedReader{}
			reader := inter.BindRemoteStream(streamInfo(), upstream)

			for _, seq := range seqs {
				if seq == 607 {
					continue // single loss inside the 31-bit mask section
				}

				upstream.items = append(upstream.items, datagram(t, media[seq], nil))
			}

			upstream.items = append(upstream.items,
				datagram(t, buildRepairPacket(t, rankFecSSRC, rankFecPT, 6000, rankMediaSSRC, 600, seqs, media), nil),
			)

			packets, _, errs := drainRead(t, c, reader)
			c.Check(len(errs) == 0, "unexpected errors: %v", errs)

			var recovered *rtp.Packet

			for _, packet := range packets {
				if packet.SequenceNumber == 607 {
					recovered = packet
				}
			}

			c.Check(recovered != nil, "seq 607 not recovered from long mask")
			c.Check(bytes.Equal(mustMarshal(t, recovered), mustMarshal(t, media[607])), "long mask recovery bytes mismatch")
		})
	})
}

// TestRepairWindow focuses on sequence windows: wrap-around, unknown-column
// eviction, equation quota and late duplicates.
func TestRepairWindow(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t.Run("recovery works across the 65535 to 0 wrap", func(t *testing.T) {
			inter, err := flexfec.NewRepairReceiver(8, 8)
			c.Check(err == nil, "constructor error: %v", err)

			seqs := []uint16{65534, 65535, 0, 1}
			media := makeMediaSet(t, seqs, mediaOptions{marker: true})
			upstream := &scriptedReader{}
			reader := inter.BindRemoteStream(streamInfo(), upstream)

			upstream.items = append(upstream.items,
				datagram(t, media[65534], nil),
				datagram(t, buildRepairPacket(t, rankFecSSRC, rankFecPT, 9000, rankMediaSSRC, 65535,
					[]uint16{65535, 0}, media), nil),
				datagram(t, buildRepairPacket(t, rankFecSSRC, rankFecPT, 9001, rankMediaSSRC, 65535,
					[]uint16{0, 1}, media), nil),
				datagram(t, media[1], nil),
			)

			packets, _, errs := drainRead(t, c, reader)
			c.Check(len(errs) == 0, "unexpected errors: %v", errs)
			c.Check(len(packets) == 4, "want two originals and two recovered, got %d", len(packets))
			c.Check(packets[0].SequenceNumber == 65534, "first output seq mismatch")
			c.Check(packets[1].SequenceNumber == 65535 && packets[2].SequenceNumber == 0,
				"wrapped recovery order mismatch: %d %d", packets[1].SequenceNumber, packets[2].SequenceNumber)
			c.Check(packets[3].SequenceNumber == 1, "last output seq mismatch")
			c.Check(bytes.Equal(mustMarshal(t, packets[1]), mustMarshal(t, media[65535])), "seq 65535 bytes mismatch")
			c.Check(bytes.Equal(mustMarshal(t, packets[2]), mustMarshal(t, media[0])), "seq 0 bytes mismatch")
		})

		t.Run("unknown column sliding out discards the whole equation", func(t *testing.T) {
			inter, err := flexfec.NewRepairReceiver(4, 16)
			c.Check(err == nil, "constructor error: %v", err)

			media := makeMediaSet(t, []uint16{700, 701, 702, 703, 704, 705, 706, 707}, mediaOptions{})
			upstream := &scriptedReader{}
			reader := inter.BindRemoteStream(streamInfo(), upstream)

			upstream.items = append(upstream.items,
				datagram(t, media[702], nil),
				// Both protected packets are unknown; the row must be dropped
				// whole once either column leaves the window.
				datagram(t, buildRepairPacket(t, rankFecSSRC, rankFecPT, 9100, rankMediaSSRC, 700,
					[]uint16{700, 701}, media), nil),
				datagram(t, media[703], nil),
				datagram(t, media[704], nil),
				datagram(t, media[705], nil),
				// Fresh equation inside the live window.
				datagram(t, buildRepairPacket(t, rankFecSSRC, rankFecPT, 9101, rankMediaSSRC, 704,
					[]uint16{704, 706}, media), nil),
				datagram(t, media[707], nil),
			)

			packets, _, errs := drainRead(t, c, reader)
			c.Check(len(errs) == 0, "unexpected errors: %v", errs)

			delivered := map[uint16]bool{}
			for _, packet := range packets {
				c.Check(packet.SSRC == rankMediaSSRC, "repair packet must never be exposed")
				delivered[packet.SequenceNumber] = true
			}

			c.Check(!delivered[700] && !delivered[701], "evicted unknowns must not be guessed: %v", delivered)
			c.Check(delivered[706], "live equation should still recover seq 706")

			var recovered706 *rtp.Packet
			for _, packet := range packets {
				if packet.SequenceNumber == 706 {
					recovered706 = packet
				}
			}

			c.Check(bytes.Equal(mustMarshal(t, recovered706), mustMarshal(t, media[706])), "seq 706 recovery bytes mismatch")
		})

		t.Run("equation quota evicts the oldest independent equation", func(t *testing.T) {
			inter, err := flexfec.NewRepairReceiver(32, 3)
			c.Check(err == nil, "constructor error: %v", err)

			media := makeMediaSet(t, []uint16{800, 801, 802, 803, 804, 805}, mediaOptions{})
			upstream := &scriptedReader{}
			reader := inter.BindRemoteStream(streamInfo(), upstream)

			upstream.items = append(upstream.items,
				datagram(t, buildRepairPacket(t, rankFecSSRC, rankFecPT, 9200, rankMediaSSRC, 800, []uint16{800, 801}, media), nil),
				datagram(t, buildRepairPacket(t, rankFecSSRC, rankFecPT, 9201, rankMediaSSRC, 800, []uint16{801, 802}, media), nil),
				datagram(t, buildRepairPacket(t, rankFecSSRC, rankFecPT, 9202, rankMediaSSRC, 800, []uint16{802, 803}, media), nil),
				// Fourth independent row evicts the 9200 equation.
				datagram(t, buildRepairPacket(t, rankFecSSRC, rankFecPT, 9203, rankMediaSSRC, 800, []uint16{803, 804}, media), nil),
				datagram(t, media[800], nil),
				datagram(t, media[801], nil),
				datagram(t, media[802], nil),
				datagram(t, media[803], nil),
				// Frontier opener covering the held 804 solution.
				datagram(t, media[805], nil),
			)

			packets, _, errs := drainRead(t, c, reader)
			c.Check(len(errs) == 0, "unexpected errors: %v", errs)

			delivered := map[uint16]bool{}
			for _, packet := range packets {
				delivered[packet.SequenceNumber] = true
			}

			c.Check(delivered[800], "original 800 must be delivered")
			c.Check(delivered[804], "804 is solvable from the retained rows")
			c.Check(len(packets) == 6, "four originals, one recovery and one opener, got %d", len(packets))

			var recovered804 *rtp.Packet
			for _, packet := range packets {
				if packet.SequenceNumber == 804 {
					recovered804 = packet
				}
			}

			c.Check(bytes.Equal(mustMarshal(t, recovered804), mustMarshal(t, media[804])), "seq 804 recovery bytes mismatch")
			c.Check(packets[4].SequenceNumber == 804 && packets[5].SequenceNumber == 805, "recovered 804 must precede opener 805")
		})

		t.Run("late duplicates inside and outside the window deliver at most once", func(t *testing.T) {
			inter, err := flexfec.NewRepairReceiver(3, 16)
			c.Check(err == nil, "constructor error: %v", err)

			media := makeMediaSet(t, []uint16{810, 811, 812, 813}, mediaOptions{})
			upstream := &scriptedReader{}
			reader := inter.BindRemoteStream(streamInfo(), upstream)

			upstream.items = append(upstream.items,
				datagram(t, media[810], nil),
				datagram(t, media[811], nil),
				datagram(t, media[812], nil),
				datagram(t, media[813], nil),
			)

			first, _, errs := drainRead(t, c, reader)
			c.Check(len(errs) == 0, "unexpected errors: %v", errs)
			c.Check(len(first) == 4, "four originals expected")

			// Re-send everything: every duplicate must be suppressed.
			upstream.items = append(upstream.items,
				datagram(t, media[810], nil),
				datagram(t, media[812], nil),
				// A late repair row for evicted unknowns must not invent data.
				datagram(t, buildRepairPacket(t, rankFecSSRC, rankFecPT, 9300, rankMediaSSRC, 810,
					[]uint16{810, 811}, media), nil),
			)

			buf := make([]byte, 1500)
			pending := 0

			for range 3 {
				_, _, err := reader.Read(buf, interceptor.Attributes{})
				if errors.Is(err, flexfec.ErrRepairPending) {
					pending++
				} else if err != nil {
					t.Fatalf("unexpected read error: %v", err)
				}
			}

			c.Check(pending == 3, "all duplicate/late inputs must return ErrRepairPending, got %d", pending)
		})
	})
}

// TestRepairIngress focuses on the interceptor entry point: bindings,
// conflicts, snapshots, lifecycle and error handling.
func TestRepairIngress(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t.Run("constructor rejects capacities outside 2 to 512", func(t *testing.T) {
			for _, params := range [][2]uint16{{1, 8}, {513, 8}, {8, 1}, {8, 513}, {0, 0}} {
				inter, err := flexfec.NewRepairReceiver(params[0], params[1])
				c.Check(err != nil, "NewRepairReceiver(%d,%d) must fail", params[0], params[1])
				c.Check(inter == nil, "failed constructor must return a nil interceptor")
			}

			inter, err := flexfec.NewRepairReceiver(2, 512)
			c.Check(err == nil && inter != nil, "boundary capacities must be accepted")
		})

		t.Run("streams without complete FEC identity pass through", func(t *testing.T) {
			inter, err := flexfec.NewRepairReceiver(16, 16)
			c.Check(err == nil, "constructor error: %v", err)

			upstream := &scriptedReader{}
			info := &interceptor.StreamInfo{SSRC: 5555, PayloadType: 96}
			reader := inter.BindRemoteStream(info, upstream)
			c.Check(reader != nil, "passthrough reader must be returned")

			packet := buildMediaPacket(t, 5555, 96, 1, []byte{1, 2, 3}, mediaOptions{})
			raw := mustMarshal(t, packet)
			upstream.items = append(upstream.items, scriptedDatagram{raw: raw, attrs: interceptor.Attributes{"k": "v"}})

			buf := make([]byte, 1500)
			n, attrs, err := reader.Read(buf, interceptor.Attributes{})
			c.Check(err == nil, "passthrough read error: %v", err)
			c.Check(n == len(raw) && bytes.Equal(buf[:n], raw), "passthrough bytes must be unchanged")
			c.Check(attrs.Get("k") == "v", "passthrough attributes must survive")
		})

		t.Run("two streams stay isolated and conflicts do not mutate bindings", func(t *testing.T) {
			inter, err := flexfec.NewRepairReceiver(16, 16)
			c.Check(err == nil, "constructor error: %v", err)

			up1 := &scriptedReader{}
			up2 := &scriptedReader{}
			info1 := &interceptor.StreamInfo{
				SSRC: 1000, PayloadType: 96,
				SSRCForwardErrorCorrection: 2000, PayloadTypeForwardErrorCorrection: 49,
			}
			info2 := &interceptor.StreamInfo{
				SSRC: 3000, PayloadType: 97,
				SSRCForwardErrorCorrection: 4000, PayloadTypeForwardErrorCorrection: 50,
			}

			reader1 := inter.BindRemoteStream(info1, up1)
			reader2 := inter.BindRemoteStream(info2, up2)
			c.Check(reader1 != nil && reader2 != nil, "both bindings should succeed")

			conflict := &interceptor.StreamInfo{
				SSRC: 5000, PayloadType: 96,
				SSRCForwardErrorCorrection: 2000, PayloadTypeForwardErrorCorrection: 49,
			}
			upConflict := &scriptedReader{}
			badReader := inter.BindRemoteStream(conflict, upConflict)
			_, _, conflictErr := badReader.Read(make([]byte, 1500), interceptor.Attributes{})
			c.Check(conflictErr != nil, "conflicting binding must return an error-only reader")
			c.Check(upConflict.reads == 0, "conflict reader must not read upstream")

			media1 := map[uint16]*rtp.Packet{
				10: buildMediaPacket(t, 1000, 96, 10, []byte{0x10}, mediaOptions{}),
				11: buildMediaPacket(t, 1000, 96, 11, []byte{0x11}, mediaOptions{}),
			}
			media2 := map[uint16]*rtp.Packet{
				20: buildMediaPacket(t, 3000, 97, 20, []byte{0x20}, mediaOptions{}),
				21: buildMediaPacket(t, 3000, 97, 21, []byte{0x21}, mediaOptions{}),
			}

			up1.items = append(up1.items,
				datagram(t, buildRepairPacket(t, 2000, 49, 7000, 1000, 10, []uint16{10, 11}, media1), nil),
				datagram(t, media1[11], nil),
			)
			up2.items = append(up2.items,
				datagram(t, buildRepairPacket(t, 4000, 50, 7001, 3000, 20, []uint16{20, 21}, media2), nil),
				datagram(t, media2[21], nil),
			)

			packets1, _, errs1 := drainRead(t, c, reader1)
			packets2, _, errs2 := drainRead(t, c, reader2)
			c.Check(len(errs1) == 0 && len(errs2) == 0, "stream errors: %v %v", errs1, errs2)

			for _, packet := range packets1 {
				c.Check(packet.SSRC == 1000, "stream1 must only expose its own media SSRC")
			}

			for _, packet := range packets2 {
				c.Check(packet.SSRC == 3000, "stream2 must only expose its own media SSRC")
			}

			c.Check(len(packets1) == 2 && len(packets2) == 2, "both streams should recover their missing packet")
			c.Check(bytes.Equal(mustMarshal(t, packets1[0]), mustMarshal(t, media1[10])), "stream1 recovery bytes mismatch")
			c.Check(bytes.Equal(mustMarshal(t, packets2[0]), mustMarshal(t, media2[20])), "stream2 recovery bytes mismatch")
		})

		t.Run("payload type mismatches and unsupported FEC formats are rejected without commit", func(t *testing.T) {
			inter, err := flexfec.NewRepairReceiver(16, 16)
			c.Check(err == nil, "constructor error: %v", err)

			upstream := &scriptedReader{}
			reader := inter.BindRemoteStream(streamInfo(), upstream)

			wrongPT := buildMediaPacket(t, rankFecSSRC, 7, 1, []byte{1}, mediaOptions{})
			upstream.items = append(upstream.items, datagram(t, wrongPT, nil))
			_, _, err = reader.Read(make([]byte, 1500), interceptor.Attributes{})
			c.Check(err != nil, "FEC SSRC with a foreign payload type must fail")

			// R bit set: explicitly unsupported FlexFEC format.
			unsupported := &rtp.Packet{
				Header:  rtp.Header{Version: 2, PayloadType: rankFecPT, SSRC: rankFecSSRC, SequenceNumber: 5},
				Payload: bytes.Repeat([]byte{0xff}, 20),
			}
			upstream.items = append(upstream.items, datagram(t, unsupported, nil))
			_, _, err = reader.Read(make([]byte, 1500), interceptor.Attributes{})
			c.Check(err != nil, "R=1 flexfec must be rejected explicitly")

			truncated := mustMarshal(t, buildMediaPacket(t, rankMediaSSRC, rankMediaPT, 9, []byte{1}, mediaOptions{}))[:2]
			upstream.items = append(upstream.items, scriptedDatagram{raw: truncated})
			_, _, err = reader.Read(make([]byte, 1500), interceptor.Attributes{})
			c.Check(err != nil, "truncated RTP datagram must fail")

			// No state was committed: a normal media packet still flows.
			good := buildMediaPacket(t, rankMediaSSRC, rankMediaPT, 9, []byte{1, 2}, mediaOptions{})
			upstream.items = append(upstream.items, datagram(t, good, interceptor.Attributes{"after": "ok"}))
			n, attrs, err := reader.Read(make([]byte, 1500), interceptor.Attributes{})
			c.Check(err == nil && n > 0, "valid packet after errors must still work: %v", err)
			c.Check(attrs.Get("after") == "ok", "attributes after rejected datagrams must survive")
		})

		t.Run("buffers and attributes are snapshotted before reuse", func(t *testing.T) {
			inter, err := flexfec.NewRepairReceiver(16, 16)
			c.Check(err == nil, "constructor error: %v", err)

			media := makeMediaSet(t, []uint16{30, 31}, mediaOptions{})
			upstream := &scriptedReader{}
			reader := inter.BindRemoteStream(streamInfo(), upstream)

			fec := buildRepairPacket(t, rankFecSSRC, rankFecPT, 7100, rankMediaSSRC, 30, []uint16{30, 31}, media)
			fecRaw := mustMarshal(t, fec)
			media31Raw := mustMarshal(t, media[31])

			// Reuse one buffer and one attributes map for every upstream read.
			sharedBuf := make([]byte, 1500)
			sharedAttrs := interceptor.Attributes{}
			sharedAttrs.Set("trace", "first")
			// Populate the cached RTP header the production chain would leave
			// behind on shared attributes.
			if _, err := sharedAttrs.GetRTPHeader(fecRaw); err != nil {
				t.Fatal(err)
			}

			upstream.items = append(upstream.items,
				scriptedDatagram{raw: fecRaw, attrs: sharedAttrs},
				scriptedDatagram{raw: media31Raw, attrs: sharedAttrs},
			)

			buf := make([]byte, 1500)
			_, _, err = reader.Read(buf, interceptor.Attributes{})
			c.Check(errors.Is(err, flexfec.ErrRepairPending), "repair datagram should be pending")

			// Mutate the caller-owned storage after the first Read returned.
			for i := range sharedBuf {
				sharedBuf[i] = 0xee
			}

			sharedAttrs.Set("trace", "second")

			packets, attrs, errs := drainRead(t, c, reader)
			c.Check(len(errs) == 0, "unexpected errors: %v", errs)
			c.Check(len(packets) == 2, "want recovered 30 and original 31, got %d", len(packets))
			c.Check(bytes.Equal(mustMarshal(t, packets[0]), mustMarshal(t, media[30])), "recovered bytes must ignore later buffer reuse")
			recoveredFlag, ok := attrs[0].Get(flexfec.AttributeKeyRecovered).(bool)
			c.Check(ok && recoveredFlag, "recovered packet attributes must set the recovery flag")
			c.Check(attrs[0].Get("trace") == "first", "recovered packet must keep the input attribute snapshot")

			for _, value := range attrs[0] {
				_, isHeader := value.(*rtp.Header)
				c.Check(!isHeader, "cached RTP header of input bytes must not leak onto recovered bytes")
			}
		})

		t.Run("upstream errors pass through while queued data is preserved", func(t *testing.T) {
			inter, err := flexfec.NewRepairReceiver(16, 16)
			c.Check(err == nil, "constructor error: %v", err)

			media := makeMediaSet(t, []uint16{40, 41}, mediaOptions{})
			upstream := &scriptedReader{}
			reader := inter.BindRemoteStream(streamInfo(), upstream)

			sentinel := errors.New("upstream exploded")
			upstream.items = append(upstream.items,
				datagram(t, buildRepairPacket(t, rankFecSSRC, rankFecPT, 7200, rankMediaSSRC, 40, []uint16{40, 41}, media), nil),
				datagram(t, media[41], nil),
				scriptedDatagram{err: sentinel},
			)

			buf := make([]byte, 1500)

			pending := 0
			if _, _, err := reader.Read(buf, interceptor.Attributes{}); errors.Is(err, flexfec.ErrRepairPending) {
				pending++
			}

			c.Check(pending == 1, "first read should be pending")

			first, _, err := reader.Read(buf, interceptor.Attributes{})
			c.Check(err == nil && first > 0, "recovered packet must be delivered before the error")

			second, _, err := reader.Read(buf, interceptor.Attributes{})
			c.Check(err == nil && second > 0, "original packet must also be delivered before the error")

			_, _, err = reader.Read(buf, interceptor.Attributes{})
			c.Check(errors.Is(err, sentinel), "upstream error must be returned verbatim, got %v", err)
		})

		t.Run("unbind closes the old reader and rebinding starts with fresh state", func(t *testing.T) {
			inter, err := flexfec.NewRepairReceiver(16, 16)
			c.Check(err == nil, "constructor error: %v", err)

			info := streamInfo()
			up1 := &scriptedReader{}
			reader1 := inter.BindRemoteStream(info, up1)

			media := makeMediaSet(t, []uint16{50, 51, 52}, mediaOptions{})
			up1.items = append(up1.items,
				datagram(t, buildRepairPacket(t, rankFecSSRC, rankFecPT, 7300, rankMediaSSRC, 50, []uint16{50, 51}, media), nil),
				datagram(t, media[52], nil),
			)
			_, _, _ = reader1.Read(make([]byte, 1500), interceptor.Attributes{})
			_, _, _ = reader1.Read(make([]byte, 1500), interceptor.Attributes{})

			inter.UnbindRemoteStream(info)
			readsBefore := up1.reads
			_, _, err = reader1.Read(make([]byte, 1500), interceptor.Attributes{})
			c.Check(errors.Is(err, io.ErrClosedPipe), "unbound reader must fail with ErrClosedPipe, got %v", err)
			c.Check(up1.reads == readsBefore, "unbound reader must not touch upstream anymore")

			up2 := &scriptedReader{}
			reader2 := inter.BindRemoteStream(info, up2)
			c.Check(reader2 != nil, "rebinding must succeed")

			// Only an opener: nothing from the old generation may carry over.
			up2.items = append(up2.items, datagram(t, media[52], nil))
			packets, _, errs := drainRead(t, c, reader2)
			c.Check(len(errs) == 0, "unexpected errors: %v", errs)
			c.Check(len(packets) == 1 && packets[0].SequenceNumber == 52, "rebound stream must inherit no old equations")

			// Fresh equations inside the new generation work normally.
			up2.items = append(up2.items,
				datagram(t, buildRepairPacket(t, rankFecSSRC, rankFecPT, 7301, rankMediaSSRC, 53, []uint16{53, 54},
					makeMediaSet(t, []uint16{53, 54}, mediaOptions{})), nil),
				datagram(t, makeMediaSet(t, []uint16{53, 54}, mediaOptions{})[54], nil),
			)
			packets, _, errs = drainRead(t, c, reader2)
			c.Check(len(errs) == 0, "unexpected errors: %v", errs)
			c.Check(len(packets) == 2 && packets[0].SequenceNumber == 53, "new generation recovery should work")
		})

		t.Run("close shuts down every binding", func(t *testing.T) {
			inter, err := flexfec.NewRepairReceiver(16, 16)
			c.Check(err == nil, "constructor error: %v", err)

			upstream := &scriptedReader{}
			reader := inter.BindRemoteStream(streamInfo(), upstream)
			c.Check(err == nil, "binding should succeed")

			c.Check(inter.Close() == nil, "close should not error")

			_, _, err = reader.Read(make([]byte, 1500), interceptor.Attributes{})
			c.Check(errors.Is(err, io.ErrClosedPipe), "existing reader must close")

			post := inter.BindRemoteStream(streamInfo(), &scriptedReader{})
			_, _, err = post.Read(make([]byte, 1500), interceptor.Attributes{})
			c.Check(errors.Is(err, io.ErrClosedPipe), "bindings after Close must be closed readers")
		})

		t.Run("pre window repair packets are bounded by the equation budget", func(t *testing.T) {
			// maxEquations=3: of five rows sent before the first media packet,
			// only the newest three survive.
			inter, err := flexfec.NewRepairReceiver(32, 3)
			c.Check(err == nil, "constructor error: %v", err)

			media := makeMediaSet(t, []uint16{10, 11, 12, 13, 14, 15, 16}, mediaOptions{})
			upstream := &scriptedReader{}
			reader := inter.BindRemoteStream(streamInfo(), upstream)

			pairs := [][2]uint16{{10, 11}, {11, 12}, {12, 13}, {13, 14}, {14, 15}}
			for i, pair := range pairs {
				upstream.items = append(upstream.items,
					datagram(t, buildRepairPacket(t, rankFecSSRC, rankFecPT, uint16(8000+i), rankMediaSSRC, pair[0],
						[]uint16{pair[0], pair[1]}, media), nil))
			}

			buf := make([]byte, 1500)
			for range 5 {
				_, _, err := reader.Read(buf, interceptor.Attributes{})
				c.Check(errors.Is(err, flexfec.ErrRepairPending), "pre-window rows stay pending, got %v", err)
			}

			// Open the window and deliver every original except seq 10: its
			// only covering row was the oldest, evicted before the base existed.
			upstream.items = append(upstream.items, datagram(t, media[16], nil))
			for _, seq := range []uint16{11, 12, 13, 14, 15} {
				upstream.items = append(upstream.items, datagram(t, media[seq], nil))
			}

			packets, _, errs := drainRead(t, c, reader)
			c.Check(len(errs) == 0, "unexpected errors: %v", errs)

			delivered := map[uint16]bool{}
			for _, packet := range packets {
				delivered[packet.SequenceNumber] = true
			}

			c.Check(!delivered[10], "seq 10 depended on an evicted pre-window row and must not be guessed")
			c.Check(delivered[11] && delivered[16], "surviving originals must still be delivered")
		})

		t.Run("inflexible and multi ssrc repair formats are rejected explicitly", func(t *testing.T) {
			inter, err := flexfec.NewRepairReceiver(16, 16)
			c.Check(err == nil, "constructor error: %v", err)

			upstream := &scriptedReader{}
			reader := inter.BindRemoteStream(streamInfo(), upstream)

			good := buildRepairPacket(t, rankFecSSRC, rankFecPT, 8100, rankMediaSSRC, 20, []uint16{20},
				makeMediaSet(t, []uint16{20}, mediaOptions{}))

			inflexible := *good
			inflexible.Payload = append([]byte(nil), good.Payload...)
			inflexible.Payload[0] |= 0x40 // F bit
			upstream.items = append(upstream.items, datagram(t, &inflexible, nil))
			_, _, err = reader.Read(make([]byte, 1500), interceptor.Attributes{})
			c.Check(err != nil, "F=1 inflexible generator matrix must be rejected")

			multiSSRC := *good
			multiSSRC.Payload = append([]byte(nil), good.Payload...)
			multiSSRC.Payload[8] = 2 // SSRCCount
			upstream.items = append(upstream.items, datagram(t, &multiSSRC, nil))
			_, _, err = reader.Read(make([]byte, 1500), interceptor.Attributes{})
			c.Check(err != nil, "multiple protected SSRCs must be rejected")
		})
	})
}
