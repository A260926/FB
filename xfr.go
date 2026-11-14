package dns

import (
	"crypto/tls"
	"errors"
	"fmt"
	"io"
	"time"
)

// Envelope is used when doing a zone transfer with a remote server.
type Envelope struct {
	RR    []RR  // The set of RRs in the answer section of the xfr reply message.
	Error error // If something went wrong, this contains the error.
}

// A Transfer defines parameters that are used during a zone transfer.
type Transfer struct {
	*Conn
	DialTimeout    time.Duration     // net.DialTimeout, defaults to 2 seconds
	ReadTimeout    time.Duration     // net.Conn.SetReadTimeout value for connections, defaults to 2 seconds
	WriteTimeout   time.Duration     // net.Conn.SetWriteTimeout value for connections, defaults to 2 seconds
	TsigProvider   TsigProvider      // An implementation of the TsigProvider interface. If defined it replaces TsigSecret and is used for all TSIG operations.
	TsigSecret     map[string]string // Secret(s) for Tsig map[<zonename>]<base64 secret>, zonename must be in canonical form (lowercase, fqdn, see RFC 4034 Section 6.2)
	tsigTimersOnly bool
	TLS            *tls.Config // TLS config. If Xfr over TLS will be attempted
}

func (t *Transfer) tsigProvider() TsigProvider {
	if t.TsigProvider != nil {
		return t.TsigProvider
	}
	if t.TsigSecret != nil {
		return tsigSecretProvider(t.TsigSecret)
	}
	return nil
}

// TODO: Think we need to away to stop the transfer

// In performs an incoming transfer with the server in a.
// If you would like to set the source IP, or some other attribute
// of a Dialer for a Transfer, you can do so by specifying the attributes
// in the Transfer.Conn:
//
//	d := net.Dialer{LocalAddr: transfer_source}
//	con, err := d.Dial("tcp", master)
//	dnscon := &dns.Conn{Conn:con}
//	transfer = &dns.Transfer{Conn: dnscon}
//	channel, err := transfer.In(message, master)
//
// The Envelopes are still delivered one per received reply message, but the
// point at which an IXFR stream is considered complete is decided by the same
// record-sequence grammar as InZone: serial numbers use RFC 1982 arithmetic,
// AXFR fallbacks and multi-group incremental sequences are recognized across
// message boundaries, and a single-SOA no-change answer ends the transfer.
func (t *Transfer) In(q *Msg, a string) (env chan *Envelope, err error) {
	switch q.Question[0].Qtype {
	case TypeAXFR, TypeIXFR:
	default:
		return nil, &Error{"unsupported question type"}
	}

	if err := t.dialAndWrite(q, a); err != nil {
		return nil, err
	}

	env = make(chan *Envelope)
	go func() {
		defer func() {
			// First close the connection, then the channel. This allows functions
			// blocked on the channel to assume that the connection is closed and no
			// further operations are pending when they resume.
			t.Close()
			close(env)
		}()
		g := newXfrGrammar(q.Question[0].Qtype, q.Question[0].Name, querySOA(q), false, noopXfrApplier{})
		t.runTransfer(q, g, func(e *Envelope) { env <- e })
	}()

	return env, nil
}

// InZone performs an incoming AXFR or IXFR with the server in a and returns
// the complete, independently owned snapshot of the zone.
//
// q must carry exactly one class IN question, of type TypeAXFR or TypeIXFR.
// The address a is interpreted exactly as for In.
//
// For an AXFR, base is ignored and the answer must be a full zone bracketed by
// identical apex SOA records. For an IXFR, base must be the complete current
// snapshot of the zone: it must contain exactly one apex SOA, whose serial is
// the serial given in the authority SOA of q, and otherwise only in-zone class
// IN records with no duplicate identities and no RRsets carrying conflicting
// TTLs. A single-SOA answer whose serial is equal to or older than the local
// serial yields an independent copy of base; the server's older SOA never
// replaces the local one. An AXFR fallback inside an IXFR answer replaces the
// whole snapshot instead of being applied on top of base.
//
// On success the returned slice and the record objects in it are owned by the
// caller: the single apex SOA is placed first, later records have no
// guaranteed order, and mutating q, base or the returned records never affects
// the other side. Neither q nor base nor their mutable RDATA is modified. On
// failure InZone returns nil and an error describing the cause, and no partial
// snapshot is published.
//
// As with In, the connection is closed before InZone returns. InZone does not
// add persistence, automatic retries or DNSSEC validation.
func (t *Transfer) InZone(q *Msg, a string, base []RR) ([]RR, error) {
	qtype, apex, nsSOA, err := validateZoneQuery(q)
	if err != nil {
		return nil, err
	}

	query := q.Copy()
	var app *zoneApplier
	var expected *SOA
	strict := false
	if qtype == TypeIXFR {
		app, expected, err = validateBase(apex, base)
		if err != nil {
			return nil, err
		}
		if expected.Serial != nsSOA.Serial {
			return nil, &Error{err: fmt.Sprintf("xfr: IXFR query serial %d does not match the base snapshot serial %d", nsSOA.Serial, expected.Serial)}
		}
		strict = true
	} else {
		app = newZoneApplier(apex)
	}

	if err := t.dialAndWrite(query, a); err != nil {
		return nil, err
	}
	defer t.Close()

	g := newXfrGrammar(qtype, apex, expected, strict, app)
	if err := t.runTransfer(query, g, nil); err != nil {
		return nil, err
	}

	if qtype == TypeIXFR && g.noChange() {
		// A single-SOA answer is only a no-change when the server's serial is
		// equal to or behind the local one. A newer serial with no delta is an
		// incomplete answer, and an incomparable serial is an error.
		rel, err := serialRelation(g.lead.Serial, expected.Serial)
		if err != nil {
			return nil, err
		}
		if rel == 1 {
			return nil, &Error{err: "xfr: single-SOA IXFR answer announces a newer serial without any changes"}
		}
		// Keep the local snapshot (never the server's stale SOA). The applier
		// holds an independent, validated copy of base with the SOA first.
		return app.snapshot()
	}
	return app.snapshot()
}

// dialAndWrite establishes the transfer connection when none was supplied on
// t.Conn and sends the query.
func (t *Transfer) dialAndWrite(q *Msg, a string) error {
	timeout := dnsTimeout
	if t.DialTimeout != 0 {
		timeout = t.DialTimeout
	}

	var err error
	if t.Conn == nil {
		if t.TLS != nil {
			t.Conn, err = DialTimeoutWithTLS("tcp-tls", a, t.TLS, timeout)
		} else {
			t.Conn, err = DialTimeout("tcp", a, timeout)
		}
		if err != nil {
			return err
		}
	}

	return t.WriteMsg(q)
}

// runTransfer reads reply messages until the transfer grammar reaches a legal
// end. deliver, when non-nil, receives one Envelope per reply message exactly
// as the records arrived; transport and protocol errors are delivered through
// it as well. The connection itself is not closed here.
func (t *Transfer) runTransfer(q *Msg, g *xfrGrammar, deliver func(*Envelope)) error {
	timeout := dnsTimeout
	if t.ReadTimeout != 0 {
		timeout = t.ReadTimeout
	}

	for {
		t.Conn.SetReadDeadline(time.Now().Add(timeout))
		in, err := t.ReadMsg()
		if err != nil {
			// A clean connection close (io.EOF for TCP, io.ErrClosedPipe for
			// net.Pipe) ends the transfer only where the grammar permits it.
			if (errors.Is(err, io.EOF) || errors.Is(err, io.ErrClosedPipe)) && g.acceptEOF() {
				return nil
			}
			return t.fail(deliver, nil, err)
		}
		if q.Id != in.Id {
			return t.fail(deliver, in.Answer, ErrId)
		}
		if in.Rcode != RcodeSuccess {
			return t.fail(deliver, in.Answer, &Error{err: fmt.Sprintf(errXFR, in.Rcode)})
		}

		for _, rr := range in.Answer {
			if err := g.step(rr); err != nil {
				return t.fail(deliver, in.Answer, err)
			}
		}
		if g.state == gStart {
			// The opening message carried no record at all; the answer must
			// begin with an apex SOA.
			return t.fail(deliver, in.Answer, ErrSoa)
		}
		// Only the opening message seeds the TSIG timing state; later
		// messages in the same transfer reuse it.
		t.tsigTimersOnly = true

		if deliver != nil {
			deliver(&Envelope{in.Answer, nil})
		}
		if g.ended() || g.noChangeConfirmed() {
			return nil
		}
	}
}

// fail reports a transfer error, delivering the offending message's records
// alongside the error to envelope consumers, and returns the error.
func (t *Transfer) fail(deliver func(*Envelope), rr []RR, err error) error {
	if deliver != nil {
		deliver(&Envelope{rr, err})
	}
	return err
}

// querySOA returns the authority SOA carried by an IXFR query, or nil when the
// query does not provide a usable one.
func querySOA(q *Msg) *SOA {
	if len(q.Ns) == 0 {
		return nil
	}
	soa, ok := q.Ns[0].(*SOA)
	if !ok {
		return nil
	}
	return soa
}

// Out performs an outgoing transfer with the client connecting in w.
// Basic use pattern:
//
//	ch := make(chan *dns.Envelope)
//	tr := new(dns.Transfer)
//	var wg sync.WaitGroup
//	wg.Add(1)
//	go func() {
//		tr.Out(w, r, ch)
//		wg.Done()
//	}()
//	ch <- &dns.Envelope{RR: []dns.RR{soa, rr1, rr2, rr3, soa}}
//	close(ch)
//	wg.Wait() // wait until everything is written out
//	w.Close() // close connection
//
// The server is responsible for sending the correct sequence of RRs through the channel ch.
func (t *Transfer) Out(w ResponseWriter, q *Msg, ch chan *Envelope) error {
	for x := range ch {
		r := new(Msg)
		// Compress?
		r.SetReply(q)
		r.Authoritative = true
		// assume it fits TODO(miek): fix
		r.Answer = append(r.Answer, x.RR...)
		if tsig := q.IsTsig(); tsig != nil && w.TsigStatus() == nil {
			r.SetTsig(tsig.Hdr.Name, tsig.Algorithm, tsig.Fudge, time.Now().Unix())
		}
		if err := w.WriteMsg(r); err != nil {
			return err
		}
		w.TsigTimersOnly(true)
	}
	return nil
}

// ReadMsg reads a message from the transfer connection t.
func (t *Transfer) ReadMsg() (*Msg, error) {
	m := new(Msg)
	p := make([]byte, MaxMsgSize)
	n, err := t.Read(p)
	if err != nil && n == 0 {
		return nil, err
	}
	p = p[:n]
	if err := m.Unpack(p); err != nil {
		return nil, err
	}

	if tp := t.tsigProvider(); tp != nil {
		// Need to work on the original message p, as that was used to calculate the tsig.
		err = TsigVerifyWithProvider(p, tp, t.tsigRequestMAC, t.tsigTimersOnly)
		if ts := m.IsTsig(); ts != nil {
			t.tsigRequestMAC = ts.MAC
		}
	}
	return m, err
}

// WriteMsg writes a message through the transfer connection t.
func (t *Transfer) WriteMsg(m *Msg) (err error) {
	var out []byte
	if ts, tp := m.IsTsig(), t.tsigProvider(); ts != nil && tp != nil {
		out, t.tsigRequestMAC, err = TsigGenerateWithProvider(m, tp, t.tsigRequestMAC, t.tsigTimersOnly)
	} else {
		out, err = m.Pack()
	}
	if err != nil {
		return err
	}
	_, err = t.Write(out)
	return err
}

const errXFR = "bad xfr rcode: %d"
