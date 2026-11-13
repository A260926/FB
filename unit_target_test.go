package gos7

// Unit tests for the ISO-on-TCP layer: COTP fragmentation and reassembly,
// connection confirm negotiation and the send/receive association. Every
// test drives a real TCPClientHandler (and NewClient where jobs are
// involved) over a net.Pipe; the peer side is scripted per test and the
// telegrams on the wire are checked byte by byte.
import (
	"bytes"
	"encoding/binary"
	"errors"
	"net"
	"os"
	"sync/atomic"
	"testing"
	"time"

	"github.com/robinson/gos7/support/unitcheck"
)

// cotpParam builds one COTP TLV parameter.
func cotpParam(code byte, value ...byte) []byte {
	return append([]byte{code, byte(len(value))}, value...)
}

// tpktFrame wraps a COTP TPDU into a TPKT frame.
func tpktFrame(tpdu []byte) []byte {
	frame := make([]byte, 4, 4+len(tpdu))
	frame[0] = 3
	binary.BigEndian.PutUint16(frame[2:], uint16(4+len(tpdu)))
	return append(frame, tpdu...)
}

// connectionConfirm builds the CC answer to a connection request: the class
// 0 fixed part with the given destination reference, then the parameters in
// the given order.
func connectionConfirm(dstRef uint16, params ...[]byte) []byte {
	tpdu := []byte{0, 0xD0, byte(dstRef >> 8), byte(dstRef), 0, 7, 0}
	for _, p := range params {
		tpdu = append(tpdu, p...)
	}
	tpdu[0] = byte(len(tpdu) - 1)
	return tpktFrame(tpdu)
}

// dtFrame wraps S7 payload bytes into one DT TPKT frame; eot marks the last
// fragment of a TSDU.
func dtFrame(payload []byte, eot bool) []byte {
	marker := byte(0)
	if eot {
		marker = cotpEOT
	}
	return tpktFrame(append([]byte{2, cotpDataTPDU, marker}, payload...))
}

// pduNegotiationAnswer builds the S7 PDU negotiation response offering
// pduLength, echoing the PDU reference of the request payload.
func pduNegotiationAnswer(requestPayload []byte, pduLength uint16) []byte {
	answer := []byte{
		3, 0, 0, 27, 2, 0xF0, 0x80,
		0x32, 3, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0,
		0xF0, 0, 0, 1, 0, 1, 0, 0,
	}
	copy(answer[11:13], requestPayload[4:6])
	binary.BigEndian.PutUint16(answer[25:], pduLength)
	return answer
}

// s7Request builds a DT telegram with an S7 payload of payloadLen bytes
// carrying the given PDU reference.
func s7Request(payloadLen int, ref uint16) []byte {
	request := make([]byte, isoHSize+payloadLen)
	for i := isoHSize; i < len(request); i++ {
		request[i] = byte(i)
	}
	request[0] = 3
	binary.BigEndian.PutUint16(request[2:], uint16(len(request)))
	request[4], request[5], request[6] = 2, cotpDataTPDU, cotpEOT
	request[7] = s7ProtocolID
	request[8] = 1 // job
	binary.BigEndian.PutUint16(request[11:], ref)
	return request
}

// s7PayloadRef returns the PDU reference of an S7 payload (shell stripped).
func s7PayloadRef(payload []byte) uint16 {
	return binary.BigEndian.Uint16(payload[4:6])
}

// readTSDU reads DT frames from conn until the EOT flag and returns the raw
// frames and the reassembled user data.
func readTSDU(conn net.Conn) (frames [][]byte, payload []byte, err error) {
	for {
		frame, err := readTelegram(conn)
		if err != nil {
			return frames, payload, err
		}
		frames = append(frames, frame)
		payload = append(payload, frame[isoHSize:]...)
		if frame[6]&cotpEOT != 0 {
			return frames, payload, nil
		}
	}
}

// pipeHandler returns a handler talking to the client end of a fresh pipe
// and the peer end for the scripted PLC.
func pipeHandler(timeout time.Duration) (*TCPClientHandler, net.Conn) {
	clientEnd, plcEnd := net.Pipe()
	handler := &TCPClientHandler{}
	handler.conn = clientEnd
	handler.Timeout = timeout
	handler.IdleTimeout = 0
	return handler, plcEnd
}

// newNegHandler returns a handler for connection negotiation tests. Nothing
// is dialed; the tests assign the conn themselves.
func newNegHandler() *TCPClientHandler {
	handler := NewTCPClientHandler("127.0.0.1", 0, 2)
	handler.Timeout = 2 * time.Second
	handler.IdleTimeout = 0
	return handler
}

// isoConnectAgainst runs handler.isoConnect with a peer that reads the
// connection request and answers cc. It returns the request as seen on the
// wire.
func isoConnectAgainst(t *testing.T, handler *TCPClientHandler, cc []byte) ([]byte, error) {
	t.Helper()
	clientEnd, plcEnd := net.Pipe()
	defer clientEnd.Close()
	defer plcEnd.Close()
	handler.conn = clientEnd
	crSeen := make(chan []byte, 1)
	go func() {
		cr, err := readTelegram(plcEnd)
		if err != nil {
			return
		}
		crSeen <- cr
		_, _ = plcEnd.Write(cc)
	}()
	err := handler.isoConnect()
	handler.conn = nil
	return <-crSeen, err
}

// connectClient runs the ISO connect and the S7 PDU negotiation of a
// handler against a sessionPeer.
func connectClient(t *testing.T, handler *TCPClientHandler) {
	t.Helper()
	if err := handler.isoConnect(); err != nil {
		t.Fatalf("isoConnect: %v", err)
	}
	if err := handler.negotiatePduLength(); err != nil {
		t.Fatalf("negotiatePduLength: %v", err)
	}
}

// sessionPeer runs the connect handshake on the peer end: it answers the
// connection request with a CC built from ccParams and the S7 PDU
// negotiation offering pduLength, then runs job for the data phase.
func sessionPeer(plc net.Conn, ccParams [][]byte, pduLength uint16, job func(net.Conn)) {
	defer plc.Close()
	cr, err := readTelegram(plc)
	if err != nil {
		return
	}
	cc := connectionConfirm(binary.BigEndian.Uint16(cr[8:10]), ccParams...)
	if _, err := plc.Write(cc); err != nil {
		return
	}
	_, neg, err := readTSDU(plc)
	if err != nil {
		return
	}
	if _, err := plc.Write(pduNegotiationAnswer(neg, pduLength)); err != nil {
		return
	}
	job(plc)
}

// shortWriteConn writes at most max bytes per Write call, like a connection
// whose writes come back short.
type shortWriteConn struct {
	net.Conn
	max int
}

func (c *shortWriteConn) Write(p []byte) (int, error) {
	if len(p) > c.max {
		p = p[:c.max]
	}
	return c.Conn.Write(p)
}

// hookConn wraps a connection with custom write behavior and counts reads,
// to prove the receive phase is never entered after a write failure.
type hookConn struct {
	net.Conn
	reads *int32
	write func([]byte) (int, error)
}

func (c *hookConn) Read(p []byte) (int, error) {
	atomic.AddInt32(c.reads, 1)
	return c.Conn.Read(p)
}

func (c *hookConn) Write(p []byte) (int, error) { return c.write(p) }

func TestCOTPReceive(t *testing.T) {
	unitcheck.Run(t, func(c *unitcheck.C) {
		// A reply in a single DT fragment stays compatible: the same
		// reading as before fragmentation support.
		handler, plc := pipeHandler(2 * time.Second)
		defer plc.Close()
		defer handler.Close()
		handler.PDULength = 240
		go func() {
			_, payload, err := readTSDU(plc)
			if err != nil {
				return
			}
			plc.Write(readVarReply(s7PayloadRef(payload), []byte{0x11, 0x22, 0x33}))
		}()
		out := make([]byte, 3)
		err := NewClient(handler).AGReadDB(1, 0, 3, out)
		c.Check(t, err == nil, "AGReadDB: %v", err)
		c.Check(t, bytes.Equal(out, []byte{0x11, 0x22, 0x33}), "bytes=%x, want 112233", out)
	})

	t.Run("ShortReads", func(t *testing.T) {
		unitcheck.Run(t, func(c *unitcheck.C) {
			// The peer writes the reply one byte per TCP write; the
			// receiver must still read the full declared TPKT.
			handler, plc := pipeHandler(2 * time.Second)
			defer plc.Close()
			defer handler.Close()
			handler.PDULength = 240
			go func() {
				_, payload, err := readTSDU(plc)
				if err != nil {
					return
				}
				reply := readVarReply(s7PayloadRef(payload), []byte{0x11, 0x22, 0x33})
				for _, b := range reply {
					if _, err := plc.Write([]byte{b}); err != nil {
						return
					}
				}
			}()
			out := make([]byte, 3)
			err := NewClient(handler).AGReadDB(1, 0, 3, out)
			c.Check(t, err == nil, "AGReadDB: %v", err)
			c.Check(t, bytes.Equal(out, []byte{0x11, 0x22, 0x33}), "bytes=%x, want 112233", out)
		})
	})

	t.Run("FragmentsReassembled", func(t *testing.T) {
		unitcheck.Run(t, func(c *unitcheck.C) {
			// The reply arrives in three DT fragments, the first one
			// shorter than an S7 header; only the last sets EOT.
			handler, plc := pipeHandler(2 * time.Second)
			defer plc.Close()
			defer handler.Close()
			handler.PDULength = 240
			go func() {
				_, payload, err := readTSDU(plc)
				if err != nil {
					return
				}
				reply := readVarReply(s7PayloadRef(payload), []byte{0x11, 0x22, 0x33})
				s7 := reply[isoHSize:]
				plc.Write(dtFrame(s7[:5], false))
				plc.Write(dtFrame(s7[5:13], false))
				plc.Write(dtFrame(s7[13:], true))
			}()
			out := make([]byte, 3)
			err := NewClient(handler).AGReadDB(1, 0, 3, out)
			c.Check(t, err == nil, "AGReadDB: %v", err)
			c.Check(t, bytes.Equal(out, []byte{0x11, 0x22, 0x33}), "bytes=%x, want 112233", out)
		})
	})

	t.Run("NormalizedHeader", func(t *testing.T) {
		unitcheck.Run(t, func(c *unitcheck.C) {
			// What the parse layer gets is one telegram: a single TPKT
			// with 02 F0 80 and the logical total length.
			handler, plc := pipeHandler(2 * time.Second)
			defer plc.Close()
			defer handler.Close()
			handler.PDULength = 240
			request := append([]byte(nil), s7ReadWriteTelegram...)
			binary.BigEndian.PutUint16(request[s7PDURefOffset:], 0x1234)
			want := readVarReply(0x1234, []byte{0x11, 0x22, 0x33})
			go func() {
				if _, _, err := readTSDU(plc); err != nil {
					return
				}
				s7 := want[isoHSize:]
				plc.Write(dtFrame(s7[:9], false))
				plc.Write(dtFrame(s7[9:], true))
			}()
			response, err := handler.Send(request)
			c.Check(t, err == nil, "Send: %v", err)
			c.Check(t, bytes.Equal(response, want), "response % x, want % x", response, want)
		})
	})

	t.Run("KeepaliveSkipped", func(t *testing.T) {
		unitcheck.Run(t, func(c *unitcheck.C) {
			// Standalone empty DT telegrams are keepalives and skipped.
			handler, plc := pipeHandler(2 * time.Second)
			defer plc.Close()
			defer handler.Close()
			handler.PDULength = 240
			go func() {
				_, payload, err := readTSDU(plc)
				if err != nil {
					return
				}
				plc.Write([]byte{3, 0, 0, 7, 2, 0xF0, 0x80})
				plc.Write([]byte{3, 0, 0, 7, 2, 0xF0, 0x80})
				plc.Write(readVarReply(s7PayloadRef(payload), []byte{0x11, 0x22, 0x33}))
			}()
			out := make([]byte, 3)
			err := NewClient(handler).AGReadDB(1, 0, 3, out)
			c.Check(t, err == nil, "AGReadDB: %v", err)
			c.Check(t, bytes.Equal(out, []byte{0x11, 0x22, 0x33}), "bytes=%x, want 112233", out)
		})
	})

	t.Run("EmptyEOTEndsTSDU", func(t *testing.T) {
		unitcheck.Run(t, func(c *unitcheck.C) {
			// With data pending, an empty EOT fragment ends the TSDU.
			handler, plc := pipeHandler(2 * time.Second)
			defer plc.Close()
			defer handler.Close()
			handler.PDULength = 240
			go func() {
				_, payload, err := readTSDU(plc)
				if err != nil {
					return
				}
				reply := readVarReply(s7PayloadRef(payload), []byte{0x11, 0x22, 0x33})
				plc.Write(dtFrame(reply[isoHSize:], false))  // everything, no EOT yet
				plc.Write([]byte{3, 0, 0, 7, 2, 0xF0, 0x80}) // empty EOT ends it
			}()
			out := make([]byte, 3)
			err := NewClient(handler).AGReadDB(1, 0, 3, out)
			c.Check(t, err == nil, "AGReadDB: %v", err)
			c.Check(t, bytes.Equal(out, []byte{0x11, 0x22, 0x33}), "bytes=%x, want 112233", out)
		})
	})

	t.Run("EmptyNonEOTFragmentRejected", func(t *testing.T) {
		unitcheck.Run(t, func(c *unitcheck.C) {
			// With data pending, an empty fragment without EOT makes no
			// progress and is rejected; the connection is closed.
			handler, plc := pipeHandler(2 * time.Second)
			defer plc.Close()
			defer handler.Close()
			handler.PDULength = 240
			go func() {
				_, payload, err := readTSDU(plc)
				if err != nil {
					return
				}
				reply := readVarReply(s7PayloadRef(payload), []byte{0x11, 0x22, 0x33})
				plc.Write(dtFrame(reply[isoHSize:isoHSize+5], false))
				plc.Write([]byte{3, 0, 0, 7, 2, 0xF0, 0x00}) // empty, no EOT
			}()
			out := make([]byte, 3)
			err := NewClient(handler).AGReadDB(1, 0, 3, out)
			c.Check(t, err != nil, "want an error, got bytes=%x", out)
			c.Check(t, handler.conn == nil, "connection not closed after the bad fragment")
			c.Check(t, bytes.Equal(out, []byte{0, 0, 0}), "buffer touched: %x", out)
		})
	})

	t.Run("StaleFragmentsNotSpliced", func(t *testing.T) {
		unitcheck.Run(t, func(c *unitcheck.C) {
			// A late answer to an earlier request arrives fragmented,
			// followed by the fragmented current answer. The stale TSDU
			// is dropped as a whole; its fragments must not leak into
			// the current one.
			handler, plc := pipeHandler(2 * time.Second)
			defer plc.Close()
			defer handler.Close()
			handler.PDULength = 240
			want := pattern(79, 1)
			go func() {
				_, payload, err := readTSDU(plc)
				if err != nil {
					return
				}
				ref := s7PayloadRef(payload)
				stale := readVarReply(ref-1, pattern(6, 0x80))[isoHSize:]
				plc.Write(dtFrame(stale[:10], false))
				plc.Write(dtFrame(stale[10:], true))
				current := readVarReply(ref, want)[isoHSize:]
				plc.Write(dtFrame(current[:40], false))
				plc.Write(dtFrame(current[40:70], false))
				plc.Write(dtFrame(current[70:], true))
			}()
			got := make([]byte, 79)
			err := NewClient(handler).AGReadDB(1, 0, 79, got)
			c.Check(t, err == nil, "AGReadDB: %v", err)
			c.Check(t, bytes.Equal(got, want), "bytes=%x", got)
		})
	})

	t.Run("OversizeReassemblyRejected", func(t *testing.T) {
		unitcheck.Run(t, func(c *unitcheck.C) {
			// Every fragment is small enough on its own, but the
			// reassembled S7 payload outgrows the negotiated PDU
			// length: the total is what counts.
			handler, plc := pipeHandler(2 * time.Second)
			defer plc.Close()
			defer handler.Close()
			handler.PDULength = 40
			go func() {
				if _, _, err := readTSDU(plc); err != nil {
					return
				}
				plc.Write(dtFrame(bytes.Repeat([]byte{1}, 20), false))
				plc.Write(dtFrame(bytes.Repeat([]byte{2}, 20), false))
				plc.Write(dtFrame(bytes.Repeat([]byte{3}, 20), true))
			}()
			out := make([]byte, 3)
			err := NewClient(handler).AGReadDB(1, 0, 3, out)
			c.Check(t, err != nil, "want an error, got bytes=%x", out)
			c.Check(t, handler.conn == nil, "connection not closed after oversize TSDU")
			c.Check(t, bytes.Equal(out, []byte{0, 0, 0}), "buffer touched: %x", out)
		})
	})

	t.Run("FragmentExceedsNegotiatedTPDU", func(t *testing.T) {
		unitcheck.Run(t, func(c *unitcheck.C) {
			// A single fragment larger than the negotiated TPDU size is
			// rejected, however small the S7 total would stay.
			handler, plc := pipeHandler(2 * time.Second)
			defer plc.Close()
			defer handler.Close()
			handler.PDULength = 240
			handler.tpduSize = 128 // as negotiated
			go func() {
				if _, _, err := readTSDU(plc); err != nil {
					return
				}
				plc.Write(dtFrame(make([]byte, 150), true)) // TPDU of 153 > 128
			}()
			out := make([]byte, 3)
			err := NewClient(handler).AGReadDB(1, 0, 3, out)
			c.Check(t, err != nil, "want an error, got bytes=%x", out)
			c.Check(t, handler.conn == nil, "connection not closed after oversize fragment")
		})
	})

	t.Run("BadFragmentClosesConnection", func(t *testing.T) {
		unitcheck.Run(t, func(c *unitcheck.C) {
			cases := []struct {
				name string
				send func(plc net.Conn, s7 []byte)
			}{
				{"BadTPKTVersion", func(plc net.Conn, s7 []byte) {
					plc.Write(dtFrame(s7[:5], false))
					plc.Write([]byte{7, 0, 0, 10, 2, 0xF0, 0x80, 1, 2, 3})
				}},
				{"TPKTLengthTooSmall", func(plc net.Conn, s7 []byte) {
					plc.Write(dtFrame(s7[:5], false))
					plc.Write([]byte{3, 0, 0, 5, 2})
				}},
				{"TPKTLengthOverMax", func(plc net.Conn, s7 []byte) {
					plc.Write(dtFrame(s7[:5], false))
					plc.Write([]byte{3, 0, 0x10, 0x00})
				}},
				{"BadLengthIndicator", func(plc net.Conn, s7 []byte) {
					plc.Write(dtFrame(s7[:5], false))
					plc.Write([]byte{3, 0, 0, 10, 5, 0xF0, 0x80, 1, 2, 3})
				}},
				{"ControlMessageDuringAssembly", func(plc net.Conn, s7 []byte) {
					plc.Write(dtFrame(s7[:5], false))
					plc.Write(connectionConfirm(1))
				}},
				{"EOFMidFragment", func(plc net.Conn, s7 []byte) {
					plc.Write(dtFrame(s7[:5], false))
					plc.Write([]byte{3, 0, 0, 20, 2, 0xF0}) // declares 20, sends 6
					plc.Close()
				}},
			}
			for _, tc := range cases {
				t.Run(tc.name, func(t *testing.T) {
					handler, plc := pipeHandler(2 * time.Second)
					defer plc.Close()
					defer handler.Close()
					handler.PDULength = 240
					reply := readVarReply(1, []byte{0x11, 0x22, 0x33})
					go func() {
						if _, _, err := readTSDU(plc); err != nil {
							return
						}
						tc.send(plc, reply[isoHSize:])
					}()
					out := make([]byte, 3)
					err := NewClient(handler).AGReadDB(1, 0, 3, out)
					c.Check(t, err != nil, "%s: want an error, got bytes=%x", tc.name, out)
					c.Check(t, handler.conn == nil, "%s: connection not closed", tc.name)
					c.Check(t, bytes.Equal(out, []byte{0, 0, 0}), "%s: buffer touched: %x", tc.name, out)
				})
			}
		})
	})
}

func TestCOTPNegotiation(t *testing.T) {
	unitcheck.Run(t, func(c *unitcheck.C) {
		// A confirm without a TPDU-size parameter keeps the 1024 bytes
		// of the request, and the request on the wire is the template
		// with the handler's TSAPs.
		handler := newNegHandler()
		cr, err := isoConnectAgainst(t, handler, connectionConfirm(1,
			cotpParam(0xC1, 1, 0), cotpParam(0xC2, 1, 2)))
		c.Check(t, err == nil, "isoConnect: %v", err)
		c.Check(t, handler.tpduSize == cotpDefaultTPDUSize, "tpduSize=%d, want %d", handler.tpduSize, cotpDefaultTPDUSize)
		want := append([]byte(nil), isoConnectionRequestTelegram...)
		want[16], want[17], want[20], want[21] = 1, 0, 1, 2
		c.Check(t, bytes.Equal(cr, want), "connection request % x, want % x", cr, want)
	})

	t.Run("TPDUSize128And256", func(t *testing.T) {
		unitcheck.Run(t, func(c *unitcheck.C) {
			for _, tc := range []struct {
				exponent byte
				want     int
			}{{7, 128}, {8, 256}, {10, 1024}} {
				handler := newNegHandler()
				_, err := isoConnectAgainst(t, handler, connectionConfirm(1, cotpParam(cotpSizeParam, tc.exponent)))
				c.Check(t, err == nil, "exponent %d: isoConnect: %v", tc.exponent, err)
				c.Check(t, handler.tpduSize == tc.want, "exponent %d: tpduSize=%d, want %d", tc.exponent, handler.tpduSize, tc.want)
			}
		})
	})

	t.Run("ReorderedAndUnknownParametersAccepted", func(t *testing.T) {
		unitcheck.Run(t, func(c *unitcheck.C) {
			// Parameters in another order than the request, an unknown
			// parameter and a length that is not 22 bytes: all fine.
			cc := connectionConfirm(1,
				cotpParam(0x99, 0xAA, 0xBB, 0xCC), // unknown parameter
				cotpParam(0xC2, 1, 2),             // dst TSAP
				cotpParam(cotpSizeParam, 8),       // TPDU size after the TSAPs
				cotpParam(0xC1, 1, 0),             // src TSAP
			)
			handler := newNegHandler()
			_, err := isoConnectAgainst(t, handler, cc)
			c.Check(t, err == nil, "isoConnect: %v", err)
			c.Check(t, len(cc) != 22, "test CC should not be the old fixed 22 bytes")
			c.Check(t, handler.tpduSize == 256, "tpduSize=%d, want 256", handler.tpduSize)
		})
	})

	t.Run("FixedPartRejected", func(t *testing.T) {
		unitcheck.Run(t, func(c *unitcheck.C) {
			notCC := connectionConfirm(1)
			notCC[5] = 0xE0 // CR where the CC code belongs
			wrongClass := connectionConfirm(1)
			wrongClass[10] = 0x10 // transport class 1
			badLI := connectionConfirm(1)
			badLI[4] = 20 // length indicator does not match the TPDU
			truncated := tpktFrame([]byte{2, 0xD0, 0})
			cases := []struct {
				name string
				cc   []byte
			}{
				{"NotAConfirm", notCC},
				{"DstRefNotTheCRSourceRef", connectionConfirm(2)},
				{"NotClass0", wrongClass},
				{"LengthIndicatorMismatch", badLI},
				{"FixedPartTruncated", truncated},
			}
			for _, tc := range cases {
				handler := newNegHandler()
				_, err := isoConnectAgainst(t, handler, tc.cc)
				c.Check(t, err != nil, "%s: want a connect failure", tc.name)
			}
		})
	})

	t.Run("DuplicateTPDUSizeRejected", func(t *testing.T) {
		unitcheck.Run(t, func(c *unitcheck.C) {
			handler := newNegHandler()
			cc := connectionConfirm(1, cotpParam(cotpSizeParam, 7), cotpParam(cotpSizeParam, 8))
			_, err := isoConnectAgainst(t, handler, cc)
			c.Check(t, err != nil, "want a connect failure for a repeated C0")
		})
	})

	t.Run("BadTPDUSizeParameterRejected", func(t *testing.T) {
		unitcheck.Run(t, func(c *unitcheck.C) {
			cases := []struct {
				name  string
				param []byte
			}{
				{"LengthZero", cotpParam(cotpSizeParam)},
				{"LengthTwo", cotpParam(cotpSizeParam, 7, 8)},
				{"ExponentBelow128", cotpParam(cotpSizeParam, 6)},
				{"ExponentOver1024", cotpParam(cotpSizeParam, 11)},
			}
			for _, tc := range cases {
				handler := newNegHandler()
				_, err := isoConnectAgainst(t, handler, connectionConfirm(1, tc.param))
				c.Check(t, err != nil, "%s: want a connect failure", tc.name)
			}
		})
	})

	t.Run("TruncatedParametersRejected", func(t *testing.T) {
		unitcheck.Run(t, func(c *unitcheck.C) {
			overrun := []byte{0, 0xD0, 0, 1, 0, 7, 0, 0xC1, 2, 1} // says 2, holds 1
			overrun[0] = byte(len(overrun) - 1)
			dangling := []byte{0, 0xD0, 0, 1, 0, 7, 0, 0xC1} // code without length
			dangling[0] = byte(len(dangling) - 1)
			for _, tc := range []struct {
				name string
				cc   []byte
			}{
				{"ParameterLengthOverruns", tpktFrame(overrun)},
				{"DanglingParameterCode", tpktFrame(dangling)},
			} {
				handler := newNegHandler()
				_, err := isoConnectAgainst(t, handler, tc.cc)
				c.Check(t, err != nil, "%s: want a connect failure", tc.name)
			}
		})
	})

	t.Run("SmallLimitNotCarriedOver", func(t *testing.T) {
		unitcheck.Run(t, func(c *unitcheck.C) {
			handler := newNegHandler()
			_, err := isoConnectAgainst(t, handler, connectionConfirm(1, cotpParam(cotpSizeParam, 7)))
			c.Check(t, err == nil && handler.tpduSize == 128, "tpduSize=%d, err=%v", handler.tpduSize, err)
			// Closing the connection drops the negotiated limit.
			handler.Close()
			c.Check(t, handler.tpduSizeLimit() == cotpDefaultTPDUSize, "limit after close=%d", handler.tpduSizeLimit())
			// A failed negotiation does not revive the old limit either.
			_, err = isoConnectAgainst(t, handler, connectionConfirm(1, cotpParam(cotpSizeParam, 11)))
			c.Check(t, err != nil, "want a connect failure for exponent 11")
			c.Check(t, handler.tpduSize == cotpDefaultTPDUSize, "tpduSize=%d after failed negotiation", handler.tpduSize)
			// And a reconnect negotiates anew: no C0, back to 1024.
			handler.Close()
			_, err = isoConnectAgainst(t, handler, connectionConfirm(1))
			c.Check(t, err == nil, "reconnect: %v", err)
			c.Check(t, handler.tpduSize == cotpDefaultTPDUSize, "tpduSize=%d after reconnect", handler.tpduSize)
		})
	})
}

func TestCOTPAssociation(t *testing.T) {
	unitcheck.Run(t, func(c *unitcheck.C) {
		// A write whose S7 payload exceeds the negotiated 128 byte TPDU
		// crosses the wire as two DT fragments; the peer reassembles the
		// exact request and confirms the write.
		handler, plc := pipeHandler(2 * time.Second)
		defer plc.Close()
		defer handler.Close()
		var gotFrames [][]byte
		var gotS7 []byte
		go sessionPeer(plc, [][]byte{cotpParam(cotpSizeParam, 7)}, 480, func(conn net.Conn) {
			frames, payload, err := readTSDU(conn)
			if err != nil {
				return
			}
			gotFrames, gotS7 = frames, payload
			conn.Write(writeVarReply(s7PayloadRef(payload), 0xFF))
		})
		connectClient(t, handler)
		c.Check(t, handler.tpduSize == 128, "tpduSize=%d, want 128", handler.tpduSize)
		data := pattern(200, 1)
		err := NewClient(handler).AGWriteDB(1, 0, 200, data)
		c.Check(t, err == nil, "AGWriteDB: %v", err)
		c.Check(t, len(gotFrames) == 2, "frames=%d, want 2", len(gotFrames))
		if len(gotFrames) == 2 {
			c.Check(t, bytes.Equal(gotFrames[0][:isoHSize], []byte{3, 0, 0, 132, 2, 0xF0, 0}), "frame 1 head % x", gotFrames[0][:isoHSize])
			c.Check(t, bytes.Equal(gotFrames[1][:isoHSize], []byte{3, 0, 0, 110, 2, 0xF0, 0x80}), "frame 2 head % x", gotFrames[1][:isoHSize])
		}
		c.Check(t, len(gotS7) == 228, "reassembled S7 %d bytes, want 228", len(gotS7))
		if len(gotS7) == 228 {
			c.Check(t, gotS7[0] == s7ProtocolID && gotS7[10] == 5, "S7 head % x", gotS7[:11])
			c.Check(t, bytes.Equal(gotS7[28:], data), "written data not intact")
		}
	})

	t.Run("FragmentBoundaries128And256", func(t *testing.T) {
		unitcheck.Run(t, func(c *unitcheck.C) {
			cases := []struct {
				name     string
				exponent byte
				s7len    int
				frames   int
			}{
				{"128_exact", 7, 125, 1},
				{"128_one_over", 7, 126, 2},
				{"128_double_exact", 7, 250, 2},
				{"128_double_plus_one", 7, 251, 3},
				{"256_exact", 8, 253, 1},
				{"256_one_over", 8, 254, 2},
			}
			for _, tc := range cases {
				t.Run(tc.name, func(t *testing.T) {
					handler, plc := pipeHandler(2 * time.Second)
					defer plc.Close()
					defer handler.Close()
					var gotFrames [][]byte
					var gotS7 []byte
					go sessionPeer(plc, [][]byte{cotpParam(cotpSizeParam, tc.exponent)}, 480, func(conn net.Conn) {
						frames, payload, err := readTSDU(conn)
						if err != nil {
							return
						}
						gotFrames, gotS7 = frames, payload
						conn.Write(readVarReply(s7PayloadRef(payload), []byte{1}))
					})
					connectClient(t, handler)
					request := s7Request(tc.s7len, 0x0042)
					before := append([]byte(nil), request...)
					_, err := handler.Send(request)
					c.Check(t, err == nil, "Send: %v", err)
					c.Check(t, len(gotFrames) == tc.frames, "frames=%d, want %d", len(gotFrames), tc.frames)
					for i, f := range gotFrames {
						c.Check(t, len(f) >= isoHSize && len(f)-4 <= 1<<tc.exponent, "frame %d: TPDU of %d bytes exceeds %d", i, len(f)-4, 1<<tc.exponent)
						c.Check(t, f[0] == 3 && f[4] == 2 && f[5] == cotpDataTPDU, "frame %d header % x", i, f[:isoHSize])
						c.Check(t, int(binary.BigEndian.Uint16(f[2:])) == len(f), "frame %d: TPKT length field", i)
						c.Check(t, f[6]&0x7F == 0, "frame %d: low 7 bits not zero", i)
						if i == len(gotFrames)-1 {
							c.Check(t, f[6]&cotpEOT != 0, "frame %d: last fragment misses EOT", i)
						} else {
							c.Check(t, f[6]&cotpEOT == 0, "frame %d: EOT before the last fragment", i)
						}
					}
					c.Check(t, bytes.Equal(gotS7, before[isoHSize:]), "reassembled payload differs from the request")
					c.Check(t, bytes.Equal(request, before), "Send modified the request buffer")
				})
			}
		})
	})

	t.Run("ExactFitSendsNoEmptyTail", func(t *testing.T) {
		unitcheck.Run(t, func(c *unitcheck.C) {
			// 125 S7 bytes at a 128 byte TPDU fill the fragment exactly:
			// one frame, and no empty tail fragment behind it.
			handler, plc := pipeHandler(2 * time.Second)
			defer handler.Close()
			var gotFrames [][]byte
			var extra int
			done := make(chan struct{})
			go func() {
				defer close(done)
				defer plc.Close()
				cr, err := readTelegram(plc)
				if err != nil {
					return
				}
				if _, err := plc.Write(connectionConfirm(binary.BigEndian.Uint16(cr[8:10]), cotpParam(cotpSizeParam, 7))); err != nil {
					return
				}
				_, neg, err := readTSDU(plc)
				if err != nil {
					return
				}
				if _, err := plc.Write(pduNegotiationAnswer(neg, 480)); err != nil {
					return
				}
				frames, payload, err := readTSDU(plc)
				if err != nil {
					return
				}
				gotFrames = frames
				if _, err := plc.Write(readVarReply(s7PayloadRef(payload), []byte{1})); err != nil {
					return
				}
				// An empty tail fragment would arrive here.
				plc.SetReadDeadline(time.Now().Add(150 * time.Millisecond))
				extra, _ = plc.Read(make([]byte, 16))
			}()
			connectClient(t, handler)
			_, err := handler.Send(s7Request(125, 0x0042))
			c.Check(t, err == nil, "Send: %v", err)
			<-done
			c.Check(t, len(gotFrames) == 1, "frames=%d, want exactly 1", len(gotFrames))
			c.Check(t, extra == 0, "%d unexpected extra bytes after the exact-fit TSDU", extra)
		})
	})

	t.Run("ShortWritesDeliverEverything", func(t *testing.T) {
		unitcheck.Run(t, func(c *unitcheck.C) {
			// The connection writes at most 3 bytes per call; every
			// fragment must still be written completely.
			handler, plc := pipeHandler(2 * time.Second)
			defer plc.Close()
			defer handler.Close()
			handler.conn = &shortWriteConn{Conn: handler.conn, max: 3}
			var gotFrames [][]byte
			var gotS7 []byte
			go sessionPeer(plc, [][]byte{cotpParam(cotpSizeParam, 7)}, 480, func(conn net.Conn) {
				frames, payload, err := readTSDU(conn)
				if err != nil {
					return
				}
				gotFrames, gotS7 = frames, payload
				conn.Write(writeVarReply(s7PayloadRef(payload), 0xFF))
			})
			connectClient(t, handler)
			data := pattern(200, 1)
			err := NewClient(handler).AGWriteDB(1, 0, 200, data)
			c.Check(t, err == nil, "AGWriteDB: %v", err)
			c.Check(t, len(gotFrames) == 2, "frames=%d, want 2", len(gotFrames))
			c.Check(t, len(gotS7) == 228, "reassembled S7 %d bytes, want 228", len(gotS7))
			if len(gotS7) == 228 {
				c.Check(t, bytes.Equal(gotS7[28:], data), "written data not intact")
			}
		})
	})

	t.Run("ZeroProgressWriteFails", func(t *testing.T) {
		unitcheck.Run(t, func(c *unitcheck.C) {
			// A write that makes no progress is an error and the
			// receive phase is never entered.
			handler, plc := pipeHandler(time.Second)
			defer plc.Close()
			defer handler.Close()
			var reads int32
			handler.conn = &hookConn{Conn: handler.conn, reads: &reads, write: func([]byte) (int, error) { return 0, nil }}
			_, err := handler.Send(s7Request(24, 1))
			c.Check(t, err != nil, "want a write error")
			c.Check(t, atomic.LoadInt32(&reads) == 0, "entered the receive phase")
		})
	})

	t.Run("WriteFailureSkipsReceive", func(t *testing.T) {
		unitcheck.Run(t, func(c *unitcheck.C) {
			handler, plc := pipeHandler(time.Second)
			defer plc.Close()
			defer handler.Close()
			errWrite := errors.New("write blew up")
			var reads int32
			handler.conn = &hookConn{Conn: handler.conn, reads: &reads, write: func([]byte) (int, error) { return 0, errWrite }}
			_, err := handler.Send(s7Request(24, 1))
			c.Check(t, errors.Is(err, errWrite), "err=%v, want the write error", err)
			c.Check(t, atomic.LoadInt32(&reads) == 0, "entered the receive phase")
		})
	})

	t.Run("SingleDeadlineCoversAllFragments", func(t *testing.T) {
		unitcheck.Run(t, func(c *unitcheck.C) {
			// The fragments arrive each within 300ms of the previous
			// one, but the whole TSDU takes 400ms: the single 300ms
			// deadline of the Send expires mid-assembly.
			handler, plc := pipeHandler(300 * time.Millisecond)
			defer plc.Close()
			defer handler.Close()
			handler.PDULength = 240
			go func() {
				_, payload, err := readTSDU(plc)
				if err != nil {
					return
				}
				reply := readVarReply(s7PayloadRef(payload), []byte{0x11, 0x22, 0x33})
				s7 := reply[isoHSize:]
				plc.Write(dtFrame(s7[:8], false))
				time.Sleep(200 * time.Millisecond)
				plc.Write(dtFrame(s7[8:16], false))
				time.Sleep(200 * time.Millisecond)
				plc.Write(dtFrame(s7[16:], true))
			}()
			out := make([]byte, 3)
			start := time.Now()
			err := NewClient(handler).AGReadDB(1, 0, 3, out)
			elapsed := time.Since(start)
			c.Check(t, err != nil, "want a timeout, got bytes=%x", out)
			c.Check(t, errors.Is(err, os.ErrDeadlineExceeded), "err=%v, want a deadline error", err)
			c.Check(t, elapsed < 390*time.Millisecond, "took %v: the deadline was renewed per fragment", elapsed)
			c.Check(t, handler.conn == nil, "connection not closed after the timeout")
		})
	})

	t.Run("ReconnectAfterProtocolError", func(t *testing.T) {
		unitcheck.Run(t, func(c *unitcheck.C) {
			// A bad fragment kills the connection; an explicit
			// reconnect on the same handler works again.
			handler, plc := pipeHandler(2 * time.Second)
			handler.PDULength = 240
			go func() {
				defer plc.Close()
				_, payload, err := readTSDU(plc)
				if err != nil {
					return
				}
				reply := readVarReply(s7PayloadRef(payload), []byte{1, 2, 3})
				plc.Write(dtFrame(reply[isoHSize:isoHSize+5], false))
				plc.Write([]byte{9, 9, 9, 9}) // not a TPKT frame
			}()
			out := make([]byte, 3)
			err := NewClient(handler).AGReadDB(1, 0, 3, out)
			c.Check(t, err != nil, "want an error, got bytes=%x", out)
			c.Check(t, handler.conn == nil, "connection not closed after the bad fragment")

			clientEnd, plcEnd := net.Pipe()
			defer plcEnd.Close()
			defer handler.Close()
			handler.conn = clientEnd
			go sessionPeer(plcEnd, [][]byte{cotpParam(cotpSizeParam, 7)}, 480, func(conn net.Conn) {
				_, payload, err := readTSDU(conn)
				if err != nil {
					return
				}
				conn.Write(readVarReply(s7PayloadRef(payload), []byte{0x11, 0x22, 0x33}))
			})
			connectClient(t, handler)
			err = NewClient(handler).AGReadDB(1, 0, 3, out)
			c.Check(t, err == nil, "AGReadDB after reconnect: %v", err)
			c.Check(t, bytes.Equal(out, []byte{0x11, 0x22, 0x33}), "bytes=%x, want 112233", out)
			c.Check(t, handler.tpduSize == 128, "tpduSize=%d, want 128", handler.tpduSize)
		})
	})
}
