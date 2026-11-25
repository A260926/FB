// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

// Command redundant demonstrates merging two complementary RTP copies into one
// canonical, continuous sequence/SSRC domain.
package main

import (
	"errors"
	"fmt"
	"time"

	"github.com/pion/interceptor/pkg/jitterbuffer"
	"github.com/pion/rtp"
)

func main() {
	base := time.Unix(1_700_000_000, 0)
	ticks := 0
	cfg := jitterbuffer.RedundantConfig{
		OutputSSRC:  424242,
		PayloadType: 96,
		Paths: [2]jitterbuffer.RedundantPath{
			{SSRC: 1001, SequenceOffset: 7, RTT: 10 * time.Millisecond},
			{SSRC: 2002, SequenceOffset: 99, RTT: 30 * time.Millisecond},
		},
		Window: 16,
		Hold:   20 * time.Millisecond,
		Retry:  50 * time.Millisecond,
		Now: func() time.Time {
			ticks++
			return base.Add(time.Duration(ticks) * time.Millisecond)
		},
	}

	buffer, err := jitterbuffer.NewRedundant(cfg)
	if err != nil {
		panic(err)
	}

	// Canonical sequences 100..105. Each physical path loses a different
	// subset; sequence 101 and 104 arrive twice.
	arrivals := []struct {
		path      int
		canonical uint16
	}{
		{0, 100},
		{0, 101},
		{1, 101},
		{1, 102},
		{0, 103},
		{1, 104},
		{0, 104},
		{1, 105},
	}

	for _, arrival := range arrivals {
		path := cfg.Paths[arrival.path]
		buffer.Push(&rtp.Packet{
			Header: rtp.Header{
				Version:        2,
				Marker:         true,
				PayloadType:    cfg.PayloadType,
				SequenceNumber: arrival.canonical + path.SequenceOffset,
				Timestamp:      90000 + uint32(arrival.canonical)*3000,
				SSRC:           path.SSRC,
			},
			Payload: []byte{byte(arrival.canonical)},
		})
	}

	for i := 0; i < len(arrivals); i++ {
		packet, popErr := buffer.Pop()
		if errors.Is(popErr, jitterbuffer.ErrRedundantPending) {
			// A real caller advances its monotonic clock and retries. The
			// example input is contiguous after merging, so this is defensive.
			break
		}
		if popErr != nil {
			panic(popErr)
		}

		fmt.Printf("canonical sequence=%d output-ssrc=%d pt=%d payload=%d\n",
			packet.SequenceNumber, packet.SSRC, packet.PayloadType, packet.Payload[0])
	}
}
