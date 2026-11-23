// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

// Package main demonstrates the receive-side FlexFEC-03 interceptor. It
// replays an in-memory RTP stream where two media packets are lost and shows
// the repair receiver reconstructing them before a jitter buffer would see
// the stream.
package main

import (
	"errors"
	"fmt"
	"log"

	"github.com/pion/interceptor"
	"github.com/pion/interceptor/pkg/flexfec"
	"github.com/pion/rtp"
)

const (
	mediaSSRC = 5000
	fecSSRC   = 5001
	mediaPT   = 96
	fecPT     = 127
)

// datagram is one wire RTP packet plus the attributes travelling with it.
type datagram struct {
	packet *rtp.Packet
	attrs  interceptor.Attributes
}

// errReplayDone marks the end of the deterministic in-memory replay.
var errReplayDone = errors.New("replay finished")

// replayReader is a deterministic in-memory RTPReader used instead of a socket.
type replayReader struct {
	datagrams []datagram
	index     int
}

func (r *replayReader) Read(buf []byte, attributes interceptor.Attributes) (int, interceptor.Attributes, error) {
	if r.index >= len(r.datagrams) {
		return 0, nil, errReplayDone
	}

	item := r.datagrams[r.index]
	r.index++

	raw, err := item.packet.Marshal()
	if err != nil {
		return 0, nil, err
	}

	copy(buf, raw)

	return len(raw), item.attrs, nil
}

func mediaPacket(seq uint16) *rtp.Packet {
	return &rtp.Packet{
		Header: rtp.Header{
			Version:        2,
			Marker:         true,
			PayloadType:    mediaPT,
			SequenceNumber: seq,
			Timestamp:      90000 + uint32(seq)*900,
			SSRC:           mediaSSRC,
		},
		Payload: []byte{0x61, 0x62, 0x63, byte(seq >> 8), byte(seq)},
	}
}

func main() {
	// Window of 32 extended sequence numbers, at most 32 retained equations.
	repair, err := flexfec.NewRepairReceiver(32, 32)
	if err != nil {
		log.Fatalf("new repair receiver: %v", err)
	}
	defer func() { _ = repair.Close() }()

	info := &interceptor.StreamInfo{
		SSRC:                              mediaSSRC,
		PayloadType:                       mediaPT,
		SSRCForwardErrorCorrection:        fecSSRC,
		PayloadTypeForwardErrorCorrection: fecPT,
	}

	// Five consecutive media packets; repair is generated with the production
	// FlexFEC-03 encoder, two repair packets per five media packets.
	media := make([]rtp.Packet, 5)
	for i := range media {
		media[i] = *mediaPacket(uint16(100 + i))
	}

	encoder := flexfec.NewFlexEncoder03(fecPT, fecSSRC)
	fecPackets := encoder.EncodeFec(media, 2)
	if len(fecPackets) != 2 {
		log.Fatalf("expected 2 repair packets, got %d", len(fecPackets))
	}

	// Simulate loss of media sequence numbers 101 and 104. The encoder's
	// interleaved coverage puts each loss in a different repair row, so both
	// can be reconstructed once the other members arrive.
	lost := map[uint16]bool{101: true, 104: true}
	upstream := &replayReader{}

	for _, packet := range media {
		if !lost[packet.SequenceNumber] {
			upstream.datagrams = append(upstream.datagrams, datagram{
				packet: &packet,
				attrs:  interceptor.Attributes{"trace": "media"},
			})
		}
	}

	for i := range fecPackets {
		upstream.datagrams = append(upstream.datagrams, datagram{
			packet: &fecPackets[i],
			attrs:  interceptor.Attributes{"trace": "repair"},
		})
	}

	// A later media packet moves the frontier past the held solution for 104,
	// which is then released to the delivery queue in sequence order.
	opener := mediaPacket(105)
	upstream.datagrams = append(upstream.datagrams, datagram{
		packet: opener,
		attrs:  interceptor.Attributes{"trace": "media"},
	})

	// The repair receiver is the first inbound stage; whatever follows (for
	// example a jitter buffer) only ever sees media RTP, original or rebuilt.
	reader := repair.BindRemoteStream(info, upstream)

	fmt.Println("inbound media delivered to the next stage (e.g. jitter buffer):")

	buf := make([]byte, 1500)

	originals, recovered := 0, 0

	for {
		n, attrs, err := reader.Read(buf, interceptor.Attributes{})
		if err != nil {
			if errors.Is(err, flexfec.ErrRepairPending) {
				continue
			}

			if errors.Is(err, errReplayDone) {
				break
			}

			log.Fatalf("read failed: %v", err)
		}

		packet := &rtp.Packet{}
		if err := packet.Unmarshal(append([]byte(nil), buf[:n]...)); err != nil {
			log.Fatalf("unmarshal delivered packet: %v", err)
		}

		wasRecovered, _ := attrs.Get(flexfec.AttributeKeyRecovered).(bool)
		origin := "original"

		if wasRecovered {
			origin = "recovered"
			recovered++
		} else {
			originals++
		}

		fmt.Printf("  seq=%-4d ssrc=%d pt=%-3d %-9s attrs=%v payload=% x\n",
			packet.SequenceNumber, packet.SSRC, packet.PayloadType, origin, attrs, packet.Payload)
	}

	fmt.Printf("replay complete: %d original and %d recovered media packets, 0 repair packets exposed downstream\n",
		originals, recovered)
}
