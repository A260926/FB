package gortsplib

import (
	"bufio"
	"fmt"
	"net"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/pion/rtcp"
	"github.com/pion/rtp"
	"github.com/stretchr/testify/require"

	"github.com/bluenviron/gortsplib/v5/pkg/base"
	"github.com/bluenviron/gortsplib/v5/pkg/conn"
	"github.com/bluenviron/gortsplib/v5/pkg/description"
	"github.com/bluenviron/gortsplib/v5/pkg/format"
	"github.com/bluenviron/gortsplib/v5/pkg/headers"
	"github.com/bluenviron/gortsplib/v5/pkg/ntp"
)

// epochSend is a frame pushed by the test server to the client.
type epochSend struct {
	media  int // 0 or 1
	isRTCP bool
	data   []byte
	done   chan error
}

// epochServerConfig configures the local RTSP server used by the epoch tests.
type epochServerConfig struct {
	protocol Protocol

	// statusCode returned by the n-th PLAY request (0 means 200)
	playStatus func(playCount int) base.StatusCode
	// statusCode returned by the n-th PAUSE request (0 means 200)
	pauseStatus func(pauseCount int) base.StatusCode
	// RTP-Info header value returned by the n-th PLAY request ("" disables it)
	rtpInfo func(s *epochServer, playCount int) string
}

// epochServer is a minimal RTSP server with two G711 medias sharing payload
// type 0, reachable over real local TCP and UDP sockets.
type epochServer struct {
	t   *testing.T
	cfg epochServerConfig

	l          net.Listener
	udpSockets [4]net.PacketConn // RTP/RTCP of media 0, then RTP/RTCP of media 1
	clientPort [2][2]int

	sends      chan epochSend
	serverDone chan error

	playCount  atomic.Int32
	pauseCount atomic.Int32
}

func startEpochServer(t *testing.T, cfg epochServerConfig) (*epochServer, *base.URL) {
	t.Helper()

	l, err := net.Listen("tcp", "127.0.0.1:0")
	require.NoError(t, err)

	u, err := base.ParseURL("rtsp://" + l.Addr().String() + "/vod")
	require.NoError(t, err)

	s := &epochServer{
		t:          t,
		cfg:        cfg,
		l:          l,
		sends:      make(chan epochSend),
		serverDone: make(chan error, 1),
	}

	for i := range s.udpSockets {
		pc, err := net.ListenPacket("udp4", "127.0.0.1:0")
		require.NoError(t, err)
		s.udpSockets[i] = pc
	}

	go s.run()

	return s, u
}

func (s *epochServer) close() {
	s.l.Close()
	for _, pc := range s.udpSockets {
		pc.Close()
	}
}

func (s *epochServer) rtspURL(path string) string {
	return "rtsp://" + s.l.Addr().String() + path
}

func (s *epochServer) waitDone(t *testing.T) {
	t.Helper()
	select {
	case err := <-s.serverDone:
		require.NoError(t, err)
	case <-time.After(5 * time.Second):
		t.Fatal("server did not finish")
	}
}

func (s *epochServer) send(media int, isRTCP bool, data []byte) {
	done := make(chan error, 1)
	select {
	case s.sends <- epochSend{media: media, isRTCP: isRTCP, data: data, done: done}:
	case <-time.After(5 * time.Second):
		s.t.Fatal("server send timeout")
	}
	require.NoError(s.t, <-done)
}

func (s *epochServer) run() {
	s.serverDone <- s.runInner()
}

func (s *epochServer) runInner() error {
	nc, err := s.l.Accept()
	if err != nil {
		return err
	}
	defer nc.Close()
	nc.SetDeadline(time.Now().Add(20 * time.Second))

	co := conn.NewConn(bufio.NewReader(nc), nc)
	requests := make(chan *base.Request)
	readErr := make(chan error, 1)

	go func() {
		for {
			r, err := co.ReadRequest()
			if err != nil {
				readErr <- err
				return
			}
			requests <- r
		}
	}()

	sdp := []byte("v=0\r\n" +
		"o=- 0 0 IN IP4 127.0.0.1\r\n" +
		"s=local\r\n" +
		"c=IN IP4 127.0.0.1\r\n" +
		"t=0 0\r\n" +
		"a=control:*\r\n" +
		"m=audio 0 RTP/AVP 0\r\n" +
		"a=control:audio0\r\n" +
		"m=audio 0 RTP/AVP 0\r\n" +
		"a=control:audio1\r\n")

	for {
		select {
		case err := <-readErr:
			return err

		case send := <-s.sends:
			var serr error
			if s.cfg.protocol == ProtocolTCP {
				channel := send.media * 2
				if send.isRTCP {
					channel++
				}
				serr = co.WriteInterleavedFrame(
					&base.InterleavedFrame{Channel: channel, Payload: send.data},
					make([]byte, 2048))
			} else {
				idx := send.media * 2
				if send.isRTCP {
					idx++
				}
				port := s.clientPort[send.media][0]
				if send.isRTCP {
					port = s.clientPort[send.media][1]
				}
				_, serr = s.udpSockets[idx].WriteTo(send.data,
					&net.UDPAddr{IP: net.ParseIP("127.0.0.1"), Port: port})
			}
			send.done <- serr

		case r := <-requests:
			h := base.Header{"CSeq": r.Header["CSeq"], "Session": {"epoch-local"}}
			var body []byte

			switch r.Method {
			case base.Options:
				h["Public"] = base.HeaderValue{"DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN"}

			case base.Describe:
				h["Content-Type"] = base.HeaderValue{"application/sdp"}
				h["Content-Base"] = base.HeaderValue{
					fmt.Sprintf("rtsp://%s/vod/", nc.LocalAddr().(*net.TCPAddr).String())}
				body = sdp

			case base.Setup:
				var tr headers.Transport
				if err := tr.Unmarshal(r.Header["Transport"]); err != nil {
					return err
				}

				media := 0
				if r.URL.Path == "/vod/audio1" {
					media = 1
				}
				s.clientPort[media] = *tr.ClientPorts

				tr.Delivery = new(headers.TransportDeliveryUnicast)
				if s.cfg.protocol == ProtocolUDP {
					tr.ServerPorts = &[2]int{
						s.udpSockets[media*2].LocalAddr().(*net.UDPAddr).Port,
						s.udpSockets[media*2+1].LocalAddr().(*net.UDPAddr).Port,
					}
				}
				h["Transport"] = tr.Marshal()

			case base.Play:
				n := int(s.playCount.Add(1))
				if s.cfg.rtpInfo != nil {
					if v := s.cfg.rtpInfo(s, n); v != "" {
						h["RTP-Info"] = base.HeaderValue{v}
					}
				}
				if s.cfg.playStatus != nil {
					if code := s.cfg.playStatus(n); code != 0 {
						h["CSeq"] = r.Header["CSeq"]
						if err := co.WriteResponse(&base.Response{
							StatusCode: base.StatusCode(code), Header: h,
						}); err != nil {
							return err
						}
						continue
					}
				}

			case base.Pause:
				n := int(s.pauseCount.Add(1))
				if s.cfg.pauseStatus != nil {
					if code := s.cfg.pauseStatus(n); code != 0 {
						if err := co.WriteResponse(&base.Response{
							StatusCode: base.StatusCode(code), Header: h,
						}); err != nil {
							return err
						}
						continue
					}
				}

			case base.Teardown:
				return co.WriteResponse(&base.Response{StatusCode: base.StatusOK, Header: h})

			default:
				return fmt.Errorf("unexpected method %s", r.Method)
			}

			if err := co.WriteResponse(&base.Response{
				StatusCode: base.StatusOK, Header: h, Body: body,
			}); err != nil {
				return err
			}
		}
	}
}

func epochRTP(mediaPT uint8, seq uint16, ts uint32) []byte {
	b, err := (&rtp.Packet{
		Header: rtp.Header{
			Version: 2, PayloadType: mediaPT, SequenceNumber: seq, Timestamp: ts, SSRC: 1234,
		},
		Payload: []byte{0x55},
	}).Marshal()
	if err != nil {
		panic(err)
	}
	return b
}

func epochSR(rtpTime uint32, at time.Time) []byte {
	b, err := (&rtcp.SenderReport{
		SSRC:    1234,
		NTPTime: ntp.Encode(at),
		RTPTime: rtpTime,
	}).Marshal()
	if err != nil {
		panic(err)
	}
	return b
}

// receivedPacket records a packet delivered to the user callback and whether
// the reception gate had already been released.
type receivedPacket struct {
	media    *description.Media
	format   format.Format
	seq      uint16
	ts       uint32
	released bool
}

type epochClientHarness struct {
	t        *testing.T
	server   *epochServer
	client   *Client
	sd       *description.Session
	received chan receivedPacket

	released atomic.Bool
	lost     atomic.Uint64

	blockPlayAt     int
	responseSeen    chan struct{}
	responseRelease chan struct{}
	releaseOnce     sync.Once
}

func newEpochClientHarness(
	t *testing.T,
	protocol Protocol,
	cfg epochServerConfig,
	blockPlayAt int,
) *epochClientHarness {
	t.Helper()

	cfg.protocol = protocol
	server, u := startEpochServer(t, cfg)

	h := &epochClientHarness{
		t:               t,
		server:          server,
		received:        make(chan receivedPacket, 256),
		blockPlayAt:     blockPlayAt,
		responseSeen:    make(chan struct{}),
		responseRelease: make(chan struct{}),
	}

	var blocked atomic.Int32

	h.client = &Client{
		Scheme:      u.Scheme,
		Host:        u.Host,
		Protocol:    &protocol,
		ResetOnPlay: true,
		OnResponse: func(res *base.Response) {
			if len(res.Header["RTP-Info"]) == 0 {
				return
			}
			if int(blocked.Add(1)) == h.blockPlayAt {
				close(h.responseSeen)
				<-h.responseRelease
			}
		},
		OnPacketsLost: func(n uint64) { h.lost.Add(n) },
		OnDecodeError: func(error) {},
	}
	require.NoError(t, h.client.Start())

	sd, _, err := h.client.Describe(u)
	require.NoError(t, err)
	require.NoError(t, h.client.SetupAll(sd.BaseURL, sd.Medias))
	h.sd = sd

	h.client.OnPacketRTPAny(func(medi *description.Media, forma format.Format, pkt *rtp.Packet) {
		h.received <- receivedPacket{
			media:    medi,
			format:   forma,
			seq:      pkt.SequenceNumber,
			ts:       pkt.Timestamp,
			released: h.released.Load(),
		}
	})

	return h
}

func (h *epochClientHarness) close() {
	h.releaseOnce.Do(func() { close(h.responseRelease) })
	h.client.Close()
	h.server.close()
	h.server.waitDone(h.t)
}

func (h *epochClientHarness) release() {
	h.released.Store(true)
	h.releaseOnce.Do(func() { close(h.responseRelease) })
}

func (h *epochClientHarness) waitPacket(wantSeq uint16) receivedPacket {
	h.t.Helper()
	select {
	case p := <-h.received:
		require.Equal(h.t, wantSeq, p.seq)
		return p
	case <-time.After(5 * time.Second):
		h.t.Fatalf("timeout waiting for RTP packet %d", wantSeq)
		return receivedPacket{}
	}
}

func (h *epochClientHarness) assertNoPacket() {
	h.t.Helper()
	select {
	case p := <-h.received:
		h.t.Fatalf("unexpected RTP packet %d", p.seq)
	case <-time.After(100 * time.Millisecond):
	}
}

func (h *epochClientHarness) drainPackets() []receivedPacket {
	var ret []receivedPacket
	for {
		select {
		case p := <-h.received:
			ret = append(ret, p)
		default:
			return ret
		}
	}
}

func (h *epochClientHarness) formatStats(media int) SessionStatsFormat {
	return h.client.Stats().Session.Medias[h.sd.Medias[media]].Formats[h.sd.Medias[media].Formats[0]]
}

// TestClientReceiveEpochTCP exercises the full PLAY/PAUSE/PLAY reception
// handover over interleaved TCP, including frames written in the same
// connection burst as the PLAY response.
func TestClientReceiveEpochTCP(t *testing.T) {
	h := newEpochClientHarness(t, ProtocolTCP, epochServerConfig{
		rtpInfo: func(s *epochServer, n int) string {
			// entries are intentionally in reverse order on the second PLAY
			if n == 1 {
				return "url=" + s.rtspURL("/vod/audio0;seq=100;rtptime=800000")
			}
			if n == 2 {
				return "url=" + s.rtspURL("/vod/audio1;seq=400") + "," +
					"url=" + s.rtspURL("/vod/audio0;seq=100;rtptime=800000")
			}
			return ""
		},
	}, 2)
	defer h.close()

	// Stats are hammered concurrently with reception, handovers and close.
	statsStop := make(chan struct{})
	var statsWG sync.WaitGroup
	statsWG.Add(1)
	go func() {
		defer statsWG.Done()
		for {
			select {
			case <-statsStop:
				return
			default:
				h.client.Stats()
			}
		}
	}()
	defer func() { close(statsStop); statsWG.Wait() }()

	_, err := h.client.Play(nil)
	require.NoError(t, err)

	// ordinary sequential reception on both medias (same payload type)
	h.server.send(0, false, epochRTP(0, 1000, 8000))
	p := h.waitPacket(1000)
	require.Equal(t, h.sd.Medias[0], p.media)

	h.server.send(1, false, epochRTP(0, 4000, 16000))
	p = h.waitPacket(4000)
	require.Equal(t, h.sd.Medias[1], p.media)

	h.server.send(0, false, epochRTP(0, 1001, 8160))
	h.waitPacket(1001)
	require.Zero(t, h.lost.Load())

	// PTS origin of the first epoch
	pts0, ok := h.client.PacketPTS(h.sd.Medias[0], &rtp.Packet{
		Header: rtp.Header{PayloadType: 0, Timestamp: 8000},
	})
	require.True(t, ok)
	require.Equal(t, int64(0), pts0)
	pts1, ok := h.client.PacketPTS(h.sd.Medias[0], &rtp.Packet{
		Header: rtp.Header{PayloadType: 0, Timestamp: 8160},
	})
	require.True(t, ok)
	require.Greater(t, pts1, pts0)

	// an epoch-1 sender report makes PacketNTP work
	srTime := time.Date(2024, 1, 1, 0, 0, 0, 0, time.UTC)
	h.server.send(0, true, epochSR(8160, srTime))
	require.Eventually(t, func() bool {
		_, ok := h.client.PacketNTP(h.sd.Medias[0], &rtp.Packet{
			Header: rtp.Header{PayloadType: 0, Timestamp: 8160},
		})
		return ok
	}, 5*time.Second, 10*time.Millisecond)

	// local SSRC and transport are negotiated once and must not change
	localSSRCBefore := h.formatStats(0).LocalSSRC
	require.NotZero(t, localSSRCBefore)
	require.NotNil(t, h.client.Transport())

	_, err = h.client.Pause()
	require.NoError(t, err)

	// seek: the server writes the PLAY response and the new-position frames
	// as one burst, while OnResponse is blocked.
	playDone := make(chan error, 1)
	go func() {
		_, e := h.client.Play(&headers.Range{Value: &headers.RangeNPT{Start: 30 * time.Second}})
		playDone <- e
	}()

	select {
	case <-h.responseSeen:
	case <-time.After(5 * time.Second):
		t.Fatal("PLAY response hook timeout")
	}

	// these frames follow the response but precede the epoch publication:
	// they must be retained, never delivered to the callback while held.
	h.server.send(0, false, epochRTP(0, 99, 799840)) // behind RTP-Info seq=100
	h.server.send(0, false, epochRTP(0, 100, 800000))
	h.server.send(0, false, epochRTP(0, 101, 800160))
	h.server.send(1, false, epochRTP(0, 400, 800000))
	h.assertNoPacket()

	h.release()
	require.NoError(t, <-playDone)

	// seq 99 was gated by RTP-Info and must never be delivered; the first
	// packets of the new position are delivered immediately and in order.
	h.waitPacket(100)
	h.waitPacket(101)
	h.waitPacket(400)
	for _, p := range h.drainPackets() {
		require.True(t, p.released, "no frame may reach the callback before OnResponse returns")
	}
	require.Zero(t, h.lost.Load(), "the backwards jump must not fabricate loss")

	// PTS restarts from the epoch's own origin
	newPTS, ok := h.client.PacketPTS(h.sd.Medias[0], &rtp.Packet{
		Header: rtp.Header{PayloadType: 0, Timestamp: 800000},
	})
	require.True(t, ok)
	require.Equal(t, int64(0), newPTS)

	// the old sender report mapping is gone until a new SR arrives
	_, ok = h.client.PacketNTP(h.sd.Medias[0], &rtp.Packet{
		Header: rtp.Header{PayloadType: 0, Timestamp: 8160},
	})
	require.False(t, ok)
	_, ok = h.client.PacketNTP(h.sd.Medias[0], &rtp.Packet{
		Header: rtp.Header{PayloadType: 0, Timestamp: 800000},
	})
	require.False(t, ok)

	newSRTime := time.Date(2025, 6, 1, 12, 0, 0, 0, time.UTC)
	h.server.send(0, true, epochSR(800000, newSRTime))
	require.Eventually(t, func() bool {
		nt, ok := h.client.PacketNTP(h.sd.Medias[0], &rtp.Packet{
			Header: rtp.Header{PayloadType: 0, Timestamp: 800000},
		})
		return ok && nt.Equal(newSRTime)
	}, 5*time.Second, 10*time.Millisecond)

	// statistics: cumulative counters grow, recent values describe the epoch
	stats := h.formatStats(0)
	require.Equal(t, uint64(4), stats.InboundRTPPackets, "received is cumulative") // 1000,1001,100,101
	require.Equal(t, uint16(101), stats.InboundRTPPacketsLastSequenceNumber)
	require.Equal(t, uint32(800160), stats.InboundRTPPacketsLastRTP)
	require.Equal(t, localSSRCBefore, stats.LocalSSRC)

	stats1 := h.formatStats(1)
	require.Equal(t, uint64(2), stats1.InboundRTPPackets) // 4000, 400
	require.Equal(t, uint16(400), stats1.InboundRTPPacketsLastSequenceNumber)

	// sequence number wraparound inside an epoch
	_, err = h.client.Pause()
	require.NoError(t, err)
	_, err = h.client.Play(nil)
	require.NoError(t, err)

	h.server.send(0, false, epochRTP(0, 65534, 900000))
	h.server.send(0, false, epochRTP(0, 65535, 900160))
	h.waitPacket(65534)
	h.waitPacket(65535)
	h.server.send(0, false, epochRTP(0, 0, 900320))
	h.waitPacket(0)
	require.Zero(t, h.lost.Load())
}

// TestClientReceiveEpochUDP exercises the handover over UDP, where without an
// epoch the new position would be treated as late duplicates and where stale
// reordering-buffer content must not be replayed in the new epoch.
func TestClientReceiveEpochUDP(t *testing.T) {
	h := newEpochClientHarness(t, ProtocolUDP, epochServerConfig{
		rtpInfo: func(s *epochServer, n int) string {
			if n == 1 {
				return "url=" + s.rtspURL("/vod/audio0;seq=300")
			}
			return ""
		},
	}, 1)
	defer h.close()

	_, err := h.client.Play(nil)
	require.NoError(t, err)

	// normal reception plus a reordered pair, which leaves 2002 in the
	// reordering buffer until 2001 arrives.
	h.server.send(0, false, epochRTP(0, 2000, 8000))
	h.waitPacket(2000)
	h.server.send(0, false, epochRTP(0, 2002, 8320))
	h.assertNoPacket()
	h.server.send(0, false, epochRTP(0, 2001, 8160))
	h.waitPacket(2001)
	h.waitPacket(2002)
	require.Zero(t, h.lost.Load())

	// put a packet in the reorder buffer, then seek before the gap is filled
	h.server.send(0, false, epochRTP(0, 2102, 10000))
	h.assertNoPacket()

	_, err = h.client.Pause()
	require.NoError(t, err)

	playDone := make(chan error, 1)
	go func() {
		_, e := h.client.Play(nil)
		playDone <- e
	}()

	select {
	case <-h.responseSeen:
	case <-time.After(5 * time.Second):
		t.Fatal("PLAY response hook timeout")
	}

	// UDP frames arriving while the response is in flight are dropped:
	// no callback, no statistics, no time mapping.
	h.server.send(0, false, epochRTP(0, 299, 799840))
	h.assertNoPacket()

	h.release()
	require.NoError(t, <-playDone)

	// the stale buffered packet 2102 of the old epoch is never replayed
	h.assertNoPacket()

	// RTP-Info gate: 299 is late, 300 establishes the new base immediately
	h.server.send(0, false, epochRTP(0, 299, 800000))
	h.assertNoPacket()
	h.server.send(0, false, epochRTP(0, 300, 800160))
	h.waitPacket(300)
	h.server.send(0, false, epochRTP(0, 301, 800320))
	h.waitPacket(301)
	require.Zero(t, h.lost.Load(), "no loss is fabricated from the backwards jump")

	// recent values describe only the new epoch, counters are cumulative
	stats := h.formatStats(0)
	require.Equal(t, uint16(301), stats.InboundRTPPacketsLastSequenceNumber)
	require.Equal(t, uint32(800320), stats.InboundRTPPacketsLastRTP)
	// delivered packets: 2000, 2001, 2002 of the first epoch and 300, 301 of
	// the second. The buffered 2102 never became a counted received packet.
	require.Equal(t, uint64(5), stats.InboundRTPPackets)
	require.Equal(t, uint64(0), stats.InboundRTPPacketsLost)
}

// TestClientReceiveEpochControl covers failed requests, malformed or
// ambiguous RTP-Info headers and the default-off behavior.
func TestClientReceiveEpochControl(t *testing.T) {
	t.Run("failed PLAY does not publish an epoch", func(t *testing.T) {
		var plays atomic.Int32
		h := newEpochClientHarness(t, ProtocolTCP, epochServerConfig{
			playStatus: func(n int) base.StatusCode {
				if plays.Add(1) == 2 {
					return base.StatusInternalServerError
				}
				return 0
			},
		}, 0)
		defer h.close()

		_, err := h.client.Play(nil)
		require.NoError(t, err)

		h.server.send(0, false, epochRTP(0, 1000, 8000))
		h.waitPacket(1000)

		_, err = h.client.Pause()
		require.NoError(t, err)

		// the second PLAY returns a non-200 status and must not publish an epoch
		_, err = h.client.Play(nil)
		require.Error(t, err)

		// the old reception state survived: the receiver still describes the
		// first epoch, and its cumulative counters were not reset.
		statsAfterFailure := h.formatStats(0)
		require.Equal(t, uint16(1000), statsAfterFailure.InboundRTPPacketsLastSequenceNumber)
		require.Equal(t, uint64(1), statsAfterFailure.InboundRTPPackets)
		require.NotNil(t, h.client.epochGate)

		// a successful retry publishes a fresh epoch: a backwards jump is
		// accepted immediately, without fabricated loss.
		_, err = h.client.Play(nil)
		require.NoError(t, err)
		h.server.send(0, false, epochRTP(0, 100, 800000))
		h.waitPacket(100)
		require.Zero(t, h.lost.Load())

		statsAfterRetry := h.formatStats(0)
		require.Equal(t, uint16(100), statsAfterRetry.InboundRTPPacketsLastSequenceNumber,
			"the new epoch restarts the recent sequence number")
		require.Equal(t, uint64(2), statsAfterRetry.InboundRTPPackets,
			"received stays cumulative across the failed and successful PLAY")
	})

	t.Run("failed PAUSE keeps the reception state", func(t *testing.T) {
		var pauses atomic.Int32
		h := newEpochClientHarness(t, ProtocolTCP, epochServerConfig{
			pauseStatus: func(n int) base.StatusCode {
				if pauses.Add(1) == 1 {
					return base.StatusInternalServerError
				}
				return 0
			},
		}, 0)
		defer h.close()

		_, err := h.client.Play(nil)
		require.NoError(t, err)
		h.server.send(0, false, epochRTP(0, 1000, 8000))
		h.waitPacket(1000)

		_, err = h.client.Pause()
		require.Error(t, err)

		// reception continues with the unchanged receiver
		h.server.send(0, false, epochRTP(0, 1001, 8160))
		h.waitPacket(1001)
		require.Zero(t, h.lost.Load())

		_, err = h.client.Pause()
		require.NoError(t, err)
		_, err = h.client.Play(nil)
		require.NoError(t, err)
		h.server.send(0, false, epochRTP(0, 100, 800000))
		h.waitPacket(100)
	})

	t.Run("malformed and ambiguous RTP-Info disables only the gate", func(t *testing.T) {
		var plays atomic.Int32
		h := newEpochClientHarness(t, ProtocolTCP, epochServerConfig{
			rtpInfo: func(s *epochServer, n int) string {
				plays.Add(1)
				switch n {
				case 1:
					// malformed header: the epoch still opens, without a gate
					return "url=" + s.rtspURL("/vod/audio0") + ";seq=not-a-number"

				case 2:
					// two entries match the same media: no gate for that media
					return "url=" + s.rtspURL("/vod/audio0") + ";seq=100," +
						"url=" + s.rtspURL("/vod/audio0") + ";seq=200"

				default:
					// absolute URL with userinfo: userinfo is ignored
					return "url=rtsp://user:pass@" + s.l.Addr().String() + "/vod/audio0;seq=300"
				}
			},
		}, 0)
		defer h.close()

		_, err := h.client.Play(nil)
		require.NoError(t, err)
		h.server.send(0, false, epochRTP(0, 1000, 8000))
		h.waitPacket(1000)

		_, err = h.client.Pause()
		require.NoError(t, err)
		_, err = h.client.Play(nil)
		require.NoError(t, err)
		// malformed gate: any first packet is accepted
		h.server.send(0, false, epochRTP(0, 50, 700000))
		h.waitPacket(50)

		_, err = h.client.Pause()
		require.NoError(t, err)
		_, err = h.client.Play(nil)
		require.NoError(t, err)
		// ambiguous gate: disabled, any first packet is accepted
		h.server.send(0, false, epochRTP(0, 150, 750000))
		h.waitPacket(150)

		_, err = h.client.Pause()
		require.NoError(t, err)
		_, err = h.client.Play(nil)
		require.NoError(t, err)
		// userinfo is stripped, so the gate is active and 299 is rejected
		h.server.send(0, false, epochRTP(0, 299, 799840))
		h.assertNoPacket()
		h.server.send(0, false, epochRTP(0, 300, 800000))
		h.waitPacket(300)
	})

	t.Run("option disabled keeps the legacy behavior", func(t *testing.T) {
		server, u := startEpochServer(t, epochServerConfig{})
		defer server.close()

		c := &Client{
			Scheme:   u.Scheme,
			Host:     u.Host,
			Protocol: new(ProtocolTCP),
		}
		require.NoError(t, c.Start())
		defer c.Close()

		sd, _, err := c.Describe(u)
		require.NoError(t, err)
		require.NoError(t, c.SetupAll(sd.BaseURL, sd.Medias))

		received := make(chan uint16, 16)
		c.OnPacketRTPAny(func(_ *description.Media, _ format.Format, pkt *rtp.Packet) {
			received <- pkt.SequenceNumber
		})

		_, err = c.Play(nil)
		require.NoError(t, err)
		server.send(0, false, epochRTP(0, 1001, 8160))
		select {
		case seq := <-received:
			require.Equal(t, uint16(1001), seq)
		case <-time.After(5 * time.Second):
			t.Fatal("packet timeout")
		}

		require.Nil(t, c.epochGate, "the gate must not be allocated when ResetOnPlay is false")

		_, err = c.Pause()
		require.NoError(t, err)
		_, err = c.Play(nil)
		require.NoError(t, err)

		// without an epoch, the backwards jump is counted as a huge loss
		var lost atomic.Uint64
		c.OnPacketsLost = func(n uint64) { lost.Add(n) }
		server.send(0, false, epochRTP(0, 100, 800000))
		select {
		case seq := <-received:
			require.Equal(t, uint16(100), seq)
		case <-time.After(5 * time.Second):
			t.Fatal("packet timeout")
		}
		require.Eventually(t, func() bool { return lost.Load() == 64634 },
			5*time.Second, 10*time.Millisecond)
	})
}
