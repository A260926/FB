// Package main contains an example.
package main

import (
	"log"
	"time"

	"github.com/pion/rtcp"
	"github.com/pion/rtp"

	"github.com/bluenviron/gortsplib/v5"
	"github.com/bluenviron/gortsplib/v5/pkg/base"
	"github.com/bluenviron/gortsplib/v5/pkg/description"
	"github.com/bluenviron/gortsplib/v5/pkg/format"
	"github.com/bluenviron/gortsplib/v5/pkg/headers"
)

// This example shows how to seek repeatedly inside the same RTSP session
// (PAUSE followed by PLAY at a different position) without rebuilding the
// session, and how to make each PLAY start a fresh reception epoch.
//
// Without ResetOnPlay, the RTP reception history survives the seek: a sequence
// number that jumps backwards (for instance from 1001 to 100) is interpreted
// as tens of thousands of lost packets on TCP, or as a late duplicate and
// dropped on UDP. The PTS decoder and the sender-report based PacketNTP mapping
// keep using data from the previous position as well.
//
// With ResetOnPlay enabled, every successful PLAY starts a new reception epoch:
//   - packets arriving while the PLAY response is in flight are discarded,
//     without invoking callbacks or moving statistics;
//   - reordering buffers, duplicate detection, sequence number cycles, jitter
//     history, sender-report mappings and the PTS decoder are cleared;
//   - the first packet of the new position is delivered immediately, and its
//     sequence number becomes the counting base, so the jump is not counted as
//     loss;
//   - a RTP-Info "seq" in the PLAY response, when present, gates late packets.
//
// Statistics keep two scopes. In Client.Stats():
//   - received, lost, error and byte counters are cumulative and keep growing
//     across epochs;
//   - LastSequenceNumber, LastRTP, LastNTP and Jitter describe only the current
//     epoch, and are zero until its first packet arrives.
//
// RTCP receiver reports likewise describe only the current epoch, and no
// receiver report is sent before that epoch receives its first RTP packet.

func main() {
	// parse URL
	u, err := base.ParseURL("rtsp://myuser:mypass@localhost:8554/mystream")
	if err != nil {
		panic(err)
	}

	c := gortsplib.Client{
		Scheme: u.Scheme,
		Host:   u.Host,

		// must be configured before Start(); defaults to false, so existing
		// callers keep their current behavior.
		ResetOnPlay: true,
	}

	// connect to the server
	err = c.Start()
	if err != nil {
		panic(err)
	}
	defer c.Close()

	// find available medias
	desc, _, err := c.Describe(u)
	if err != nil {
		panic(err)
	}

	// setup all medias
	err = c.SetupAll(desc.BaseURL, desc.Medias)
	if err != nil {
		panic(err)
	}

	// called when a RTP packet arrives
	c.OnPacketRTPAny(func(medi *description.Media, forma format.Format, pkt *rtp.Packet) {
		// PacketPTS is computed from the epoch's own origin after every PLAY.
		pts, ptsOK := c.PacketPTS(medi, pkt)
		log.Printf("RTP packet from media %v, seq=%d, ptsOK=%v, pts=%d",
			medi, pkt.SequenceNumber, ptsOK, pts)
	})

	// called when a RTCP packet arrives
	c.OnPacketRTCPAny(func(medi *description.Media, pkt rtcp.Packet) {
		log.Printf("RTCP packet from media %v, type %T\n", medi, pkt)
	})

	// start playing
	_, err = c.Play(nil)
	if err != nil {
		panic(err)
	}

	positions := []time.Duration{
		30 * time.Second,
		10 * time.Second, // seek backwards
		90 * time.Second, // seek forwards
	}

	for _, pos := range positions {
		time.Sleep(2 * time.Second)

		// pause, then play from another position inside the same session.
		// the successful PLAY publishes a new reception epoch.
		if _, err = c.Pause(); err != nil {
			panic(err)
		}

		if _, err = c.Play(&headers.Range{
			Value: &headers.RangeNPT{Start: pos},
		}); err != nil {
			panic(err)
		}

		// cumulative counters span every epoch; the "last" values describe
		// only the epoch that the PLAY above just opened.
		stats := c.Stats()
		log.Printf("after seek to %v: received=%d lost=%d bytes=%d",
			pos,
			stats.Session.InboundRTPPackets,
			stats.Session.InboundRTPPacketsLost,
			stats.Session.InboundBytes)
	}
}
