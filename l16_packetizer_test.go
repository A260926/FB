// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package rtp_test

import (
	"bytes"
	"testing"

	"github.com/pion/rtp"
)

// l16PCM encodes channel-interleaved signed 16-bit samples in network byte
// order, one sample frame per inner slice.
func l16PCM(frames ...[]int16) []byte {
	var out []byte
	for _, frame := range frames {
		for _, sample := range frame {
			out = append(out, byte(uint16(sample)>>8), byte(uint16(sample))) // nolint: gosec // G115
		}
	}

	return out
}

// l16RoundTrip marshals every packet, unmarshals it back, and returns the
// decoded packets plus the wire sizes.
func l16RoundTrip(t *testing.T, packets []*rtp.Packet) ([]rtp.Packet, []int) {
	t.Helper()

	decoded := make([]rtp.Packet, len(packets))
	wireSizes := make([]int, len(packets))
	for i, pkt := range packets {
		wire, err := pkt.Marshal()
		if err != nil {
			t.Fatalf("Marshal packet %d: %v", i, err)
		}
		wireSizes[i] = len(wire)
		if err = decoded[i].Unmarshal(wire); err != nil {
			t.Fatalf("Unmarshal packet %d: %v", i, err)
		}
	}

	return decoded, wireSizes
}

func TestL16PacketizerStereoWrapExample(t *testing.T) {
	// Stereo, packet mtu 22: 10 bytes of payload budget per packet, i.e. two
	// 4-byte sample frames per packet.
	p, err := rtp.NewL16Packetizer(22, 2, 96, 7, 4294967294, rtp.NewFixedSequencer(65534))
	if err != nil {
		t.Fatal(err)
	}

	input := l16PCM([]int16{1, -1}, []int16{2, -2}, []int16{3, -3}, []int16{4, -4}, []int16{5, -5})
	packets, err := p.Packetize(input)
	if err != nil {
		t.Fatal(err)
	}
	if len(packets) != 3 {
		t.Fatalf("got %d packets, want 3", len(packets))
	}

	wantPayloadLens := []int{8, 8, 4}
	wantTimestamps := []uint32{4294967294, 0, 2} // one tick per sample frame, wraps at 2^32
	wantSeqs := []uint16{65534, 65535, 0}
	wantMarkers := []bool{true, false, false}
	for i, pkt := range packets {
		if len(pkt.Payload) != wantPayloadLens[i] {
			t.Fatalf("packet %d payload length = %d, want %d", i, len(pkt.Payload), wantPayloadLens[i])
		}
		if pkt.Timestamp != wantTimestamps[i] {
			t.Fatalf("packet %d timestamp = %d, want %d", i, pkt.Timestamp, wantTimestamps[i])
		}
		if pkt.SequenceNumber != wantSeqs[i] {
			t.Fatalf("packet %d sequence = %d, want %d", i, pkt.SequenceNumber, wantSeqs[i])
		}
		if pkt.Marker != wantMarkers[i] {
			t.Fatalf("packet %d marker = %v, want %v", i, pkt.Marker, wantMarkers[i])
		}
	}
	if first := packets[0].Payload; !bytes.Equal(first, []byte{0x00, 0x01, 0xff, 0xff, 0x00, 0x02, 0xff, 0xfe}) {
		t.Fatalf("first payload = % x, want 00 01 ff ff 00 02 ff fe", first)
	}

	decoded, wireSizes := l16RoundTrip(t, packets)
	for i := range decoded {
		if want := 12 + wantPayloadLens[i]; wireSizes[i] != want {
			t.Fatalf("packet %d wire size = %d, want %d (fixed header + payload, no padding/extensions)", i, wireSizes[i], want)
		}
		hdr := decoded[i].Header
		if hdr.Version != 2 || hdr.Padding || hdr.Extension || hdr.CSRC != nil || hdr.PayloadType != 96 || hdr.SSRC != 7 {
			t.Fatalf("packet %d decoded header = %+v", i, hdr)
		}
		if hdr.Marker != wantMarkers[i] || hdr.Timestamp != wantTimestamps[i] || hdr.SequenceNumber != wantSeqs[i] {
			t.Fatalf("packet %d decoded header = %+v", i, hdr)
		}
	}
	var reassembled []byte
	for i := range decoded {
		reassembled = append(reassembled, decoded[i].Payload...)
	}
	if !bytes.Equal(reassembled, input) {
		t.Fatalf("reassembled = % x, want % x", reassembled, input)
	}

	// Returned payloads own their bytes.
	input[0] = 0xee
	if packets[0].Payload[0] != 0x00 {
		t.Fatal("payload aliases the caller's input buffer")
	}

	// Skipping silence advances only the clock and re-arms the Marker.
	p.SkipFrames(7)
	next, err := p.Packetize(l16PCM([]int16{6, -6}))
	if err != nil {
		t.Fatal(err)
	}
	if len(next) != 1 {
		t.Fatalf("got %d packets, want 1", len(next))
	}
	if next[0].Timestamp != 10 || next[0].SequenceNumber != 1 || !next[0].Marker {
		t.Fatalf("after SkipFrames(7): timestamp=%d sequence=%d marker=%v, want 10/1/true",
			next[0].Timestamp, next[0].SequenceNumber, next[0].Marker)
	}
}

func TestL16PacketizerThreeChannelsUnevenMTU(t *testing.T) {
	// Three channels: 6-byte sample frames. Packet mtu 28 leaves a 16-byte
	// payload budget, which does not divide the frame width: each packet
	// carries 2 frames (12 bytes) and 4 bytes of the budget stay unused.
	p, err := rtp.NewL16Packetizer(28, 3, 111, 42, 1000, rtp.NewFixedSequencer(500))
	if err != nil {
		t.Fatal(err)
	}

	input := l16PCM(
		[]int16{1, 2, 3}, []int16{4, 5, 6}, []int16{7, 8, 9}, []int16{10, 11, 12}, []int16{13, 14, 15},
	)
	packets, err := p.Packetize(input)
	if err != nil {
		t.Fatal(err)
	}
	if len(packets) != 3 {
		t.Fatalf("got %d packets, want 3", len(packets))
	}

	wantPayloadLens := []int{12, 12, 6}
	wantTimestamps := []uint32{1000, 1002, 1004}
	decoded, wireSizes := l16RoundTrip(t, packets)
	for i := range decoded {
		if len(decoded[i].Payload) != wantPayloadLens[i] {
			t.Fatalf("packet %d payload length = %d, want %d", i, len(decoded[i].Payload), wantPayloadLens[i])
		}
		if want := 12 + wantPayloadLens[i]; wireSizes[i] != want {
			t.Fatalf("packet %d wire size = %d, want %d", i, wireSizes[i], want)
		}
		if decoded[i].Timestamp != wantTimestamps[i] {
			t.Fatalf("packet %d timestamp = %d, want %d", i, decoded[i].Timestamp, wantTimestamps[i])
		}
		if decoded[i].SequenceNumber != uint16(500+i) { // nolint: gosec // G115
			t.Fatalf("packet %d sequence = %d, want %d", i, decoded[i].SequenceNumber, 500+i)
		}
	}
	if !bytes.Equal(decoded[2].Payload, []byte{0x00, 0x0d, 0x00, 0x0e, 0x00, 0x0f}) {
		t.Fatalf("tail payload = % x, want 00 0d 00 0e 00 0f", decoded[2].Payload)
	}
	var reassembled []byte
	for i := range decoded {
		reassembled = append(reassembled, decoded[i].Payload...)
	}
	if !bytes.Equal(reassembled, input) {
		t.Fatalf("reassembled = % x, want % x", reassembled, input)
	}
}

func TestL16PacketizerSplitAcrossCalls(t *testing.T) {
	p, err := rtp.NewL16Packetizer(22, 2, 96, 7, 5000, rtp.NewFixedSequencer(1))
	if err != nil {
		t.Fatal(err)
	}

	// The same five frames as one logical input, split over two calls.
	first, err := p.Packetize(l16PCM([]int16{1, -1}, []int16{2, -2}))
	if err != nil {
		t.Fatal(err)
	}
	second, err := p.Packetize(l16PCM([]int16{3, -3}, []int16{4, -4}, []int16{5, -5}))
	if err != nil {
		t.Fatal(err)
	}

	if len(first) != 1 || first[0].Timestamp != 5000 || first[0].SequenceNumber != 1 || !first[0].Marker {
		t.Fatalf("first call: %+v", first[0].Header)
	}
	if len(second) != 2 {
		t.Fatalf("second call produced %d packets, want 2", len(second))
	}
	// The clock continues across calls; the marker does not re-arm.
	if second[0].Timestamp != 5002 || second[0].Marker {
		t.Fatalf("second call packet 0: timestamp=%d marker=%v, want 5002/false",
			second[0].Timestamp, second[0].Marker)
	}
	if second[1].Timestamp != 5004 || second[1].SequenceNumber != 3 {
		t.Fatalf("second call packet 1: timestamp=%d sequence=%d, want 5004/3",
			second[1].Timestamp, second[1].SequenceNumber)
	}
}

func TestL16PacketizerEmptyAndBadFrameBetweenCalls(t *testing.T) {
	p, err := rtp.NewL16Packetizer(22, 2, 96, 7, 0, rtp.NewFixedSequencer(1))
	if err != nil {
		t.Fatal(err)
	}

	if _, err = p.Packetize(l16PCM([]int16{1, -1})); err != nil {
		t.Fatal(err)
	}

	// An empty call produces nothing and consumes nothing.
	empty, err := p.Packetize(nil)
	if err != nil || empty != nil {
		t.Fatalf("empty Packetize = %v, %v; want nil, nil", empty, err)
	}

	// A partial frame is rejected and consumes nothing.
	if _, err = p.Packetize([]byte{0x00, 0x01, 0xff}); err == nil {
		t.Fatal("partial frame: expected error, got nil")
	}

	next, err := p.Packetize(l16PCM([]int16{2, -2}))
	if err != nil {
		t.Fatal(err)
	}
	if len(next) != 1 || next[0].Timestamp != 1 || next[0].SequenceNumber != 2 || next[0].Marker {
		t.Fatalf("after empty+bad calls: timestamp=%d sequence=%d marker=%v, want 1/2/false",
			next[0].Timestamp, next[0].SequenceNumber, next[0].Marker)
	}
}

func TestL16PacketizerBadFrameKeepsFirstPacketMarker(t *testing.T) {
	p, err := rtp.NewL16Packetizer(22, 2, 96, 7, 0, rtp.NewFixedSequencer(1))
	if err != nil {
		t.Fatal(err)
	}

	// A rejected call must not consume the talkspurt-start Marker.
	if _, err = p.Packetize([]byte{0x00}); err == nil {
		t.Fatal("partial frame: expected error, got nil")
	}
	packets, err := p.Packetize(l16PCM([]int16{1, -1}))
	if err != nil {
		t.Fatal(err)
	}
	if len(packets) != 1 || !packets[0].Marker || packets[0].SequenceNumber != 1 {
		t.Fatalf("first valid packet: marker=%v sequence=%d, want true/1",
			packets[0].Marker, packets[0].SequenceNumber)
	}
}

func TestL16PacketizerSkipThenBadFrameThenRetry(t *testing.T) {
	p, err := rtp.NewL16Packetizer(22, 2, 96, 7, 100, rtp.NewFixedSequencer(1))
	if err != nil {
		t.Fatal(err)
	}

	if _, err = p.Packetize(l16PCM([]int16{1, -1})); err != nil {
		t.Fatal(err)
	}
	p.SkipFrames(5)

	// The bad frame after the skip must not consume the re-armed Marker.
	if _, err = p.Packetize([]byte{0x00, 0x01, 0xff}); err == nil {
		t.Fatal("partial frame: expected error, got nil")
	}
	packets, err := p.Packetize(l16PCM([]int16{2, -2}))
	if err != nil {
		t.Fatal(err)
	}
	if len(packets) != 1 || packets[0].Timestamp != 106 || packets[0].SequenceNumber != 2 || !packets[0].Marker {
		t.Fatalf("after skip+bad frame: timestamp=%d sequence=%d marker=%v, want 106/2/true",
			packets[0].Timestamp, packets[0].SequenceNumber, packets[0].Marker)
	}
}

func TestL16PacketizerSkipFramesZero(t *testing.T) {
	p, err := rtp.NewL16Packetizer(22, 2, 96, 7, 0, rtp.NewFixedSequencer(1))
	if err != nil {
		t.Fatal(err)
	}

	if _, err = p.Packetize(l16PCM([]int16{1, -1})); err != nil {
		t.Fatal(err)
	}
	p.SkipFrames(0) // no gap, no new talkspurt

	next, err := p.Packetize(l16PCM([]int16{2, -2}))
	if err != nil {
		t.Fatal(err)
	}
	if len(next) != 1 || next[0].Timestamp != 1 || next[0].Marker {
		t.Fatalf("after SkipFrames(0): timestamp=%d marker=%v, want 1/false",
			next[0].Timestamp, next[0].Marker)
	}
}

func TestL16PacketizerConstructorValidation(t *testing.T) {
	seq := rtp.NewFixedSequencer(1)
	for name, tc := range map[string]struct {
		mtu         uint16
		channels    uint16
		payloadType uint8
		sequencer   rtp.Sequencer
	}{
		"ZeroChannels":      {mtu: 100, channels: 0, payloadType: 96, sequencer: seq},
		"MTULeavesNoFrame":  {mtu: 15, channels: 2, payloadType: 96, sequencer: seq}, // 12-byte header + 4-byte frame needs 16
		"PayloadTypeTooBig": {mtu: 100, channels: 2, payloadType: 128, sequencer: seq},
		"NilSequencer":      {mtu: 100, channels: 2, payloadType: 96, sequencer: nil},
	} {
		t.Run(name, func(t *testing.T) {
			if _, err := rtp.NewL16Packetizer(tc.mtu, tc.channels, tc.payloadType, 7, 0, tc.sequencer); err == nil {
				t.Fatal("expected error, got nil")
			}
		})
	}

	// The smallest mtu that fits one stereo frame is accepted.
	if _, err := rtp.NewL16Packetizer(16, 2, 96, 7, 0, rtp.NewFixedSequencer(1)); err != nil {
		t.Fatalf("mtu 16 with stereo: expected success, got %v", err)
	}
	// Payload type 127 is the largest legal value.
	if _, err := rtp.NewL16Packetizer(100, 2, 127, 7, 0, rtp.NewFixedSequencer(1)); err != nil {
		t.Fatalf("payload type 127: expected success, got %v", err)
	}
}
