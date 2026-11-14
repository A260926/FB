package dns

import "fmt"

// This file implements the interpretation of the RR stream produced by an
// AXFR/IXFR transfer (RFC 1034 section 4.3.5 and RFC 1995) and the machinery
// used by Transfer.InZone to turn such a stream into an owned zone snapshot.
// The same grammar drives Transfer.In's transfer termination, so both entry
// points agree on what a record sequence means regardless of how the RRs are
// split across TCP messages.

// rrsetKey identifies an RRset (owner, type and class, but neither TTL nor
// RDATA). The owner name is canonicalized, matching IsDuplicate's
// case-insensitive name handling.
type rrsetKey struct {
	name  string
	rtype uint16
	class uint16
}

func keyFor(rr RR) rrsetKey {
	h := rr.Header()
	return rrsetKey{CanonicalName(h.Name), h.Rrtype, h.Class}
}

// serialRelation compares two 32 bit zone serial numbers using the serial
// number arithmetic from RFC 1982. It returns 1 when cur is later than prev,
// -1 when cur is earlier and 0 when they are equal. A difference of exactly
// 2^31 leaves the two values incomparable and is reported as an error.
func serialRelation(cur, prev uint32) (int, error) {
	if cur == prev {
		return 0, nil
	}
	switch d := cur - prev; {
	case d == 1<<31:
		return 0, &Error{err: fmt.Sprintf("xfr: serial %d and %d are not comparable", cur, prev)}
	case d < 1<<31:
		return 1, nil
	default:
		return -1, nil
	}
}

// markerSOA returns rr when it is an apex SOA of the queried IN class, nil when
// it is not a SOA at all, and an error when it is a SOA that cannot legally be
// used as a transfer segment marker.
func markerSOA(rr RR, apex string) (*SOA, error) {
	soa, ok := rr.(*SOA)
	if !ok {
		return nil, nil
	}
	if soa.Hdr.Class != ClassINET || !equal(soa.Hdr.Name, apex) {
		return nil, &Error{err: "xfr: SOA record is not an in-class IN apex SOA"}
	}
	return soa, nil
}

// soaEqual reports whether two SOA records describe the same zone version,
// including the serial. The header TTL and owner name are compared as well;
// domain names are compared case-insensitively.
func soaEqual(a, b *SOA) bool {
	return soaEqualExceptSerial(a, b) && a.Serial == b.Serial
}

// soaEqualExceptSerial reports whether two SOA records agree on everything
// but the serial.
func soaEqualExceptSerial(a, b *SOA) bool {
	if a == nil || b == nil {
		return a == b
	}
	ah, bh := a.Hdr, b.Hdr
	if ah.Ttl != bh.Ttl || ah.Class != bh.Class || !equal(ah.Name, bh.Name) {
		return false
	}
	if !equal(a.Ns, b.Ns) || !equal(a.Mbox, b.Mbox) {
		return false
	}
	return a.Refresh == b.Refresh && a.Retry == b.Retry && a.Expire == b.Expire && a.Minttl == b.Minttl
}

// xfrApplier receives the records accepted by the transfer grammar.
type xfrApplier interface {
	// useAxfr marks that the answer is a full AXFR (either a direct AXFR
	// answer or an AXFR fallback to an IXFR request) and discards any state
	// derived from the previous snapshot.
	useAxfr(soa *SOA)
	// useNewSOA installs the current-version SOA of an IXFR change group.
	useNewSOA(soa *SOA)
	applyDelete(rr RR) error
	applyAdd(rr RR) error
}

type noopXfrApplier struct{}

func (noopXfrApplier) useAxfr(*SOA)         {}
func (noopXfrApplier) useNewSOA(*SOA)       {}
func (noopXfrApplier) applyDelete(RR) error { return nil }
func (noopXfrApplier) applyAdd(RR) error    { return nil }

type grammarState int

const (
	gStart  grammarState = iota // no record seen yet
	gSingle                     // only the leading current SOA seen
	gAxfr                       // collecting AXFR zone data
	gDel                        // inside an IXFR deletion section
	gAdd                        // inside an IXFR addition section
	gClosed                     // closing SOA consumed
)

// xfrGrammar is a streaming, message-boundary agnostic interpreter for the RR
// sequence of an AXFR/IXFR answer.
type xfrGrammar struct {
	qtype         uint16 // TypeAXFR or TypeIXFR
	apex          string
	app           xfrApplier
	strict        bool // enforce full-SOA chaining against a validated base
	state         grammarState
	lead          *SOA // server's current SOA, first record of the answer
	expected      *SOA // SOA version the next change group's old SOA must match
	baseConnected bool // whether the first change group was attached to expected
	groupOld      *SOA
	groupNew      *SOA
}

func newXfrGrammar(qtype uint16, apex string, expected *SOA, strict bool, app xfrApplier) *xfrGrammar {
	return &xfrGrammar{qtype: qtype, apex: apex, expected: expected, strict: strict, app: app}
}

// ended reports whether a closing SOA has been consumed, so no further record
// may arrive.
func (g *xfrGrammar) ended() bool { return g.state == gClosed }

// acceptEOF reports whether a clean connection close at this point is a legal
// end of the transfer. An IXFR no-change answer consists of a single SOA, but
// an AXFR always needs a closing SOA.
func (g *xfrGrammar) acceptEOF() bool {
	switch g.state {
	case gClosed:
		return true
	case gSingle:
		return g.qtype == TypeIXFR
	default:
		return false
	}
}

// noChange reports whether the whole answer was a single current SOA.
func (g *xfrGrammar) noChange() bool { return g.state == gSingle }

// noChangeConfirmed reports that a single-SOA answer is a complete no-change
// response: the query carried a base SOA and the server's serial is equal to
// or behind it, so no forward delta can legitimately follow. An incomparable
// serial is left for the caller to handle after the connection closes.
func (g *xfrGrammar) noChangeConfirmed() bool {
	if g.state != gSingle || g.qtype != TypeIXFR || g.expected == nil {
		return false
	}
	rel, err := serialRelation(g.lead.Serial, g.expected.Serial)
	if err != nil {
		return false
	}
	return rel <= 0
}

// step feeds one answer RR into the grammar.
func (g *xfrGrammar) step(rr RR) error {
	switch g.state {
	case gStart:
		soa, err := markerSOA(rr, g.apex)
		if err != nil {
			return err
		}
		if soa == nil {
			return ErrSoa
		}
		g.lead = soa
		g.state = gSingle
		return nil

	case gClosed:
		return &Error{err: "xfr: trailing records after closing SOA"}

	case gSingle:
		soa, err := markerSOA(rr, g.apex)
		if err != nil {
			return err
		}
		if soa == nil {
			// A plain record after the leading SOA means a full AXFR.
			g.startAxfr()
			g.state = gAxfr
			return g.app.applyAdd(rr)
		}
		if soaEqual(soa, g.lead) {
			// AXFR of an empty zone: the closing SOA arrived immediately.
			g.startAxfr()
			g.state = gClosed
			return nil
		}
		if g.qtype == TypeAXFR {
			return &Error{err: "xfr: AXFR answer contains a foreign apex SOA"}
		}
		// IXFR: this is the old-version SOA of the first change group.
		if err := g.beginGroup(soa); err != nil {
			return err
		}
		g.state = gDel
		return nil

	case gAxfr:
		soa, err := markerSOA(rr, g.apex)
		if err != nil {
			return err
		}
		if soa == nil {
			return g.app.applyAdd(rr)
		}
		if !soaEqual(soa, g.lead) {
			return &Error{err: "xfr: AXFR answer contains a foreign apex SOA"}
		}
		g.state = gClosed
		return nil

	case gDel:
		soa, err := markerSOA(rr, g.apex)
		if err != nil {
			return err
		}
		if soa == nil {
			return g.app.applyDelete(rr)
		}
		// The new-version SOA closes the deletion section.
		rel, err := serialRelation(soa.Serial, g.groupOld.Serial)
		if err != nil {
			return err
		}
		if rel != 1 {
			return &Error{err: fmt.Sprintf("xfr: IXFR change must move strictly forward, serial %d does not succeed %d", soa.Serial, g.groupOld.Serial)}
		}
		g.groupNew = soa
		g.expected = soa
		g.app.useNewSOA(soa)
		g.state = gAdd
		return nil
	case gAdd:
		soa, err := markerSOA(rr, g.apex)
		if err != nil {
			return err
		}
		if soa == nil {
			return g.app.applyAdd(rr)
		}
		if soaEqual(soa, g.lead) {
			// Closing current SOA: the last group must have arrived exactly
			// at the version announced by the leading SOA.
			if g.groupNew == nil || !soaEqual(g.groupNew, g.lead) {
				return &Error{err: "xfr: IXFR closing SOA does not match the current zone version"}
			}
			g.state = gClosed
			return nil
		}
		// Another change group starts; its old SOA must connect to the new
		// version of the previous group.
		if err := g.beginGroup(soa); err != nil {
			return err
		}
		g.state = gDel
		return nil
	}
	return &Error{err: "xfr: internal error: unknown transfer grammar state"}
}

func (g *xfrGrammar) startAxfr() {
	g.app.useAxfr(g.lead)
}

func (g *xfrGrammar) beginGroup(old *SOA) error {
	if g.expected == nil {
		return &Error{err: "xfr: IXFR change group without a known base version"}
	}
	if !g.baseConnected {
		// The first group continues the version the request was based on.
		// Transfer.In only knows the query SOA, whose non-serial fields are
		// not populated, so it checks the serial alone; InZone has a validated
		// base SOA and requires full equality.
		if old.Serial != g.expected.Serial {
			return &Error{err: fmt.Sprintf("xfr: IXFR old SOA serial %d does not continue from serial %d", old.Serial, g.expected.Serial)}
		}
		if g.strict && !soaEqual(old, g.expected) {
			return &Error{err: "xfr: IXFR first old SOA does not match the base apex SOA"}
		}
		g.baseConnected = true
	} else if !soaEqual(old, g.expected) {
		return &Error{err: fmt.Sprintf("xfr: IXFR old SOA serial %d does not continue from serial %d", old.Serial, g.expected.Serial)}
	}
	g.groupOld = old
	g.groupNew = nil
	return nil
}

// zoneApplier maintains the privately owned working snapshot of InZone.
type zoneApplier struct {
	apex   string
	soa    *SOA
	rrsets map[rrsetKey][]RR
}

func newZoneApplier(apex string) *zoneApplier {
	return &zoneApplier{apex: apex, rrsets: make(map[rrsetKey][]RR)}
}

func (z *zoneApplier) useAxfr(soa *SOA) {
	// An AXFR replaces the whole snapshot; nothing from base survives.
	z.soa = Copy(soa).(*SOA)
	z.rrsets = make(map[rrsetKey][]RR)
}

func (z *zoneApplier) useNewSOA(soa *SOA) {
	z.soa = Copy(soa).(*SOA)
}

func (z *zoneApplier) applyDelete(rr RR) error {
	key := keyFor(rr)
	bucket := z.rrsets[key]
	for i, existing := range bucket {
		if !IsDuplicate(existing, rr) {
			continue
		}
		if existing.Header().Ttl != rr.Header().Ttl {
			return &Error{err: fmt.Sprintf("xfr: deletion of %s type %d carries the wrong TTL", rr.Header().Name, rr.Header().Rrtype)}
		}
		bucket[i] = bucket[len(bucket)-1]
		z.rrsets[key] = bucket[:len(bucket)-1]
		return nil
	}
	return &Error{err: fmt.Sprintf("xfr: deletion of %s type %d does not exist in the zone", rr.Header().Name, rr.Header().Rrtype)}
}

func (z *zoneApplier) applyAdd(rr RR) error {
	key := keyFor(rr)
	for _, existing := range z.rrsets[key] {
		if IsDuplicate(existing, rr) {
			return &Error{err: fmt.Sprintf("xfr: %s type %d is already present in the zone", rr.Header().Name, rr.Header().Rrtype)}
		}
		if existing.Header().Ttl != rr.Header().Ttl {
			return &Error{err: fmt.Sprintf("xfr: RRset of %s type %d would have conflicting TTLs", rr.Header().Name, rr.Header().Rrtype)}
		}
	}
	z.rrsets[key] = append(z.rrsets[key], Copy(rr))
	return nil
}

// snapshot emits the working snapshot with the single apex SOA first. The
// order of the remaining records is not guaranteed.
func (z *zoneApplier) snapshot() ([]RR, error) {
	if z.soa == nil {
		return nil, &Error{err: "xfr: transfer produced no apex SOA"}
	}
	n := 1
	for _, bucket := range z.rrsets {
		n += len(bucket)
	}
	out := make([]RR, 0, n)
	out = append(out, z.soa)
	for _, bucket := range z.rrsets {
		if len(bucket) == 0 {
			continue
		}
		ttl := bucket[0].Header().Ttl
		for _, rr := range bucket {
			h := rr.Header()
			if h.Class != ClassINET || !IsSubDomain(z.apex, h.Name) {
				return nil, &Error{err: fmt.Sprintf("xfr: record %s type %d is outside the queried zone", h.Name, h.Rrtype)}
			}
			if h.Ttl != ttl {
				return nil, &Error{err: fmt.Sprintf("xfr: RRset of %s type %d has conflicting TTLs", h.Name, h.Rrtype)}
			}
			out = append(out, rr)
		}
	}
	return out, nil
}

// validateBase checks the caller supplied snapshot and turns it into an owned
// working snapshot.
func validateBase(apex string, base []RR) (*zoneApplier, *SOA, error) {
	if len(base) == 0 {
		return nil, nil, &Error{err: "xfr: IXFR requires a non-empty base snapshot"}
	}
	z := newZoneApplier(apex)
	var baseSOA *SOA
	for _, rr := range base {
		if rr == nil {
			return nil, nil, &Error{err: "xfr: base snapshot contains a nil record"}
		}
		h := rr.Header()
		if h.Class != ClassINET {
			return nil, nil, &Error{err: fmt.Sprintf("xfr: base record %s is not class IN", h.Name)}
		}
		if !IsSubDomain(apex, h.Name) {
			return nil, nil, &Error{err: fmt.Sprintf("xfr: base record %s is outside the zone", h.Name)}
		}
		if soa, ok := rr.(*SOA); ok {
			if !equal(h.Name, apex) {
				return nil, nil, &Error{err: "xfr: base snapshot contains a non-apex SOA"}
			}
			if baseSOA != nil {
				return nil, nil, &Error{err: "xfr: base snapshot contains more than one apex SOA"}
			}
			baseSOA = Copy(soa).(*SOA)
			continue
		}
		if err := z.applyAdd(rr); err != nil {
			return nil, nil, err
		}
	}
	if baseSOA == nil {
		return nil, nil, &Error{err: "xfr: base snapshot contains no apex SOA"}
	}
	z.soa = baseSOA
	return z, baseSOA, nil
}

// validateZoneQuery checks the transfer message accepted by InZone.
func validateZoneQuery(q *Msg) (qtype uint16, apex string, nsSOA *SOA, err error) {
	if q == nil || len(q.Question) != 1 {
		return 0, "", nil, &Error{err: "xfr: query must contain exactly one question"}
	}
	question := q.Question[0]
	if question.Qclass != ClassINET {
		return 0, "", nil, &Error{err: "xfr: only class IN transfers are supported"}
	}
	switch question.Qtype {
	case TypeAXFR, TypeIXFR:
		qtype = question.Qtype
	default:
		return 0, "", nil, &Error{err: "unsupported question type"}
	}
	apex = question.Name
	if qtype == TypeIXFR {
		if len(q.Ns) != 1 {
			return 0, "", nil, &Error{err: "xfr: IXFR query must carry exactly one authority SOA"}
		}
		soa, ok := q.Ns[0].(*SOA)
		if !ok {
			return 0, "", nil, &Error{err: "xfr: IXFR authority section must contain a SOA"}
		}
		if soa.Hdr.Class != ClassINET || !equal(soa.Hdr.Name, apex) {
			return 0, "", nil, &Error{err: "xfr: IXFR authority SOA is not an in-class IN apex SOA"}
		}
		nsSOA = soa
	}
	return qtype, apex, nsSOA, nil
}
