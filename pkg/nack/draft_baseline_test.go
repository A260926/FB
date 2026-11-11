//go:build draftbaseline

// Z1152 starting-state witnesses. Not part of the default regression suite.
package nack

import (
	"errors"
	"reflect"
	"testing"
	"time"

	"github.com/pion/interceptor"
	"github.com/pion/rtcp"
	"github.com/pion/rtp"
)

func draftGenerator(t *testing.T) *GeneratorInterceptor {
	t.Helper()
	f, err := NewGeneratorInterceptor(GeneratorSize(64), GeneratorMaxNacksPerPacket(1), GeneratorInterval(time.Millisecond))
	if err != nil {
		t.Fatal(err)
	}
	i, err := f.NewInterceptor("draft")
	if err != nil {
		t.Fatal(err)
	}
	n := i.(*GeneratorInterceptor)
	t.Cleanup(func() {
		if err := n.Close(); err != nil {
			t.Error(err)
		}
	})
	return n
}

func draftInput(t *testing.T, n *GeneratorInterceptor) func(...uint16) {
	t.Helper()
	info := &interceptor.StreamInfo{SSRC: 7, RTCPFeedback: []interceptor.RTCPFeedback{{Type: "nack"}}}
	var seq uint16
	reader := n.BindRemoteStream(info, interceptor.RTPReaderFunc(func(b []byte, a interceptor.Attributes) (int, interceptor.Attributes, error) {
		p := rtp.Packet{Header: rtp.Header{Version: 2, SSRC: 7, SequenceNumber: seq}, Payload: []byte{1}}
		raw, err := p.Marshal()
		if err != nil {
			return 0, nil, err
		}
		return copy(b, raw), a, nil
	}))
	return func(seqs ...uint16) {
		t.Helper()
		for _, next := range seqs {
			seq = next
			b := make([]byte, 1500)
			if _, _, err := reader.Read(b, nil); err != nil {
				t.Fatal(err)
			}
		}
	}
}

func TestDraftNormalAndWindowAlias(t *testing.T) {
	n := draftGenerator(t)
	feed := draftInput(t, n)
	feed(100, 102)
	rl := n.receiveLogs[7]
	get := func() []uint16 { return append([]uint16{}, rl.missingSeqNumbers(0, make([]uint16, 64))...) }
	if got := get(); !reflect.DeepEqual(got, []uint16{101}) {
		t.Fatalf("gap=%v", got)
	}
	feed(101)
	if got := get(); len(got) != 0 {
		t.Fatalf("repair=%v", got)
	}
	t.Log("NORMAL gap=101 repaired=true")
	feed(166)
	before := get()
	if len(before) != 63 || before[0] != 103 || before[62] != 165 {
		t.Fatalf("before=%v", before)
	}
	feed(101) // outside [103,166], but aliases the slot for current missing packet 165
	after := get()
	if len(after) != 62 || after[len(after)-1] != 164 {
		t.Fatalf("starting behavior changed: %v", after)
	}
	t.Log("DEFECT stale=101 aliases=165 missing_before=63 missing_after=62")
}

func TestDraftFailedWriteSpendsBudget(t *testing.T) {
	n := draftGenerator(t)
	feed := draftInput(t, n)
	feed(100, 102)
	entered := make(chan []uint16, 1)
	release := make(chan struct{})
	n.BindRTCPWriter(interceptor.RTCPWriterFunc(func(pkts []rtcp.Packet, _ interceptor.Attributes) (int, error) {
		seqs := []uint16{}
		for _, p := range pkts {
			for _, pair := range p.(*rtcp.TransportLayerNack).Nacks {
				pair.Range(func(s uint16) bool { seqs = append(seqs, s); return true })
			}
		}
		entered <- seqs
		<-release
		return 0, errors.New("draft local writer failure")
	}))
	defer close(release)
	select {
	case seqs := <-entered:
		if !reflect.DeepEqual(seqs, []uint16{101}) {
			t.Fatalf("nack=%v", seqs)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("writer did not run")
	}
	// No successful Write has occurred: the writer is still blocked.
	n.receiveLogsMu.Lock()
	count := n.nackCountLogs[7][101]
	missing := append([]uint16{}, n.receiveLogs[7].missingSeqNumbers(0, make([]uint16, 64))...)
	n.receiveLogsMu.Unlock()
	if count != 1 || !reflect.DeepEqual(missing, []uint16{101}) {
		t.Fatalf("count=%d missing=%v", count, missing)
	}
	t.Log("DEFECT successful_writes=0 nack_budget_used=1 missing=101")
}
