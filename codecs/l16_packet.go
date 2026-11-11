// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package codecs

// L16Payloader payloads L16 audio: channel-interleaved, big-endian signed
// 16-bit linear PCM as described by RFC 2586. One sample frame spans all
// channels and occupies 2*Channels bytes; fragments never split a frame.
type L16Payloader struct {
	// Channels is the number of interleaved channels in the stream.
	Channels uint16
}

// Payload fragments an L16 packet across one or more byte arrays.
//
// mtu is the maximum payload size in bytes. Every fragment holds a whole
// number of sample frames and is filled with as many frames as fit in mtu;
// the final fragment keeps the remaining complete frames.
//
// No fragments are returned when Channels is zero, when mtu cannot hold even
// one sample frame, or when payload does not contain a whole number of
// sample frames. An empty payload yields no fragments (and no empty
// fragment). Each returned fragment owns its bytes: later changes to payload
// do not affect them.
func (p *L16Payloader) Payload(mtu uint16, payload []byte) [][]byte {
	var out [][]byte

	frameSize := 2 * int(p.Channels)
	if frameSize == 0 || len(payload) == 0 {
		return out
	}
	if len(payload)%frameSize != 0 || int(mtu) < frameSize {
		return out
	}

	framesPerFragment := int(mtu) / frameSize
	fragmentSize := framesPerFragment * frameSize
	for len(payload) >= fragmentSize {
		o := make([]byte, fragmentSize)
		copy(o, payload[:fragmentSize])
		out = append(out, o)
		payload = payload[fragmentSize:]
	}
	if len(payload) > 0 {
		o := make([]byte, len(payload))
		copy(o, payload)

		return append(out, o)
	}

	return out
}
