// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package rtp

import (
	"errors"

	"github.com/pion/rtp/codecs"
)

var (
	errL16ZeroChannels = errors.New("l16: channels must be greater than zero")
	errL16MTUTooSmall  = errors.New("l16: mtu leaves no room for a complete sample frame")
	errL16PayloadType  = errors.New("l16: payload type must be <= 127")
	errL16NilSequencer = errors.New("l16: sequencer must not be nil")
	errL16PartialFrame = errors.New("l16: pcm is not a whole number of sample frames")
)

// rtpFixedHeaderSize is the size of the fixed RTP header in bytes. The
// L16Packetizer never adds extensions or padding, so every packet it
// produces is exactly rtpFixedHeaderSize+len(payload) bytes on the wire.
const rtpFixedHeaderSize = 12

// L16Packetizer packetizes a continuous L16 (linear PCM) audio stream.
//
// Unlike the generic Packetizer, which treats one Packetize call as one
// frame sharing a single timestamp and marks the last fragment, L16Packetizer
// is sample-accurate: every packet's timestamp is the sample clock value of
// the first sample frame it carries, one sample frame (all channels) advances
// the clock by a single tick, and the Marker bit signals the start of a
// talkspurt (the first packet after construction or after SkipFrames).
//
// The two MTUs involved are easy to confuse:
//   - NewL16Packetizer's mtu is a whole-packet budget: it includes the fixed
//     12-byte RTP header. This entry point adds no extensions or padding, so
//     no packet it produces exceeds mtu bytes on the wire.
//   - codecs.L16Payloader.Payload's mtu is a payload-only budget: it bounds
//     the PCM bytes in a single fragment, header not included.
//
// The payload type and the sample rate of the clock are chosen by the caller
// (e.g. via SDP); this packetizer only counts sample frames.
type L16Packetizer struct {
	payloadMTU  uint16 // per-packet PCM budget: mtu minus the fixed RTP header
	payloadType uint8
	ssrc        uint32
	sequencer   Sequencer
	timestamp   uint32
	payloader   *codecs.L16Payloader
	frameSize   int  // bytes per sample frame: 2 * channels
	markNext    bool // mark the first packet of the next non-empty Packetize
}

// NewL16Packetizer returns a packetizer for a continuous L16 stream.
//
// mtu is the maximum size of a whole RTP packet including the fixed 12-byte
// header; channels is the number of interleaved PCM channels; payloadType is
// the 7-bit RTP payload type; initialTimestamp is the sample clock value of
// the first frame passed to Packetize; sequencer supplies sequence numbers.
//
// It returns an error when channels is zero, when mtu leaves no room for one
// complete sample frame after the fixed header, when payloadType exceeds 127,
// or when sequencer is nil.
func NewL16Packetizer(
	mtu uint16,
	channels uint16,
	payloadType uint8,
	ssrc uint32,
	initialTimestamp uint32,
	sequencer Sequencer,
) (*L16Packetizer, error) {
	if channels == 0 {
		return nil, errL16ZeroChannels
	}
	if payloadType > 127 {
		return nil, errL16PayloadType
	}
	if sequencer == nil {
		return nil, errL16NilSequencer
	}
	frameSize := 2 * int(channels)
	if int(mtu) < rtpFixedHeaderSize+frameSize {
		return nil, errL16MTUTooSmall
	}

	return &L16Packetizer{
		payloadMTU:  mtu - rtpFixedHeaderSize,
		payloadType: payloadType,
		ssrc:        ssrc,
		sequencer:   sequencer,
		timestamp:   initialTimestamp,
		payloader:   &codecs.L16Payloader{Channels: channels},
		frameSize:   frameSize,
		markNext:    true,
	}, nil
}

// Packetize converts pcm into one or more RTP packets.
//
// pcm must be channel-interleaved, big-endian signed 16-bit samples holding a
// whole number of sample frames; it is not byte-swapped. Each packet's
// timestamp is the sample clock value of its first frame, and each sample
// frame advances the clock by one tick regardless of the channel count. The
// first packet of a talkspurt (after construction or SkipFrames) is marked.
//
// An empty pcm is a no-op that returns no packets and changes no state. A pcm
// that does not hold a whole number of sample frames returns an error and
// likewise consumes no timestamp, sequence number, or pending Marker.
func (p *L16Packetizer) Packetize(pcm []byte) ([]*Packet, error) {
	if len(pcm) == 0 {
		return nil, nil // nolint: nilnil
	}
	if len(pcm)%p.frameSize != 0 {
		return nil, errL16PartialFrame
	}

	payloads := p.payloader.Payload(p.payloadMTU, pcm)
	packets := make([]*Packet, len(payloads))
	for i, pp := range payloads {
		packets[i] = &Packet{
			Header: Header{
				Version:        2,
				Padding:        false,
				Extension:      false,
				Marker:         p.markNext && i == 0,
				PayloadType:    p.payloadType,
				SequenceNumber: p.sequencer.NextSequenceNumber(),
				Timestamp:      p.timestamp,
				SSRC:           p.ssrc,
			},
			Payload: pp,
		}
		p.timestamp += uint32(len(pp)) / uint32(p.frameSize) // nolint: gosec // G115
	}
	p.markNext = false

	return packets, nil
}

// SkipFrames advances the sample clock by n frames without producing packets
// or consuming sequence numbers, as if n frames of silence were dropped. The
// first packet of the next non-empty Packetize is marked as the start of a
// new talkspurt. SkipFrames(0) is a no-op and does not start a talkspurt.
func (p *L16Packetizer) SkipFrames(n uint32) {
	if n == 0 {
		return
	}
	p.timestamp += n
	p.markNext = true
}
