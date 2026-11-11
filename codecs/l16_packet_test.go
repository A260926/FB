// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package codecs

import (
	"bytes"
	"testing"
)

// l16PCM encodes channel-interleaved signed 16-bit samples in network byte
// order, one frame per inner slice.
func l16PCM(frames ...[]int16) []byte {
	var out []byte
	for _, frame := range frames {
		for _, sample := range frame {
			out = append(out, byte(uint16(sample)>>8), byte(uint16(sample))) // nolint: gosec // G115
		}
	}

	return out
}

func l16PayloadLens(payloads [][]byte) []int {
	lens := make([]int, len(payloads))
	for i, p := range payloads {
		lens[i] = len(p)
	}

	return lens
}

func TestL16Payloader(t *testing.T) {
	input := l16PCM([]int16{1, -1}, []int16{2, -2}, []int16{3, -3}, []int16{4, -4}, []int16{5, -5})

	t.Run("StereoFramesFillMTU", func(t *testing.T) {
		payloader := L16Payloader{Channels: 2}
		payloads := payloader.Payload(10, input) // 2 frames per fragment, frame is 4 bytes
		if got, want := l16PayloadLens(payloads), []int{8, 8, 4}; !equalInts(got, want) {
			t.Fatalf("fragment lengths = %v, want %v", got, want)
		}
		if first := payloads[0]; !bytes.Equal(first, []byte{0x00, 0x01, 0xff, 0xff, 0x00, 0x02, 0xff, 0xfe}) {
			t.Fatalf("first fragment = % x, want 00 01 ff ff 00 02 ff fe", first)
		}
		var joined []byte
		for _, p := range payloads {
			joined = append(joined, p...)
		}
		if !bytes.Equal(joined, input) {
			t.Fatalf("reassembled = % x, want % x", joined, input)
		}
	})

	t.Run("ThreeChannelsUnevenMTU", func(t *testing.T) {
		payloader := L16Payloader{Channels: 3}
		// mtu 16 is not a multiple of the 6-byte frame: 2 frames (12 bytes)
		// per fragment, 4 bytes of slack.
		payloads := payloader.Payload(16, l16PCM(
			[]int16{1, 2, 3}, []int16{4, 5, 6}, []int16{7, 8, 9}, []int16{10, 11, 12}, []int16{13, 14, 15},
		))
		if got, want := l16PayloadLens(payloads), []int{12, 12, 6}; !equalInts(got, want) {
			t.Fatalf("fragment lengths = %v, want %v", got, want)
		}
		if !bytes.Equal(payloads[2], []byte{0x00, 0x0d, 0x00, 0x0e, 0x00, 0x0f}) {
			t.Fatalf("tail fragment = % x, want 00 0d 00 0e 00 0f", payloads[2])
		}
	})

	t.Run("ExactMultipleLeavesNoEmptyTail", func(t *testing.T) {
		payloader := L16Payloader{Channels: 1}
		payloads := payloader.Payload(4, l16PCM([]int16{1}, []int16{2}, []int16{3}, []int16{4}))
		if got, want := l16PayloadLens(payloads), []int{4, 4}; !equalInts(got, want) {
			t.Fatalf("fragment lengths = %v, want %v", got, want)
		}
	})

	t.Run("EmptyResults", func(t *testing.T) {
		stereo := l16PCM([]int16{1, -1})
		for name, tc := range map[string]struct {
			payloader L16Payloader
			mtu       uint16
			payload   []byte
		}{
			"ZeroChannels":     {L16Payloader{Channels: 0}, 100, stereo},
			"MTUBelowOneFrame": {L16Payloader{Channels: 2}, 3, stereo},
			"IncompleteInput":  {L16Payloader{Channels: 2}, 100, stereo[:len(stereo)-1]},
			"EmptyInput":       {L16Payloader{Channels: 2}, 100, nil},
		} {
			t.Run(name, func(t *testing.T) {
				if got := tc.payloader.Payload(tc.mtu, tc.payload); len(got) != 0 {
					t.Fatalf("Payload(%d, % x) = %v, want no fragments", tc.mtu, tc.payload, got)
				}
			})
		}
	})

	t.Run("FragmentsOwnTheirBytes", func(t *testing.T) {
		payloader := L16Payloader{Channels: 2}
		mutable := l16PCM([]int16{1, -1}, []int16{2, -2}, []int16{3, -3})
		payloads := payloader.Payload(10, mutable)
		for i := range mutable {
			mutable[i] = 0
		}
		if !bytes.Equal(payloads[0], []byte{0x00, 0x01, 0xff, 0xff, 0x00, 0x02, 0xff, 0xfe}) {
			t.Fatalf("fragment changed after mutating input: % x", payloads[0])
		}
	})
}

func equalInts(a, b []int) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}

	return true
}
