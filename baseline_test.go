package rtp_test

import (
	"bytes"
	"github.com/pion/rtp"
	"github.com/pion/rtp/codecs"
	"testing"
)

func TestBaseline(t *testing.T) {
	input := []byte{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}
	p := rtp.NewPacketizerWithOptions(16, &codecs.G711Payloader{}, rtp.NewFixedSequencer(65535), 8000, rtp.WithTimestamp(0xfffffffe), rtp.WithSSRC(7), rtp.WithPayloadType(8))
	packets := p.Packetize(input, 10)
	if len(packets) != 3 {
		t.Fatal(len(packets))
	}
	var all []byte
	for i, x := range packets {
		if x.Timestamp != 0xfffffffe || x.SequenceNumber != uint16(65535+i) || x.Marker != (i == 2) {
			t.Fatal(x.Header)
		}
		wire, e := x.Marshal()
		if e != nil {
			t.Fatal(e)
		}
		var back rtp.Packet
		if e = back.Unmarshal(wire); e != nil {
			t.Fatal(e)
		}
		all = append(all, back.Payload...)
	}
	if !bytes.Equal(all, input) {
		t.Fatal(all)
	}
	input[0] = 99
	if packets[0].Payload[0] != 0 {
		t.Fatal("source aliased")
	}
	next := p.Packetize([]byte{1}, 1)
	if next[0].Timestamp != 8 {
		t.Fatal(next[0].Timestamp)
	}
	t.Log("PASS: legacy G711 fragmentation, wrap, wire roundtrip and copied payload; common timestamp is existing behavior")
}
