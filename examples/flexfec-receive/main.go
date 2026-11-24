// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

// Command flexfec-receive replays an interleaved stream of media and FlexFEC-03
// repair datagrams through the public RepairReceiverInterceptor entry point.
//
// It demonstrates joint multi-equation recovery: three media packets A, B and C
// are all missing, while the repairs protecting A+B, B+C and A+B+C arrive. The
// rank-complete GF(2) system recovers all three instead of stalling after a
// first single-unknown peeling pass. The replay is fully in memory so it can be
// run offline.
package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"log"
	"os"

	"github.com/pion/interceptor"
	"github.com/pion/interceptor/pkg/flexfec"
	"github.com/pion/rtp"
)

const (
	mediaSSRC  uint32 = 40001
	repairSSRC uint32 = 40002
	mediaPT    uint8  = 96
	repairPT   uint8  = 97
)

// replaySource is an in-memory interceptor.RTPReader. A real application would
// back it with the same RTP socket that receives both SSRCs.
type replaySource struct {
	packets [][]byte
}

func (r *replaySource) Read(b []byte, a interceptor.Attributes) (int, interceptor.Attributes, error) {
	if len(r.packets) == 0 {
		return 0, a, io.EOF
	}
	pkt := r.packets[0]
	r.packets = r.packets[1:]

	return copy(b, pkt), a, nil
}

func mediaPacket(seq uint16, payload []byte) []byte {
	pkt := rtp.Packet{
		Header: rtp.Header{
			Version:        2,
			Marker:         true,
			PayloadType:    mediaPT,
			SequenceNumber: seq,
			Timestamp:      90000 + uint32(seq)*900,
			SSRC:           mediaSSRC,
		},
		Payload: payload,
	}
	raw, err := pkt.Marshal()
	if err != nil {
		log.Fatalf("marshal media: %v", err)
	}

	return raw
}

// recoveryVector builds the same 8 byte header word + post-byte-12 body that
// FlexFEC-03 XORs for one media packet.
func recoveryVector(raw []byte) []byte {
	bodyLen := len(raw) - 12
	vec := make([]byte, 8+bodyLen)
	vec[0] = raw[0]
	vec[1] = raw[1]
	binary.BigEndian.PutUint16(vec[2:4], uint16(bodyLen))
	copy(vec[4:8], raw[4:8])
	copy(vec[8:], raw[12:])

	return vec
}

// repairPacket hand-constructs (without the production encoder) one FlexFEC-03
// repair packet protecting the supplied packets at base+offsets.
func repairPacket(base uint16, offsets []int, protected [][]byte, fecSeq uint16) []byte {
	head := make([]byte, 8)
	maxBody := 0
	vecs := make([][]byte, len(protected))
	for i, raw := range protected {
		v := recoveryVector(raw)
		vecs[i] = v
		for j := 0; j < 8; j++ {
			head[j] ^= v[j]
		}
		if len(v)-8 > maxBody {
			maxBody = len(v) - 8
		}
	}
	head[0] &^= 0xC0 // R=0, F=0

	body := make([]byte, maxBody)
	for _, v := range vecs {
		for j := 8; j < len(v); j++ {
			body[j-8] ^= v[j]
		}
	}

	var mask uint16 = 0x8000 // k=1, single 20 byte FlexFEC header
	for _, off := range offsets {
		mask |= 1 << (14 - off)
	}

	fecPayload := make([]byte, 20+maxBody)
	copy(fecPayload[0:8], head)
	fecPayload[8] = 1 // single protected SSRC count
	binary.BigEndian.PutUint32(fecPayload[12:16], mediaSSRC)
	binary.BigEndian.PutUint16(fecPayload[16:18], base)
	binary.BigEndian.PutUint16(fecPayload[18:20], mask)
	copy(fecPayload[20:], body)

	pkt := rtp.Packet{
		Header: rtp.Header{
			Version:        2,
			PayloadType:    repairPT,
			SequenceNumber: fecSeq,
			SSRC:           repairSSRC,
		},
		Payload: fecPayload,
	}
	raw, err := pkt.Marshal()
	if err != nil {
		log.Fatalf("marshal repair: %v", err)
	}

	return raw
}

func main() {
	// Window context: 100 establishes the base, 104 advances the right edge so
	// the solutions for 101..103 are eligible for delivery. A, B and C
	// themselves never arrive as original packets.
	base := mediaPacket(100, []byte("base media"))
	a := mediaPacket(101, []byte("A original media bytes"))
	b := mediaPacket(102, []byte("B"))
	c := mediaPacket(103, make([]byte, 40))
	edge := mediaPacket(104, []byte("edge media"))

	rAB := repairPacket(101, []int{0, 1}, [][]byte{a, b}, 1)
	rBC := repairPacket(102, []int{0, 1}, [][]byte{b, c}, 2)
	rABC := repairPacket(101, []int{0, 1, 2}, [][]byte{a, b, c}, 3)

	source := &replaySource{packets: [][]byte{base, edge, rAB, rBC, rABC}}

	receiver, err := flexfec.NewRepairReceiver(64, 64)
	if err != nil {
		log.Fatalf("NewRepairReceiver: %v", err)
	}
	reader := receiver.BindRemoteStream(&interceptor.StreamInfo{
		SSRC:                              mediaSSRC,
		PayloadType:                       mediaPT,
		SSRCForwardErrorCorrection:        repairSSRC,
		PayloadTypeForwardErrorCorrection: repairPT,
	}, source)

	fmt.Println("FlexFEC receive replay: delivering original and recovered media")
	buf := make([]byte, 1500)
	delivered := 0
	recovered := 0
	for calls := 0; calls < 1000; calls++ {
		n, attrs, err := reader.Read(buf, interceptor.Attributes{})
		if errors.Is(err, flexfec.ErrRepairPending) {
			continue
		}
		if errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			log.Fatalf("Read: %v", err)
		}
		if n == 0 {
			continue
		}

		var pkt rtp.Packet
		if err := pkt.Unmarshal(buf[:n]); err != nil {
			log.Fatalf("unmarshal output: %v", err)
		}
		isRecovered := attrs != nil && attrs.Get(flexfec.AttributeRecovered) == true
		if isRecovered {
			recovered++
		}
		delivered++
		fmt.Printf("  seq=%5d ssrc=%d pt=%d recovered=%-5v payload=%q\n",
			pkt.SequenceNumber, pkt.SSRC, pkt.PayloadType, isRecovered, pkt.Payload)
	}

	if delivered != 5 || recovered != 3 {
		fmt.Fprintf(os.Stderr, "unexpected replay result: delivered=%d recovered=%d\n", delivered, recovered)
		os.Exit(1)
	}
	fmt.Printf("OK: delivered=%d media packets, %d jointly recovered (A,B,C)\n", delivered, recovered)
}
