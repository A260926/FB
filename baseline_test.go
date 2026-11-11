package gortsplib

import (
	"bufio"
	"fmt"
	"net"
	"strings"
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

// This is a witness of the unmodified client's behavior, not a regression test
// for the requested new mode. It intentionally expects the old behavior.
func TestBaselineReceiveEpoch(t *testing.T) {
	for _, protocol := range []Protocol{ProtocolTCP, ProtocolUDP} {
		t.Run(protocol.String(), func(t *testing.T) {
			l, err := net.Listen("tcp", "127.0.0.1:0")
			require.NoError(t, err)
			defer l.Close()
			u, err := base.ParseURL("rtsp://" + l.Addr().String() + "/vod")
			require.NoError(t, err)
			udpRTP, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.ParseIP("127.0.0.1")})
			require.NoError(t, err)
			defer udpRTP.Close()
			udpRTCP, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.ParseIP("127.0.0.1")})
			require.NoError(t, err)
			defer udpRTCP.Close()

			type send struct {
				rtcp    bool
				payload []byte
				done    chan error
			}
			sends := make(chan send)
			serverDone := make(chan error, 1)
			go func() {
				serverDone <- func() error {
					nc, e := l.Accept()
					if e != nil {
						return e
					}
					defer nc.Close()
					nc.SetDeadline(time.Now().Add(10 * time.Second))
					co := conn.NewConn(bufio.NewReader(nc), nc)
					var ports *[2]int
					requests := make(chan *base.Request)
					readErr := make(chan error, 1)
					go func() {
						for {
							r, e := co.ReadRequest()
							if e != nil {
								readErr <- e
								return
							}
							requests <- r
						}
					}()
					for {
						select {
						case e := <-readErr:
							return e
						case s := <-sends:
							if protocol == ProtocolTCP {
								channel := 0
								if s.rtcp {
									channel = 1
								}
								e = co.WriteInterleavedFrame(&base.InterleavedFrame{Channel: channel, Payload: s.payload}, make([]byte, 2048))
							} else {
								sock, port := udpRTP, ports[0]
								if s.rtcp {
									sock, port = udpRTCP, ports[1]
								}
								_, e = sock.WriteToUDP(s.payload, &net.UDPAddr{IP: net.ParseIP("127.0.0.1"), Port: port})
							}
							s.done <- e
						case r := <-requests:
							h := base.Header{"CSeq": r.Header["CSeq"], "Session": {"local"}}
							var body []byte
							switch r.Method {
							case base.Options:
								h["Public"] = base.HeaderValue{"DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN"}
							case base.Describe:
								h["Content-Type"] = base.HeaderValue{"application/sdp"}
								h["Content-Base"] = base.HeaderValue{u.String() + "/"}
								body = []byte("v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=local\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\na=control:*\r\nm=audio 0 RTP/AVP 0\r\na=control:audio\r\n")
							case base.Setup:
								var tr headers.Transport
								if e = tr.Unmarshal(r.Header["Transport"]); e != nil {
									return e
								}
								ports = tr.ClientPorts
								tr.Delivery = new(headers.TransportDeliveryUnicast)
								if protocol == ProtocolUDP {
									tr.ServerPorts = &[2]int{udpRTP.LocalAddr().(*net.UDPAddr).Port, udpRTCP.LocalAddr().(*net.UDPAddr).Port}
								}
								h["Transport"] = tr.Marshal()
							case base.Play:
								h["Range"] = base.HeaderValue{"npt=30-"}
								h["RTP-Info"] = base.HeaderValue{"url=" + u.String() + "/audio;seq=100;rtptime=800000"}
							case base.Pause, base.Teardown:
							default:
								return fmt.Errorf("unexpected method %s", r.Method)
							}
							if e = co.WriteResponse(&base.Response{StatusCode: base.StatusOK, Header: h, Body: body}); e != nil {
								return e
							}
							if r.Method == base.Teardown {
								return nil
							}
						}
					}
				}()
			}()
			sendPacket := func(payload []byte, isRTCP bool) {
				done := make(chan error, 1)
				select {
				case sends <- send{isRTCP, payload, done}:
				case <-time.After(5 * time.Second):
					t.Fatal("server send timeout")
				}
				require.NoError(t, <-done)
			}
			pack := func(seq uint16, ts uint32, pt uint8) []byte {
				b, e := (&rtp.Packet{Header: rtp.Header{Version: 2, PayloadType: pt, SequenceNumber: seq, Timestamp: ts, SSRC: 1234}, Payload: []byte{0x55}}).Marshal()
				require.NoError(t, e)
				return b
			}
			received := make(chan uint16, 16)
			barrier := make(chan struct{}, 4)
			senderReport := make(chan struct{}, 4)
			var lost atomic.Uint64
			var playResponses atomic.Int32
			responseSeen := make(chan struct{})
			responseRelease := make(chan struct{})
			var releaseOnce sync.Once
			c := Client{Scheme: u.Scheme, Host: u.Host, Protocol: &protocol,
				OnResponse: func(r *base.Response) {
					if len(r.Header["RTP-Info"]) > 0 && playResponses.Add(1) == 2 {
						close(responseSeen)
						<-responseRelease
					}
				},
				OnPacketsLost: func(n uint64) { lost.Add(n) },
				OnDecodeError: func(e error) {
					if strings.Contains(e.Error(), "payload type") {
						barrier <- struct{}{}
					}
				},
			}
			require.NoError(t, c.Start())
			defer func() {
				c.Close()
				select {
				case e := <-serverDone:
					require.NoError(t, e)
				case <-time.After(5 * time.Second):
					t.Error("server did not finish")
				}
			}()
			defer releaseOnce.Do(func() { close(responseRelease) })
			sd, _, err := c.Describe(u)
			require.NoError(t, err)
			require.NoError(t, c.SetupAll(sd.BaseURL, sd.Medias))
			m := sd.Medias[0]
			c.OnPacketRTPAny(func(_ *description.Media, _ format.Format, p *rtp.Packet) { received <- p.SequenceNumber })
			c.OnPacketRTCPAny(func(_ *description.Media, p rtcp.Packet) {
				if _, ok := p.(*rtcp.SenderReport); ok {
					senderReport <- struct{}{}
				}
			})
			_, err = c.Play(nil)
			require.NoError(t, err)
			getPacket := func(want uint16) {
				select {
				case n := <-received:
					require.Equal(t, want, n)
				case <-time.After(5 * time.Second):
					t.Fatal("RTP callback timeout")
				}
			}
			sendPacket(pack(1000, 8000, 0), false)
			getPacket(1000)
			sendPacket(pack(1001, 8160, 0), false)
			getPacket(1001)
			require.Zero(t, lost.Load())
			sr, e := (&rtcp.SenderReport{SSRC: 1234, NTPTime: ntp.Encode(time.Date(2024, 1, 1, 0, 0, 0, 0, time.UTC)), RTPTime: 8160}).Marshal()
			require.NoError(t, e)
			sendPacket(sr, true)
			select {
			case <-senderReport:
			case <-time.After(5 * time.Second):
				t.Fatal("SR timeout")
			}
			_, ok := c.PacketNTP(m, &rtp.Packet{Header: rtp.Header{PayloadType: 0, Timestamp: 8160}})
			require.True(t, ok)
			t.Log("NORMAL sequential=1000,1001 lost=0 sender_report_mapping=true")
			_, err = c.Pause()
			require.NoError(t, err)
			playDone := make(chan error, 1)
			go func() {
				_, e := c.Play(&headers.Range{Value: &headers.RangeNPT{Start: 30 * time.Second}})
				playDone <- e
			}()
			select {
			case <-responseSeen:
			case <-time.After(5 * time.Second):
				t.Fatal("PLAY response hook timeout")
			}
			sendPacket(pack(100, 800000, 0), false)
			sendPacket(pack(101, 800160, 0), false)
			// Same RTP socket/stream, unknown PT is observed after both preceding
			// packets. This avoids a sleep-based guess about receiver processing.
			sendPacket(pack(102, 800320, 127), false)
			select {
			case <-barrier:
			case <-time.After(5 * time.Second):
				t.Fatal("RTP ordering barrier timeout")
			}
			if protocol == ProtocolTCP {
				getPacket(100)
				getPacket(101)
				require.Equal(t, uint64(64634), lost.Load())
				t.Log("WITNESS TCP resumed=100,101 fabricated_loss=64634")
			} else {
				require.Empty(t, received)
				require.Zero(t, lost.Load())
				t.Log("WITNESS UDP resumed=100,101 delivered=0")
			}
			t.Log("WITNESS RTP_processed_while_PLAY_response_hook_blocked=true")
			releaseOnce.Do(func() { close(responseRelease) })
			require.NoError(t, <-playDone)
			_, ok = c.PacketNTP(m, &rtp.Packet{Header: rtp.Header{PayloadType: 0, Timestamp: 800000}})
			require.True(t, ok)
			t.Log("WITNESS old_sender_report_mapping_survives_play=true")
		})
	}
}
