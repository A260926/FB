package rdf

import (
	"bytes"
	"strings"
	"testing"
)

// TestTargetQuotedTerms verifies the QuotedTriple term type: construction,
// validation, interface participation and recursive content equality.
func TestTargetQuotedTerms(t *testing.T) {
	base := Triple{
		Subj: IRI{str: "https://e/s"},
		Pred: IRI{str: "https://e/p"},
		Obj:  IRI{str: "https://e/o"},
	}

	// construction and accessor
	qt, err := NewQuotedTriple(base)
	targetCheck(t, err == nil)
	targetCheck(t, qt.Type() == TermQuoted)
	targetCheck(t, TriplesEqual(qt.Triple(), base))

	// QuotedTriple participates in the Term, Subject and Object interfaces
	var term Term = qt
	targetCheck(t, term.Type() == TermQuoted)
	var subj Subject = qt
	var obj Object = qt
	targetCheck(t, subj != nil && obj != nil)

	// empty fields are an error
	_, err = NewQuotedTriple(Triple{})
	targetCheck(t, err != nil)
	_, err = NewQuotedTriple(Triple{Pred: base.Pred, Obj: base.Obj})
	targetCheck(t, err != nil)
	_, err = NewQuotedTriple(Triple{Subj: base.Subj, Obj: base.Obj})
	targetCheck(t, err != nil)
	_, err = NewQuotedTriple(Triple{Subj: base.Subj, Pred: base.Pred})
	targetCheck(t, err != nil)

	// a zero-value QuotedTriple nested inside is also an error
	_, err = NewQuotedTriple(Triple{Subj: QuotedTriple{}, Pred: base.Pred, Obj: base.Obj})
	targetCheck(t, err != nil)

	// nested quoting: quoted triple as subject of another quoted triple
	capture, _ := NewLiteral("capture")
	inner, err := NewQuotedTriple(base)
	targetCheck(t, err == nil)
	outer, err := NewQuotedTriple(Triple{
		Subj: inner,
		Pred: IRI{str: "https://e/src"},
		Obj:  capture,
	})
	targetCheck(t, err == nil)
	targetCheck(t, TermsEqual(outer.Triple().Subj, inner))

	// equality is by recursive content, not by object identity
	qt2, err := NewQuotedTriple(base)
	targetCheck(t, err == nil)
	targetCheck(t, TermsEqual(qt, qt2))
	diff, err := NewQuotedTriple(Triple{Subj: base.Subj, Pred: base.Pred, Obj: IRI{str: "https://e/other"}})
	targetCheck(t, err == nil)
	targetCheck(t, !TermsEqual(qt, diff))
	targetCheck(t, !TermsEqual(qt, base.Subj))

	inner2, _ := NewQuotedTriple(base)
	outer2, _ := NewQuotedTriple(Triple{Subj: inner2, Pred: IRI{str: "https://e/src"}, Obj: capture})
	targetCheck(t, TermsEqual(outer, outer2))

	// TriplesEqual and QuadsEqual recurse into quoted terms
	tr1 := Triple{Subj: outer, Pred: IRI{str: "https://e/p2"}, Obj: IRI{str: "https://e/o2"}}
	tr2 := Triple{Subj: outer2, Pred: IRI{str: "https://e/p2"}, Obj: IRI{str: "https://e/o2"}}
	targetCheck(t, TriplesEqual(tr1, tr2))
	targetCheck(t, QuadsEqual(
		Quad{Triple: tr1, Ctx: IRI{str: "https://e/g"}},
		Quad{Triple: tr2, Ctx: IRI{str: "https://e/g"}},
	))

	// explicit quoted syntax on serialization
	targetCheck(t, qt.Serialize(NTriples) == "<< <https://e/s> <https://e/p> <https://e/o> >>")

	// a quoted triple does not assert the quoted relation: one statement in,
	// one statement out
	dec := NewTripleDecoder(strings.NewReader("<< <https://e/s> <https://e/p> <https://e/o> >> <https://e/src> \"capture\" ."), NTriples)
	ts, err := dec.DecodeAll()
	targetCheck(t, err == nil)
	targetCheck(t, len(ts) == 1)
	qs, ok := ts[0].Subj.(QuotedTriple)
	targetCheck(t, ok)
	targetCheck(t, TermsEqual(qs.Triple().Subj, IRI{str: "https://e/s"}))
	targetCheck(t, TermsEqual(qs.Triple().Pred, IRI{str: "https://e/p"}))
	targetCheck(t, TermsEqual(qs.Triple().Obj, IRI{str: "https://e/o"}))
	targetCheck(t, TermsEqual(ts[0].Pred, IRI{str: "https://e/src"}))
}

// TestTargetAnnotations verifies Turtle annotation blocks: expansion into
// statements on the quoted relation, emission order, comma-separated objects,
// predicate switches, nesting, collections, property lists and empty blocks.
func TestTargetAnnotations(t *testing.T) {
	// basic block with two properties
	in := `@prefix e: <https://e/> .
e:s e:p e:o {| e:src "capture" ; e:conf 0.9 |} .
`
	ts, err := NewTripleDecoder(strings.NewReader(in), Turtle).DecodeAll()
	targetCheck(t, err == nil)
	targetCheck(t, len(ts) == 3)
	// the annotated relation comes first
	targetCheck(t, TermsEqual(ts[0].Subj, IRI{str: "https://e/s"}))
	targetCheck(t, TermsEqual(ts[0].Pred, IRI{str: "https://e/p"}))
	targetCheck(t, TermsEqual(ts[0].Obj, IRI{str: "https://e/o"}))
	// then one statement per annotation property, on the quoted relation
	qa0, ok := ts[1].Subj.(QuotedTriple)
	targetCheck(t, ok)
	targetCheck(t, TriplesEqual(qa0.Triple(), ts[0]))
	targetCheck(t, TermsEqual(ts[1].Pred, IRI{str: "https://e/src"}))
	targetCheck(t, TermsEqual(ts[1].Obj, Literal{str: "capture", DataType: xsdString}))
	qa1, ok := ts[2].Subj.(QuotedTriple)
	targetCheck(t, ok)
	targetCheck(t, TriplesEqual(qa1.Triple(), ts[0]))
	targetCheck(t, TermsEqual(ts[2].Pred, IRI{str: "https://e/conf"}))
	targetCheck(t, TermsEqual(ts[2].Obj, Literal{str: "0.9", DataType: xsdDecimal}))

	// each comma-separated object can carry its own block
	in2 := `@prefix e: <https://e/> .
e:s e:p e:o1 {| e:a e:b |}, e:o2 {| e:c e:d |} .
`
	ts2, err := NewTripleDecoder(strings.NewReader(in2), Turtle).DecodeAll()
	targetCheck(t, err == nil)
	targetCheck(t, len(ts2) == 4)
	targetCheck(t, TermsEqual(ts2[0].Obj, IRI{str: "https://e/o1"}))
	qb0, ok := ts2[1].Subj.(QuotedTriple)
	targetCheck(t, ok)
	targetCheck(t, TriplesEqual(qb0.Triple(), ts2[0]))
	targetCheck(t, TermsEqual(ts2[1].Pred, IRI{str: "https://e/a"}))
	targetCheck(t, TermsEqual(ts2[2].Obj, IRI{str: "https://e/o2"}))
	qb1, ok := ts2[3].Subj.(QuotedTriple)
	targetCheck(t, ok)
	targetCheck(t, TriplesEqual(qb1.Triple(), ts2[2]))
	targetCheck(t, TermsEqual(ts2[3].Pred, IRI{str: "https://e/c"}))

	// after ';' switches the predicate, the annotated statement is re-determined
	in3 := `@prefix e: <https://e/> .
e:s e:p1 e:o1 {| e:a e:b |} ; e:p2 e:o2 {| e:c e:d |} .
`
	ts3, err := NewTripleDecoder(strings.NewReader(in3), Turtle).DecodeAll()
	targetCheck(t, err == nil)
	targetCheck(t, len(ts3) == 4)
	qc0, ok := ts3[1].Subj.(QuotedTriple)
	targetCheck(t, ok)
	targetCheck(t, TermsEqual(qc0.Triple().Pred, IRI{str: "https://e/p1"}))
	qc1, ok := ts3[3].Subj.(QuotedTriple)
	targetCheck(t, ok)
	targetCheck(t, TermsEqual(qc1.Triple().Pred, IRI{str: "https://e/p2"}))

	// annotation objects can themselves be annotated; relation first, then
	// its annotation
	in4 := `@prefix e: <https://e/> .
e:s e:p e:o {| e:a e:b {| e:c e:d |} |} .
`
	ts4, err := NewTripleDecoder(strings.NewReader(in4), Turtle).DecodeAll()
	targetCheck(t, err == nil)
	targetCheck(t, len(ts4) == 3)
	qd0, ok := ts4[1].Subj.(QuotedTriple)
	targetCheck(t, ok)
	targetCheck(t, TriplesEqual(qd0.Triple(), ts4[0]))
	qd1, ok := ts4[2].Subj.(QuotedTriple)
	targetCheck(t, ok)
	targetCheck(t, TriplesEqual(qd1.Triple(), ts4[1]))
	targetCheck(t, TermsEqual(ts4[2].Pred, IRI{str: "https://e/c"}))

	// relations inside collections can carry blocks
	in5 := `@prefix e: <https://e/> .
e:s e:p ( e:o {| e:a e:b |} ) .
`
	ts5, err := NewTripleDecoder(strings.NewReader(in5), Turtle).DecodeAll()
	targetCheck(t, err == nil)
	targetCheck(t, len(ts5) == 4)
	targetCheck(t, TermsEqual(ts5[1].Pred, IRI{str: "http://www.w3.org/1999/02/22-rdf-syntax-ns#first"}))
	qe0, ok := ts5[2].Subj.(QuotedTriple)
	targetCheck(t, ok)
	targetCheck(t, TriplesEqual(qe0.Triple(), ts5[1]))
	targetCheck(t, TermsEqual(ts5[2].Pred, IRI{str: "https://e/a"}))

	// relations inside blank node property lists can carry blocks
	in6 := `@prefix e: <https://e/> .
e:s e:p [ e:q e:v {| e:a e:b |} ] .
`
	ts6, err := NewTripleDecoder(strings.NewReader(in6), Turtle).DecodeAll()
	targetCheck(t, err == nil)
	targetCheck(t, len(ts6) == 3)
	qf0, ok := ts6[2].Subj.(QuotedTriple)
	targetCheck(t, ok)
	targetCheck(t, TriplesEqual(qf0.Triple(), ts6[1]))

	// an empty annotation block is legal and produces no extra statements
	in7 := `@prefix e: <https://e/> .
e:s e:p e:o {| |} .
`
	ts7, err := NewTripleDecoder(strings.NewReader(in7), Turtle).DecodeAll()
	targetCheck(t, err == nil)
	targetCheck(t, len(ts7) == 1)
	targetCheck(t, TermsEqual(ts7[0].Subj, IRI{str: "https://e/s"}))
}

// TestTargetFormats verifies parsing and round-tripping of quoted triples in
// N-Triples, Turtle and N-Quads, including nested quoting, and that '<<'/'>>'
// and braces inside literals and comments are not mistaken for structure.
func TestTargetFormats(t *testing.T) {
	// N-Triples: double-nested quoted triple as subject
	ntIn := "<< << <https://e/a> <https://e/b> <https://e/c> >> <https://e/p> <https://e/o> >> <https://e/src> \"capture\" .\n"
	ts, err := NewTripleDecoder(strings.NewReader(ntIn), NTriples).DecodeAll()
	targetCheck(t, err == nil)
	targetCheck(t, len(ts) == 1)
	qs, ok := ts[0].Subj.(QuotedTriple)
	targetCheck(t, ok)
	qss, ok := qs.Triple().Subj.(QuotedTriple)
	targetCheck(t, ok)
	targetCheck(t, TermsEqual(qss.Triple().Subj, IRI{str: "https://e/a"}))
	targetCheck(t, TermsEqual(qss.Triple().Pred, IRI{str: "https://e/b"}))
	targetCheck(t, TermsEqual(qss.Triple().Obj, IRI{str: "https://e/c"}))

	// N-Triples round trip
	var ntBuf bytes.Buffer
	ntEnc := NewTripleEncoder(&ntBuf, NTriples)
	targetCheck(t, ntEnc.EncodeAll(ts) == nil)
	targetCheck(t, ntEnc.Close() == nil)
	tsBack, err := NewTripleDecoder(strings.NewReader(ntBuf.String()), NTriples).DecodeAll()
	targetCheck(t, err == nil)
	targetCheck(t, len(tsBack) == 1)
	targetCheck(t, TriplesEqual(ts[0], tsBack[0]))

	// Turtle: quoted triple using prefixes, 'a' and a relative IRI
	ttlIn := `@base <https://base/> .
@prefix e: <https://e/> .
<< e:s a e:C >> e:src <rel> {| e:batch 2 |} .
`
	ts2, err := NewTripleDecoder(strings.NewReader(ttlIn), Turtle).DecodeAll()
	targetCheck(t, err == nil)
	targetCheck(t, len(ts2) == 2)
	qs2, ok := ts2[0].Subj.(QuotedTriple)
	targetCheck(t, ok)
	targetCheck(t, TermsEqual(qs2.Triple().Subj, IRI{str: "https://e/s"}))
	targetCheck(t, TermsEqual(qs2.Triple().Pred, IRI{str: "http://www.w3.org/1999/02/22-rdf-syntax-ns#type"}))
	targetCheck(t, TermsEqual(qs2.Triple().Obj, IRI{str: "https://e/C"}))
	targetCheck(t, TermsEqual(ts2[0].Obj, IRI{str: "https://base/rel"}))
	qs2b, ok := ts2[1].Subj.(QuotedTriple)
	targetCheck(t, ok)
	targetCheck(t, TriplesEqual(qs2b.Triple(), ts2[0]))

	// Turtle round trip (explicit quoted syntax, no annotation shorthand)
	var ttlBuf bytes.Buffer
	ttlEnc := NewTripleEncoder(&ttlBuf, Turtle)
	targetCheck(t, ttlEnc.EncodeAll(ts2) == nil)
	targetCheck(t, ttlEnc.Close() == nil)
	ts2Back, err := NewTripleDecoder(strings.NewReader(ttlBuf.String()), Turtle).DecodeAll()
	targetCheck(t, err == nil)
	targetCheck(t, len(ts2Back) == len(ts2))
	for _, want := range ts2 {
		found := false
		for _, got := range ts2Back {
			if TriplesEqual(want, got) {
				found = true
				break
			}
		}
		targetCheck(t, found)
	}

	// N-Quads: quoted triple as subject, with graph name
	nqIn := "<< <https://e/s> <https://e/p> <https://e/o> >> <https://e/src> \"capture\" <https://e/g> .\n"
	quads, err := NewQuadDecoder(strings.NewReader(nqIn), NQuads).DecodeAll()
	targetCheck(t, err == nil)
	targetCheck(t, len(quads) == 1)
	targetCheck(t, TermsEqual(quads[0].Ctx, IRI{str: "https://e/g"}))
	nqs, ok := quads[0].Subj.(QuotedTriple)
	targetCheck(t, ok)
	targetCheck(t, TermsEqual(nqs.Triple().Obj, IRI{str: "https://e/o"}))

	// N-Quads round trip
	var nqBuf bytes.Buffer
	nqEnc := NewQuadEncoder(&nqBuf, NQuads)
	targetCheck(t, nqEnc.EncodeAll(quads) == nil)
	targetCheck(t, nqEnc.Close() == nil)
	quadsBack, err := NewQuadDecoder(strings.NewReader(nqBuf.String()), NQuads).DecodeAll()
	targetCheck(t, err == nil)
	targetCheck(t, len(quadsBack) == 1)
	targetCheck(t, QuadsEqual(quads[0], quadsBack[0]))

	// '<<', '>>' and braces inside literals, and braces in comments, are not
	// mistaken for structure
	tricky := "<https://e/s> <https://e/p> \"a << b >> {| c |} ( d )\" . # comment with ( ) << >> {|}\n"
	tsT, err := NewTripleDecoder(strings.NewReader(tricky), Turtle).DecodeAll()
	targetCheck(t, err == nil)
	targetCheck(t, len(tsT) == 1)
	targetCheck(t, tsT[0].Obj.String() == "a << b >> {| c |} ( d )")
}

// TestTargetInvalidRoles verifies that quoted triples are rejected in term
// positions where they are not allowed, and that blank node property lists
// and collections are rejected inside quoted triples.
func TestTargetInvalidRoles(t *testing.T) {
	// quoted triple as predicate is rejected (N-Triples)
	_, err := NewTripleDecoder(strings.NewReader("<https://e/s> << <https://e/a> <https://e/b> <https://e/c> >> <https://e/o> ."), NTriples).DecodeAll()
	targetCheck(t, err != nil)

	// quoted triple as graph name is rejected (N-Quads)
	_, err = NewQuadDecoder(strings.NewReader("<https://e/s> <https://e/p> <https://e/o> << <https://e/a> <https://e/b> <https://e/c> >> ."), NQuads).DecodeAll()
	targetCheck(t, err != nil)

	// quoted triple as predicate is rejected (Turtle)
	_, err = NewTripleDecoder(strings.NewReader("<https://e/s> << <https://e/a> <https://e/b> <https://e/c> >> <https://e/o> ."), Turtle).DecodeAll()
	targetCheck(t, err != nil)

	// literal as subject of a quoted triple is rejected
	_, err = NewTripleDecoder(strings.NewReader("<< \"x\" <https://e/b> <https://e/c> >> <https://e/p> <https://e/o> ."), NTriples).DecodeAll()
	targetCheck(t, err != nil)

	// literal as predicate of a quoted triple is rejected
	_, err = NewTripleDecoder(strings.NewReader("<< <https://e/a> \"x\" <https://e/c> >> <https://e/p> <https://e/o> ."), NTriples).DecodeAll()
	targetCheck(t, err != nil)

	// blank node property lists are not allowed inside quoted triples (Turtle)
	_, err = NewTripleDecoder(strings.NewReader("<https://e/s> <https://e/p> << <https://e/a> <https://e/b> [ <https://e/c> <https://e/d> ] >> ."), Turtle).DecodeAll()
	targetCheck(t, err != nil)

	// collections are not allowed inside quoted triples (Turtle)
	_, err = NewTripleDecoder(strings.NewReader("<https://e/s> <https://e/p> << <https://e/a> <https://e/b> ( <https://e/c> ) >> ."), Turtle).DecodeAll()
	targetCheck(t, err != nil)

	// model level: empty fields and invalid nested quoted triples are rejected
	iriS := IRI{str: "https://e/s"}
	iriP := IRI{str: "https://e/p"}
	iriO := IRI{str: "https://e/o"}
	_, err = NewQuotedTriple(Triple{Subj: iriS, Pred: iriP})
	targetCheck(t, err != nil)
	_, err = NewQuotedTriple(Triple{Subj: QuotedTriple{}, Pred: iriP, Obj: iriO})
	targetCheck(t, err != nil)
	_, err = NewQuotedTriple(Triple{Subj: iriS, Pred: iriP, Obj: QuotedTriple{}})
	targetCheck(t, err != nil)
}
