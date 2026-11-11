package astits_test

import (
	"bytes"
	"context"
	astits "github.com/asticode/go-astits"
	"testing"
)

// Hand-built TS/PES: the fixture does not use the library muxer.
func seedTSPacket(cc byte, start bool, payload []byte) []byte {
	p := bytes.Repeat([]byte{0xff}, 188)
	p[0] = 0x47
	p[1] = 0x01
	if start {
		p[1] |= 0x40
	}
	p[2] = 0x00
	p[3] = 0x10 | (cc & 15)
	off := 4
	if len(payload) < 184 {
		p[3] |= 0x20
		n := 183 - len(payload)
		p[4] = byte(n)
		if n > 0 {
			p[5] = 0
		}
		off = 5 + n
	}
	copy(p[off:], payload)
	return p
}
func seedTS(duplicate bool) ([]byte, []byte) {
	data := make([]byte, 400)
	for i := range data {
		data[i] = byte(i*7 + 11)
	}
	pes := []byte{0, 0, 1, 0xe0, 1, 0x93, 0x80, 0, 0} // 403 bytes after length
	pes = append(pes, data...)
	var out []byte
	for i, off := 0, 0; off < len(pes); i, off = i+1, off+184 {
		end := off + 184
		if end > len(pes) {
			end = len(pes)
		}
		p := seedTSPacket(byte(14+i), off == 0, pes[off:end])
		out = append(out, p...)
		if duplicate && i == 1 {
			out = append(out, p...)
		}
	}
	return out, data
}
func seedPES(t *testing.T, raw []byte) [][]byte {
	t.Helper()
	d := astits.NewDemuxer(context.Background(), bytes.NewReader(raw), astits.DemuxerOptPacketSize(188))
	var got [][]byte
	for {
		v, err := d.NextData()
		if err == astits.ErrNoMorePackets {
			break
		}
		if err != nil {
			t.Fatal(err)
		}
		if v.PES != nil {
			got = append(got, append([]byte(nil), v.PES.Data...))
		}
	}
	return got
}
func TestSeedContinuityClean(t *testing.T) {
	raw, want := seedTS(false)
	got := seedPES(t, raw)
	if len(got) != 1 || !bytes.Equal(got[0], want) {
		t.Fatalf("clean PES: count=%d, want one 400-byte payload", len(got))
	}
	t.Log("clean: one 400-byte PES, CC 14/15/0")
}
func TestSeedContinuityDuplicate(t *testing.T) {
	raw, want := seedTS(true)
	got := seedPES(t, raw)
	if len(got) != 1 || !bytes.Equal(got[0], want) {
		t.Fatalf("middle TS packet repeated: count=%d, want one unchanged 400-byte PES", len(got))
	}
}

// --- Transport continuity regressions ----------------------------------------

// tcPacket builds a TS packet carrying a payload, stuffing with an adaptation
// field when the payload is shorter than 184 bytes.
func tcPacket(pid uint16, cc byte, pusi bool, payload []byte) []byte {
	return tcPacketAF(pid, cc, pusi, nil, payload)
}

// tcPacketPCR builds a TS packet carrying a payload and a PCR in its
// adaptation field.
func tcPacketPCR(pid uint16, cc byte, pusi bool, payload []byte) []byte {
	return tcPacketAF(pid, cc, pusi, append([]byte{0x10}, tcPCR(90000)...), payload)
}

// tcAdaptationOnly builds a TS packet made of an adaptation field only. Such
// packets must not advance the payload continuity counter.
func tcAdaptationOnly(pid uint16, cc byte, discontinuity, pcr bool) []byte {
	af := []byte{0}
	if discontinuity {
		af[0] |= 0x80
	}
	if pcr {
		af[0] |= 0x10
		af = append(af, tcPCR(27000000)...)
	}
	return tcPacketAF(pid, cc, false, af, nil)
}

// tcPCR encodes a PCR made of a 33-bit base and a null extension.
func tcPCR(base uint64) []byte {
	return []byte{
		byte(base >> 25), byte(base >> 17), byte(base >> 9), byte(base >> 1),
		byte(base)<<7 | 0x7e, 0,
	}
}

// tcPacketAF builds a 188-byte TS packet with an optional adaptation field
// content (flags byte included) and an optional payload, stuffing the rest.
func tcPacketAF(pid uint16, cc byte, pusi bool, af, payload []byte) []byte {
	p := bytes.Repeat([]byte{0xff}, 188)
	p[0] = 0x47
	p[1] = byte(pid >> 8)
	if pusi {
		p[1] |= 0x40
	}
	p[2] = byte(pid)
	off := 4
	if stuffing := 184 - len(af) - len(payload); stuffing < 0 {
		panic("packet content too big")
	} else if stuffing == 0 && len(af) == 0 {
		p[3] = 0x10 | (cc & 15)
	} else {
		p[3] = 0x20 | (cc & 15)
		if len(payload) > 0 {
			p[3] |= 0x10
		}
		// Adaptation field length covers flags, content and stuffing
		p[4] = byte(183 - len(payload))
		p[5] = 0 // flags
		copy(p[5:], af)
		off = 5 + int(p[4])
	}
	copy(p[off:], payload)
	return p
}

// tcPES builds a PES packet wrapping n data bytes.
func tcPES(seed, n int) (pes, data []byte) {
	data = make([]byte, n)
	for i := range data {
		data[i] = byte(seed + i*7 + 11)
	}
	pes = []byte{0, 0, 1, 0xe0, byte((3 + n) >> 8), byte(3 + n), 0x80, 0, 0}
	return append(pes, data...), data
}

// tcPayloads reads a stream and returns the PES payloads delivered by NextData.
func tcPayloads(t *testing.T, raw []byte) [][]byte {
	t.Helper()
	d := astits.NewDemuxer(context.Background(), bytes.NewReader(raw), astits.DemuxerOptPacketSize(188))
	var got [][]byte
	for {
		v, err := d.NextData()
		if err == astits.ErrNoMorePackets {
			break
		}
		if err != nil {
			t.Fatal(err)
		}
		if v.PES != nil {
			got = append(got, append([]byte(nil), v.PES.Data...))
		}
	}
	return got
}

// A PES that completes within a single TS packet must be delivered exactly
// once, even if the packet is retransmitted afterwards, and even if the
// retransmission carries a refreshed PCR. NextPacket still sees the raw
// sequence, duplicates included.
func TestTransportContinuityCompletePESRetransmission(t *testing.T) {
	pes1, data1 := tcPES(0, 100)
	pes2, data2 := tcPES(3, 50)
	p1 := tcPacket(0x100, 7, true, pes1)
	raw := append([]byte(nil), p1...)
	raw = append(raw, p1...)                                // exact retransmission
	raw = append(raw, tcPacketPCR(0x100, 7, true, pes1)...) // retransmission with updated PCR
	raw = append(raw, tcPacket(0x100, 8, true, pes2)...)

	// NextPacket preserves the raw sequence
	d := astits.NewDemuxer(context.Background(), bytes.NewReader(raw), astits.DemuxerOptPacketSize(188))
	var n int
	for {
		if _, err := d.NextPacket(); err == astits.ErrNoMorePackets {
			break
		} else if err != nil {
			t.Fatal(err)
		}
		n++
	}
	if n != 4 {
		t.Fatalf("NextPacket: got %d packets, want 4", n)
	}

	got := tcPayloads(t, raw)
	if len(got) != 2 || !bytes.Equal(got[0], data1) || !bytes.Equal(got[1], data2) {
		t.Fatalf("retransmitted single-packet PES: got %d payloads, want the 100-byte and 50-byte ones", len(got))
	}
}

// A packet repeating the previous continuity counter with a different payload
// voids the PES being assembled on this PID: delivery resumes at the next
// payload start, and other PIDs are unaffected.
func TestTransportContinuityConflictRecovery(t *testing.T) {
	pesX, _ := tcPES(0, 400)
	pesB, dataB := tcPES(1, 50)
	pesY, dataY := tcPES(2, 60)
	pesZ, _ := tcPES(9, 30)
	var raw []byte
	raw = append(raw, tcPacket(0x100, 0, true, pesX[:184])...)
	raw = append(raw, tcPacket(0x100, 1, false, pesX[184:368])...)
	// Same counter as the previous packet on this PID, different payload:
	// conflict. The payload even looks like a valid PES, it must still be
	// dropped since it is not a payload start.
	raw = append(raw, tcPacket(0x100, 1, false, pesZ)...)
	// Non-start packet while recovering: dropped
	raw = append(raw, tcPacket(0x100, 2, false, pesX[368:])...)
	// Another PID carries a complete PES in the meantime
	raw = append(raw, tcPacket(0x101, 9, true, pesB)...)
	// Recovery at the next payload start
	raw = append(raw, tcPacket(0x100, 3, true, pesY)...)

	got := tcPayloads(t, raw)
	if len(got) != 2 || !bytes.Equal(got[0], dataB) || !bytes.Equal(got[1], dataY) {
		t.Fatalf("conflict recovery: got %d payloads, want the 50-byte PES of PID 0x101 and the 60-byte recovery PES", len(got))
	}
}

// Adaptation-field-only packets don't advance the payload continuity counter.
func TestTransportContinuityAdaptationOnly(t *testing.T) {
	pes, data := tcPES(0, 400)
	var raw []byte
	raw = append(raw, tcPacket(0x100, 14, true, pes[:184])...)
	raw = append(raw, tcAdaptationOnly(0x100, 15, false, true)...) // PCR only, no payload
	raw = append(raw, tcPacket(0x100, 15, false, pes[184:368])...)
	raw = append(raw, tcAdaptationOnly(0x100, 0, false, false)...)
	raw = append(raw, tcPacket(0x100, 0, false, pes[368:])...)

	got := tcPayloads(t, raw)
	if len(got) != 1 || !bytes.Equal(got[0], data) {
		t.Fatalf("adaptation-only packets: got %d payloads, want one 400-byte PES", len(got))
	}
}

// An explicit discontinuity indicator opens a new continuity interval: the
// partial PES is voided and a non-start payload can't be appended to it.
func TestTransportContinuityDiscontinuityIndicator(t *testing.T) {
	pesX, _ := tcPES(0, 400)
	pesY, dataY := tcPES(2, 60)
	var raw []byte
	raw = append(raw, tcPacket(0x100, 0, true, pesX[:184])...)
	raw = append(raw, tcPacket(0x100, 1, false, pesX[184:368])...)
	// New continuity interval: the partial PES above is voided
	raw = append(raw, tcAdaptationOnly(0x100, 2, true, false)...)
	// Non-start payload, continuous with the voided interval: it must not
	// complete the voided PES
	raw = append(raw, tcPacket(0x100, 2, false, bytes.Repeat([]byte{0xab}, 41))...)
	// Fresh start in the new interval
	raw = append(raw, tcPacket(0x100, 3, true, pesY)...)

	got := tcPayloads(t, raw)
	if len(got) != 1 || !bytes.Equal(got[0], dataY) {
		t.Fatalf("discontinuity indicator: got %d payloads, want only the 60-byte PES", len(got))
	}
}

// Interleaved PIDs keep independent continuity state: identical counters on
// different PIDs are not retransmissions, and counter wrap-around is fine.
func TestTransportContinuityInterleavedPIDs(t *testing.T) {
	pesA, dataA := tcPES(0, 400)
	pesB, dataB := tcPES(1, 300)
	var raw []byte
	raw = append(raw, tcPacket(0x100, 14, true, pesA[:184])...)
	raw = append(raw, tcPacket(0x101, 14, true, pesB[:184])...)
	raw = append(raw, tcPacket(0x100, 15, false, pesA[184:368])...)
	raw = append(raw, tcPacket(0x101, 15, false, pesB[184:])...)
	raw = append(raw, tcPacket(0x100, 0, false, pesA[368:])...)

	got := tcPayloads(t, raw)
	if len(got) != 2 || !bytes.Equal(got[0], dataB) || !bytes.Equal(got[1], dataA) {
		t.Fatalf("interleaved PIDs: got %d payloads, want the 300-byte and 400-byte PES", len(got))
	}
}
