package gortmplib_test

import (
	"context"
	"crypto/rand"
	"crypto/rsa"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"fmt"
	"math/big"
	"net"
	"net/url"
	"strings"
	"testing"
	"time"

	"github.com/stretchr/testify/require"

	"github.com/bluenviron/gortmplib"
	"github.com/bluenviron/gortmplib/pkg/amf0"
	"github.com/bluenviron/gortmplib/pkg/bytecounter"
	"github.com/bluenviron/gortmplib/pkg/handshake"
	"github.com/bluenviron/gortmplib/pkg/message"
)

// TLS test material is generated locally instead of embedding a private key.
var serverCert, serverKey = func() ([]byte, []byte) {
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		panic(err)
	}
	template := &x509.Certificate{SerialNumber: big.NewInt(1), Subject: pkix.Name{CommonName: "localhost"}, DNSNames: []string{"localhost"}, NotBefore: time.Now().Add(-time.Hour), NotAfter: time.Now().Add(time.Hour), KeyUsage: x509.KeyUsageDigitalSignature | x509.KeyUsageKeyEncipherment, ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}}
	der, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
	if err != nil {
		panic(err)
	}
	return pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der}), pem.EncodeToMemory(&pem.Block{Type: "RSA PRIVATE KEY", Bytes: x509.MarshalPKCS1PrivateKey(key)})
}()

func queryDecode(enc string) map[string]string {
	// do not use url.ParseQuery since values are not URL-encoded
	vals := make(map[string]string)

	for kv := range strings.SplitSeq(enc, "&") {
		tmp := strings.SplitN(kv, "=", 2)
		if len(tmp) == 2 {
			vals[tmp[0]] = tmp[1]
		}
	}

	return vals
}

func TestClientURL(t *testing.T) {
	for _, ca := range []struct {
		name              string
		url               string
		expectedTcURL     string
		expectedApp       string
		expectedStreamKey string
	}{
		{
			name:          "one component",
			url:           "rtmp://localhost:9121/app",
			expectedTcURL: "rtmp://localhost:9121/app",
			expectedApp:   "app",
		},
		{
			name:          "one component with query",
			url:           "rtmp://localhost:9121/app?query=1",
			expectedTcURL: "rtmp://localhost:9121/app?query=1",
			expectedApp:   "app?query=1",
		},
		{
			name:          "two components",
			url:           "rtmp://localhost:9121/app/stream",
			expectedTcURL: "rtmp://localhost:9121/app/stream",
			expectedApp:   "app/stream",
		},
		{
			name:          "two components with query",
			url:           "rtmp://localhost:9121/app/stream?query=1",
			expectedTcURL: "rtmp://localhost:9121/app/stream?query=1",
			expectedApp:   "app/stream?query=1",
		},
		{
			name: "fragment",
			url: "rtmp://localhost:9121/three/segment/path?delaycc=0.1" +
				"#BigBuckBunny?audioLifetime=0.25&videoLifetime=2",
			expectedTcURL:     "rtmp://localhost:9121/three/segment/path?delaycc=0.1",
			expectedApp:       "three/segment/path?delaycc=0.1",
			expectedStreamKey: "BigBuckBunny?audioLifetime=0.25&videoLifetime=2",
		},
	} {
		t.Run(ca.name, func(t *testing.T) {
			ln, err := net.Listen("tcp", "127.0.0.1:9121")
			require.NoError(t, err)
			defer ln.Close()

			done := make(chan struct{})

			go func() {
				conn, err2 := ln.Accept()
				require.NoError(t, err2)
				defer conn.Close()
				bc := bytecounter.NewReadWriter(conn)

				_, _, err2 = handshake.DoServer(bc, false)
				require.NoError(t, err2)

				mrw := message.NewReadWriter(bc, bc, true)

				msg, err2 := mrw.Read()
				require.NoError(t, err2)
				require.Equal(t, &message.SetWindowAckSize{Value: 2500000}, msg)

				msg, err2 = mrw.Read()
				require.NoError(t, err2)
				require.Equal(t, &message.SetPeerBandwidth{Value: 2500000, Type: 2}, msg)

				msg, err2 = mrw.Read()
				require.NoError(t, err2)
				require.Equal(t, &message.SetChunkSize{Value: 65536}, msg)

				msg, err2 = mrw.Read()
				require.NoError(t, err2)
				connectMsg := msg.(*message.CommandAMF0)
				require.Equal(t, "connect", connectMsg.Name)
				obj := connectMsg.Arguments[0].(amf0.Object)
				actualTcURL, _ := obj.GetString("tcUrl")
				actualApp, _ := obj.GetString("app")
				require.Equal(t, ca.expectedTcURL, actualTcURL)
				require.Equal(t, ca.expectedApp, actualApp)

				err2 = mrw.Write(&message.CommandAMF0{
					ChunkStreamID: 3,
					Name:          "_result",
					CommandID:     1,
					Arguments: []any{
						amf0.Object{
							{Key: "fmsVer", Value: "LNX 9,0,124,2"},
							{Key: "capabilities", Value: float64(31)},
						},
						amf0.Object{
							{Key: "level", Value: "status"},
							{Key: "code", Value: "NetConnection.Connect.Success"},
							{Key: "description", Value: "Connection succeeded."},
							{Key: "objectEncoding", Value: float64(0)},
						},
					},
				})
				require.NoError(t, err2)

				msg, err2 = mrw.Read()
				require.NoError(t, err2)
				require.Equal(t, &message.CommandAMF0{
					ChunkStreamID: 3,
					Name:          "createStream",
					CommandID:     2,
					Arguments:     []any{nil},
				}, msg)

				err2 = mrw.Write(&message.CommandAMF0{
					ChunkStreamID: 3,
					Name:          "_result",
					CommandID:     2,
					Arguments:     []any{nil, float64(1)},
				})
				require.NoError(t, err2)

				msg, err2 = mrw.Read()
				require.NoError(t, err2)
				require.Equal(t, &message.UserControlSetBufferLength{BufferLength: 0x64}, msg)

				msg, err2 = mrw.Read()
				require.NoError(t, err2)
				playMsg := msg.(*message.CommandAMF0)
				require.Equal(t, "play", playMsg.Name)
				actualStreamKey := playMsg.Arguments[1].(string)
				require.Equal(t, ca.expectedStreamKey, actualStreamKey)

				err2 = mrw.Write(&message.CommandAMF0{
					ChunkStreamID:   5,
					MessageStreamID: 0x1000000,
					Name:            "onStatus",
					CommandID:       3,
					Arguments: []any{
						nil,
						amf0.Object{
							{Key: "level", Value: "status"},
							{Key: "code", Value: "NetStream.Play.Reset"},
							{Key: "description", Value: "play reset"},
						},
					},
				})
				require.NoError(t, err2)

				close(done)
			}()

			ur, err := url.Parse(ca.url)
			require.NoError(t, err)

			c := &gortmplib.Client{
				URL: ur,
			}
			err = c.Initialize(context.Background())
			require.NoError(t, err)
			defer c.Close()

			<-done
		})
	}
}

func TestClientReadPublish(t *testing.T) {
	for _, ca := range []string{
		"auth",
		"read",
		"read nginx rtmp",
		"read srs",
		"publish",
	} {
		t.Run(ca, func(t *testing.T) {
			ln, err := net.Listen("tcp", "127.0.0.1:9121")
			require.NoError(t, err)
			defer ln.Close()

			done := make(chan struct{})
			authState := 0

			go func() {
				for {
					conn, err2 := ln.Accept()
					require.NoError(t, err2)
					defer conn.Close()
					bc := bytecounter.NewReadWriter(conn)

					_, _, err2 = handshake.DoServer(bc, false)
					require.NoError(t, err2)

					mrw := message.NewReadWriter(bc, bc, true)

					msg, err2 := mrw.Read()
					require.NoError(t, err2)
					require.Equal(t, &message.SetWindowAckSize{
						Value: 2500000,
					}, msg)

					msg, err2 = mrw.Read()
					require.NoError(t, err2)
					require.Equal(t, &message.SetPeerBandwidth{
						Value: 2500000,
						Type:  2,
					}, msg)

					msg, err2 = mrw.Read()
					require.NoError(t, err2)
					require.Equal(t, &message.SetChunkSize{
						Value: 65536,
					}, msg)

					switch ca {
					case "auth":
						msg, err2 = mrw.Read()
						require.NoError(t, err2)

						switch authState {
						case 0: //nolint:dupl
							require.Equal(t, &message.CommandAMF0{ //nolint:dupl
								ChunkStreamID: 3,
								Name:          "connect",
								CommandID:     1,
								Arguments: []any{
									amf0.Object{
										{Key: "app", Value: "stream"},
										{Key: "flashVer", Value: "LNX 9,0,124,2"},
										{Key: "tcUrl", Value: "rtmp://127.0.0.1:9121/stream"},
										{Key: "objectEncoding", Value: float64(0)},
										{Key: "fpad", Value: false},
										{Key: "capabilities", Value: float64(15)},
										{Key: "audioCodecs", Value: float64(1413)},
										{Key: "videoCodecs", Value: float64(128)},
										{Key: "videoFunction", Value: float64(0)},
										{Key: "fourCcList", Value: amf0.StrictArray{
											"av01",
											"vp09",
											"hvc1",
											"avc1",
											"Opus",
											"fLaC",
											"ac-3",
											"mp4a",
											".mp3",
										}},
									},
								},
							}, msg)

						case 1: //nolint:dupl
							require.Equal(t, &message.CommandAMF0{ //nolint:dupl
								ChunkStreamID: 3,
								Name:          "connect",
								CommandID:     1,
								Arguments: []any{
									amf0.Object{
										{Key: "app", Value: "stream?authmod=adobe&user=myuser"},
										{Key: "flashVer", Value: "LNX 9,0,124,2"},
										{Key: "tcUrl", Value: "rtmp://127.0.0.1:9121/stream?authmod=adobe&user=myuser"},
										{Key: "objectEncoding", Value: float64(0)},
										{Key: "fpad", Value: false},
										{Key: "capabilities", Value: float64(15)},
										{Key: "audioCodecs", Value: float64(1413)},
										{Key: "videoCodecs", Value: float64(128)},
										{Key: "videoFunction", Value: float64(0)},
										{Key: "fourCcList", Value: amf0.StrictArray{
											"av01",
											"vp09",
											"hvc1",
											"avc1",
											"Opus",
											"fLaC",
											"ac-3",
											"mp4a",
											".mp3",
										}},
									},
								},
							}, msg)

						case 2:
							app, _ := msg.(*message.CommandAMF0).Arguments[0].(amf0.Object).GetString("app")
							query := queryDecode(app[len("stream?"):])
							clientChallenge := query["challenge"]
							response := authResponse("myuser", "mypass", "salt123", "", "server456challenge", clientChallenge)

							require.Equal(t, &message.CommandAMF0{
								ChunkStreamID: 3,
								Name:          "connect",
								CommandID:     1,
								Arguments: []any{
									amf0.Object{
										{
											Key: "app",
											Value: "stream?authmod=adobe&user=myuser&challenge=" +
												clientChallenge + "&response=" + response,
										},
										{Key: "flashVer", Value: "LNX 9,0,124,2"},
										{
											Key: "tcUrl",
											Value: "rtmp://127.0.0.1:9121/stream?authmod=adobe&user=myuser&challenge=" +
												clientChallenge + "&response=" + response,
										},
										{Key: "objectEncoding", Value: float64(0)},
										{Key: "fpad", Value: false},
										{Key: "capabilities", Value: float64(15)},
										{Key: "audioCodecs", Value: float64(1413)},
										{Key: "videoCodecs", Value: float64(128)},
										{Key: "videoFunction", Value: float64(0)},
										{Key: "fourCcList", Value: amf0.StrictArray{
											"av01",
											"vp09",
											"hvc1",
											"avc1",
											"Opus",
											"fLaC",
											"ac-3",
											"mp4a",
											".mp3",
										}},
									},
								},
							}, msg)
						}

					case "read", "read nginx rtmp", "read srs":
						msg, err2 = mrw.Read()
						require.NoError(t, err2)
						require.Equal(t, &message.CommandAMF0{ //nolint:dupl
							ChunkStreamID: 3,
							Name:          "connect",
							CommandID:     1,
							Arguments: []any{
								amf0.Object{
									{Key: "app", Value: "stream"},
									{Key: "flashVer", Value: "LNX 9,0,124,2"},
									{Key: "tcUrl", Value: "rtmp://127.0.0.1:9121/stream"},
									{Key: "objectEncoding", Value: float64(0)},
									{Key: "fpad", Value: false},
									{Key: "capabilities", Value: float64(15)},
									{Key: "audioCodecs", Value: float64(1413)},
									{Key: "videoCodecs", Value: float64(128)},
									{Key: "videoFunction", Value: float64(0)},
									{Key: "fourCcList", Value: amf0.StrictArray{
										"av01",
										"vp09",
										"hvc1",
										"avc1",
										"Opus",
										"fLaC",
										"ac-3",
										"mp4a",
										".mp3",
									}},
								},
							},
						}, msg)

					case "publish":
						msg, err2 = mrw.Read()
						require.NoError(t, err2)
						require.Equal(t, &message.CommandAMF0{
							ChunkStreamID: 3,
							Name:          "connect",
							CommandID:     1,
							Arguments: []any{
								amf0.Object{
									{Key: "app", Value: "stream"},
									{Key: "flashVer", Value: "LNX 9,0,124,2"},
									{Key: "tcUrl", Value: "rtmp://127.0.0.1:9121/stream"},
									{Key: "objectEncoding", Value: float64(0)},
								},
							},
						}, msg)
					}

					if ca == "auth" {
						switch authState {
						case 0:
							err2 = mrw.Write(&message.CommandAMF0{
								ChunkStreamID: 3,
								Name:          "_error",
								CommandID:     1,
								Arguments: []any{
									nil,
									amf0.Object{
										{Key: "level", Value: "error"},
										{Key: "code", Value: "NetConnection.Connect.Rejected"},
										{Key: "description", Value: "code=403 need auth; authmod=adobe"},
									},
								},
							})
							require.NoError(t, err2)

							authState++
							continue

						case 1:
							err2 = mrw.Write(&message.CommandAMF0{
								ChunkStreamID: 3,
								Name:          "_error",
								CommandID:     1,
								Arguments: []any{
									nil,
									amf0.Object{
										{Key: "level", Value: "error"},
										{Key: "code", Value: "NetConnection.Connect.Rejected"},
										{
											Key:   "description",
											Value: "authmod=adobe ?reason=needauth&user=myuser&salt=salt123&challenge=server456challenge",
										},
									},
								},
							})
							require.NoError(t, err2)

							authState++
							continue
						}
					}

					err2 = mrw.Write(&message.CommandAMF0{
						ChunkStreamID: 3,
						Name:          "_result",
						CommandID:     1,
						Arguments: []any{
							amf0.Object{
								{Key: "fmsVer", Value: "LNX 9,0,124,2"},
								{Key: "capabilities", Value: float64(31)},
							},
							amf0.Object{
								{Key: "level", Value: "status"},
								{Key: "code", Value: "NetConnection.Connect.Success"},
								{Key: "description", Value: "Connection succeeded."},
								{Key: "objectEncoding", Value: float64(0)},
							},
						},
					})
					require.NoError(t, err2)

					switch ca {
					case "auth", "read", "read nginx rtmp", "read srs":
						msg, err2 = mrw.Read()
						require.NoError(t, err2)
						require.Equal(t, &message.CommandAMF0{
							ChunkStreamID: 3,
							Name:          "createStream",
							CommandID:     2,
							Arguments: []any{
								nil,
							},
						}, msg)

						if ca == "read srs" {
							err2 = mrw.Write(&message.CommandAMF0{
								ChunkStreamID: 3,
								Name:          "onBWDone",
								CommandID:     0,
								Arguments: []any{
									nil,
								},
							})
							require.NoError(t, err2)
						}

						err2 = mrw.Write(&message.CommandAMF0{
							ChunkStreamID: 3,
							Name:          "_result",
							CommandID:     2,
							Arguments: []any{
								nil,
								float64(1),
							},
						})
						require.NoError(t, err2)

						msg, err2 = mrw.Read()
						require.NoError(t, err2)
						require.Equal(t, &message.UserControlSetBufferLength{
							BufferLength: 0x64,
						}, msg)

						msg, err2 = mrw.Read()
						require.NoError(t, err2)
						require.Equal(t, &message.CommandAMF0{
							ChunkStreamID:   4,
							MessageStreamID: 0x1000000,
							Name:            "play",
							CommandID:       3,
							Arguments: []any{
								nil,
								"",
							},
						}, msg)

						err2 = mrw.Write(&message.CommandAMF0{
							ChunkStreamID:   5,
							MessageStreamID: 0x1000000,
							Name:            "onStatus",
							CommandID: func() int {
								if ca == "read nginx rtmp" {
									return 0
								}
								return 3
							}(),
							Arguments: []any{
								nil,
								amf0.Object{
									{Key: "level", Value: "status"},
									{Key: "code", Value: "NetStream.Play.Reset"},
									{Key: "description", Value: "play reset"},
								},
							},
						})
						require.NoError(t, err2)

					case "publish":
						msg, err2 = mrw.Read()
						require.NoError(t, err2)
						require.Equal(t, &message.CommandAMF0{
							ChunkStreamID: 3,
							Name:          "releaseStream",
							CommandID:     2,
							Arguments: []any{
								nil,
								"",
							},
						}, msg)

						msg, err2 = mrw.Read()
						require.NoError(t, err2)
						require.Equal(t, &message.CommandAMF0{
							ChunkStreamID: 3,
							Name:          "FCPublish",
							CommandID:     3,
							Arguments: []any{
								nil,
								"",
							},
						}, msg)

						msg, err2 = mrw.Read()
						require.NoError(t, err2)
						require.Equal(t, &message.CommandAMF0{
							ChunkStreamID: 3,
							Name:          "createStream",
							CommandID:     4,
							Arguments: []any{
								nil,
							},
						}, msg)

						err2 = mrw.Write(&message.CommandAMF0{
							ChunkStreamID: 3,
							Name:          "_result",
							CommandID:     4,
							Arguments: []any{
								nil,
								float64(1),
							},
						})
						require.NoError(t, err2)

						msg, err2 = mrw.Read()
						require.NoError(t, err2)
						require.Equal(t, &message.CommandAMF0{
							ChunkStreamID:   4,
							MessageStreamID: 0x1000000,
							Name:            "publish",
							CommandID:       5,
							Arguments: []any{
								nil,
								"",
								"live",
							},
						}, msg)

						err2 = mrw.Write(&message.CommandAMF0{
							ChunkStreamID:   5,
							MessageStreamID: 0x1000000,
							Name:            "onStatus",
							CommandID:       5,
							Arguments: []any{
								nil,
								amf0.Object{
									{Key: "level", Value: "status"},
									{Key: "code", Value: "NetStream.Publish.Start"},
									{Key: "description", Value: "publish start"},
								},
							},
						})
						require.NoError(t, err2)
					}

					close(done)
					break
				}
			}()

			var rawURL string

			if ca == "auth" {
				rawURL = "rtmp://myuser:mypass@127.0.0.1:9121/stream"
			} else {
				rawURL = "rtmp://127.0.0.1:9121/stream"
			}

			u, err := url.Parse(rawURL)
			require.NoError(t, err)

			c := &gortmplib.Client{
				URL:     u,
				Publish: (ca == "publish"),
			}
			err = c.Initialize(context.Background())
			require.NoError(t, err)
			defer c.Close()

			switch ca {
			case "read", "read nginx rtmp":
				require.Equal(t, uint64(3421), c.BytesReceived())
				require.Equal(t, uint64(0xdba), c.BytesSent())

			case "read srs":
				require.Equal(t, uint64(0xd7a), c.BytesReceived())
				require.Equal(t, uint64(0xdba), c.BytesSent())

			case "publish":
				require.Equal(t, uint64(3427), c.BytesReceived())
				require.Equal(t, uint64(0xd3e), c.BytesSent())
			}

			<-done
		})
	}
}

func TestClientRTMPS(t *testing.T) {
	cert, err := tls.X509KeyPair(serverCert, serverKey)
	require.NoError(t, err)

	l, err := tls.Listen("tcp", "localhost:1936", &tls.Config{
		Certificates: []tls.Certificate{cert},
		VerifyConnection: func(cs tls.ConnectionState) error {
			// check that SNI is correctly filled by client
			require.Equal(t, "localhost", cs.ServerName)
			return nil
		},
	})
	require.NoError(t, err)
	defer l.Close()

	serverDone := make(chan struct{})
	defer func() { <-serverDone }()

	go func() {
		defer close(serverDone)

		nconn, err2 := l.Accept()
		require.NoError(t, err2)
		defer nconn.Close()
		bc := bytecounter.NewReadWriter(nconn)

		_, _, err2 = handshake.DoServer(bc, false)
		require.NoError(t, err2)

		mrw := message.NewReadWriter(bc, bc, true)

		for {
			var msg message.Message
			msg, err = mrw.Read()
			require.NoError(t, err)

			if msg, ok := msg.(*message.CommandAMF0); ok && msg.Name == "connect" {
				break
			}
		}

		err2 = mrw.Write(&message.CommandAMF0{
			ChunkStreamID: 3,
			Name:          "_result",
			CommandID:     1,
			Arguments: []any{
				amf0.Object{
					{Key: "fmsVer", Value: "LNX 9,0,124,2"},
					{Key: "capabilities", Value: float64(31)},
				},
				amf0.Object{
					{Key: "level", Value: "status"},
					{Key: "code", Value: "NetConnection.Connect.Success"},
					{Key: "description", Value: "Connection succeeded."},
					{Key: "objectEncoding", Value: float64(0)},
				},
			},
		})
		require.NoError(t, err2)

		for {
			var msg message.Message
			msg, err = mrw.Read()
			require.NoError(t, err)

			if msg, ok := msg.(*message.CommandAMF0); ok && msg.Name == "createStream" {
				break
			}
		}

		err2 = mrw.Write(&message.CommandAMF0{
			ChunkStreamID: 3,
			Name:          "_result",
			CommandID:     4,
			Arguments: []any{
				nil,
				float64(1),
			},
		})
		require.NoError(t, err2)

		for {
			var msg message.Message
			msg, err = mrw.Read()
			require.NoError(t, err)

			if msg, ok := msg.(*message.CommandAMF0); ok && msg.Name == "publish" {
				break
			}
		}

		err2 = mrw.Write(&message.CommandAMF0{
			ChunkStreamID:   5,
			MessageStreamID: 0x1000000,
			Name:            "onStatus",
			CommandID:       5,
			Arguments: []any{
				nil,
				amf0.Object{
					{Key: "level", Value: "status"},
					{Key: "code", Value: "NetStream.Publish.Start"},
					{Key: "description", Value: "publish start"},
				},
			},
		})
		require.NoError(t, err2)
	}()

	u, err := url.Parse("rtmps://localhost:1936/test")
	require.NoError(t, err)

	c := &gortmplib.Client{
		URL:     u,
		Publish: true,
		TLSConfig: &tls.Config{
			InsecureSkipVerify: true,
		},
	}
	err = c.Initialize(context.Background())
	require.NoError(t, err)
	defer c.Close()
}

func TestClientDefaultPort(t *testing.T) {
	for _, ca := range []struct {
		name         string
		url          string
		expectedAddr string
	}{
		{
			name:         "rtmp",
			url:          "rtmp://localhost/app",
			expectedAddr: "localhost:1935",
		},
		{
			name:         "rtmps",
			url:          "rtmps://localhost/app",
			expectedAddr: "localhost:443",
		},
	} {
		t.Run(ca.name, func(t *testing.T) {
			var dialedAddr string

			ur, err := url.Parse(ca.url)
			require.NoError(t, err)

			c := &gortmplib.Client{
				URL: ur,
				DialContext: func(_ context.Context, _, address string) (net.Conn, error) {
					dialedAddr = address
					return nil, fmt.Errorf("dial intercepted")
				},
			}
			err = c.Initialize(context.Background())
			require.Error(t, err)
			require.Equal(t, ca.expectedAddr, dialedAddr)
		})
	}
}
