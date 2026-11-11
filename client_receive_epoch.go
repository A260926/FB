package gortsplib

import (
	"net/url"
	"sync"

	"github.com/bluenviron/gortsplib/v5/pkg/base"
	"github.com/bluenviron/gortsplib/v5/pkg/description"
	"github.com/bluenviron/gortsplib/v5/pkg/headers"
	"github.com/bluenviron/gortsplib/v5/pkg/rtptime"
)

// reception phases while a PLAY request is in flight.
const (
	epochPhaseOpen     uint8 = 0 // ordinary reception
	epochPhaseDropping uint8 = 1 // PLAY sent, its response has not been seen yet
	epochPhaseHolding  uint8 = 2 // PLAY response seen, the new epoch is being published
	epochPhaseClosed   uint8 = 3 // the client is closing
)

// receiveEpochGate coordinates the handover between two reception epochs
// while a PLAY request is in flight.
//
// In the dropping phase every inbound RTP/RTCP packet is discarded: it does
// not reach the user callbacks and does not move reception statistics or time
// mappings. Once the matching PLAY response has been read, the gate enters the
// holding phase: packets following the response block until the new epoch has
// been published, so that the first frames of the new position are never lost
// to a scheduling race. On TCP the frame is retained on the reader's stack;
// on UDP the packets stay in the socket receive buffer.
//
// RTSP serializes requests on the control connection, so while the gate is not
// open, the next response read by the connection reader is necessarily the
// PLAY response. A non-200 response leaves the gate in the dropping phase
// instead of holding, which keeps authentication retries working.
type receiveEpochGate struct {
	mu    sync.Mutex
	cond  *sync.Cond
	phase uint8
}

func newReceiveEpochGate() *receiveEpochGate {
	g := &receiveEpochGate{}
	g.cond = sync.NewCond(&g.mu)
	return g
}

func (g *receiveEpochGate) begin() {
	g.mu.Lock()
	g.phase = epochPhaseDropping
	g.mu.Unlock()
}

// onResponse is called by the connection reader for every response, before it
// starts reading the next message. Flipping to the holding phase here
// guarantees that frames following a successful PLAY response on the wire
// cannot be processed before the new epoch is ready.
func (g *receiveEpochGate) onResponse(statusCode base.StatusCode) {
	g.mu.Lock()
	defer g.mu.Unlock()
	if g.phase == epochPhaseDropping && statusCode == base.StatusOK {
		g.phase = epochPhaseHolding
	}
}

// allow reports whether an inbound packet may be processed. It blocks while
// the new epoch is being published.
func (g *receiveEpochGate) allow() bool {
	g.mu.Lock()
	defer g.mu.Unlock()

	for g.phase == epochPhaseHolding {
		g.cond.Wait()
	}

	return g.phase == epochPhaseOpen
}

func (g *receiveEpochGate) open() {
	g.mu.Lock()
	g.phase = epochPhaseOpen
	g.cond.Broadcast()
	g.mu.Unlock()
}

// close releases every blocked reader and rejects further packets.
func (g *receiveEpochGate) close() {
	g.mu.Lock()
	g.phase = epochPhaseClosed
	g.cond.Broadcast()
	g.mu.Unlock()
}

// resetReceiveEpoch publishes a new reception epoch after a successful PLAY.
//
// It clears the PTS decoder and every receiver's reordering buffer, duplicate
// detection, extended sequence number cycles, jitter history and sender report
// mapping. A sequence number threshold parsed from the PLAY response's
// RTP-Info header is installed on the matching medias. The gate is opened only
// once everything is ready.
func (c *Client) resetReceiveEpoch(res *base.Response) {
	c.propsMutex.Lock()

	thresholds := c.parseRTPInfoThresholds(res)

	c.timeDecoderMutex.Lock()
	c.timeDecoder = &rtptime.GlobalDecoder{}
	c.timeDecoder.Initialize()
	c.timeDecoderMutex.Unlock()

	for medi := range c.setuppedMedias {
		threshold, ok := thresholds[medi]
		c.setuppedMedias[medi].resetReceive(threshold, ok)
	}

	c.propsMutex.Unlock()

	c.epochGate.open()
}

// abortReceiveEpoch reopens reception without publishing a new epoch: it is
// used when the PLAY request failed or returned a non-200 status code.
func (c *Client) abortReceiveEpoch() {
	c.epochGate.open()
}

// parseRTPInfoThresholds maps setupped medias to the unique sequence number
// advertised for them in the PLAY response's RTP-Info header.
//
// Medias are matched by absolute URL (userinfo is ignored), never by header
// item order or payload type. Unknown URLs are ignored. A malformed header, an
// entry without a seq, or several entries matching the same media disable the
// threshold for that media; the epoch itself is still published.
func (c *Client) parseRTPInfoThresholds(res *base.Response) map[*description.Media]*uint16 {
	ret := make(map[*description.Media]*uint16)

	values, ok := res.Header["RTP-Info"]
	if !ok {
		return ret
	}

	var info headers.RTPInfo
	if err := info.Unmarshal(values); err != nil {
		return ret
	}

	for medi := range c.setuppedMedias {
		setupURL, err := medi.URL(c.baseURL)
		if err != nil {
			continue
		}

		var match *headers.RTPInfoEntry
		matches := 0
		for _, entry := range info {
			entryURL, err := base.ParseURL(entry.URL)
			if err != nil {
				continue
			}

			if sameURLWithoutUserinfo(entryURL, setupURL) {
				match = entry
				matches++
			}
		}

		if matches == 1 && match.SequenceNumber != nil {
			seq := *match.SequenceNumber
			ret[medi] = &seq
		}
	}

	return ret
}

func sameURLWithoutUserinfo(a, b *base.URL) bool {
	au := url.URL(*a.CloneWithoutCredentials())
	bu := url.URL(*b.CloneWithoutCredentials())
	return au.String() == bu.String()
}
