// SPDX-License-Identifier: MIT
package jitterbuffer

import (
	"errors"
	"testing"

	"github.com/pion/interceptor"
	"github.com/pion/rtp"
)

func TestBaselinePacketFlow(t *testing.T) {
	jb := New(WithMinimumPacketCount(1))
	jb.Push(&rtp.Packet{Header: rtp.Header{SequenceNumber: 65535, Timestamp: 4294967200}, Payload: []byte{10}})
	jb.Push(&rtp.Packet{Header: rtp.Header{SequenceNumber: 1, Timestamp: 900}, Payload: []byte{20}})
	p, err := jb.Pop()
	if err != nil || p.SequenceNumber != 65535 || p.Payload[0] != 10 {
		t.Fatalf("first=%v err=%v", p, err)
	}
	p, err = jb.Pop()
	if p != nil || !errors.Is(err, ErrNotFound) || jb.PlayoutHead() != 0 {
		t.Fatalf("gap=%v err=%v head=%d", p, err, jb.PlayoutHead())
	}
	jb.SetPlayoutHead(1)
	p, err = jb.Pop()
	if err != nil || p.SequenceNumber != 1 || p.Payload[0] != 20 {
		t.Fatalf("manual=%v err=%v", p, err)
	}
	jb.Clear(true)
	if _, err = jb.PeekAtSequence(1); err == nil {
		t.Fatal("clear retained packet")
	}

	factory, err := NewInterceptor()
	if err != nil {
		t.Fatal(err)
	}
	inst, err := factory.NewInterceptor("baseline")
	if err != nil {
		t.Fatal(err)
	}
	defer inst.Close()
	next := uint16(100)
	upstream := interceptor.RTPReaderFunc(func(b []byte, a interceptor.Attributes) (int, interceptor.Attributes, error) {
		pkt := &rtp.Packet{Header: rtp.Header{Version: 2, SSRC: 77, SequenceNumber: next, Timestamp: uint32(next) * 900}, Payload: []byte{byte(next)}}
		next++
		n, e := pkt.MarshalTo(b)
		return n, a, e
	})
	info := &interceptor.StreamInfo{SSRC: 77, ClockRate: 90000}
	reader := inst.BindRemoteStream(info, upstream)
	raw := make([]byte, 13)
	for k := 0; k < 49; k++ {
		_, _, err = reader.Read(raw, nil)
		if !errors.Is(err, ErrPopWhileBuffering) {
			t.Fatalf("read %d: %v", k, err)
		}
	}
	n, _, err := reader.Read(raw, nil)
	if err != nil {
		t.Fatal(err)
	}
	var out rtp.Packet
	if err = out.Unmarshal(raw[:n]); err != nil {
		t.Fatal(err)
	}
	if out.SequenceNumber != 100 || out.SSRC != 77 || len(out.Payload) != 1 || out.Payload[0] != 100 {
		t.Fatalf("output=%+v", out)
	}
	inst.UnbindRemoteStream(info)
	t.Log("CHECK_BASELINE_JITTER wrap=65535,1 gap_head=0 manual_resume=1 receiver_count=50 output=100")
}
