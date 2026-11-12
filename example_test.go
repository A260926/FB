package rdf_test

import (
	"fmt"
	"strings"

	"github.com/knakk/rdf"
)

// ExampleQuotedTriple demonstrates reading a Turtle document with a quoted
// triple and an annotation block from an in-memory string, and re-encoding
// the decoded statements as N-Triples.
func ExampleQuotedTriple() {
	in := `@prefix e: <https://example.test/> .
e:s e:p e:o {| e:source "capture-1" ; e:confidence 0.9 |} .
`
	dec := rdf.NewTripleDecoder(strings.NewReader(in), rdf.Turtle)
	triples, err := dec.DecodeAll()
	if err != nil {
		panic(err)
	}

	var out strings.Builder
	enc := rdf.NewTripleEncoder(&out, rdf.NTriples)
	if err := enc.EncodeAll(triples); err != nil {
		panic(err)
	}
	if err := enc.Close(); err != nil {
		panic(err)
	}
	fmt.Print(out.String())

	// Output:
	// <https://example.test/s> <https://example.test/p> <https://example.test/o> .
	// << <https://example.test/s> <https://example.test/p> <https://example.test/o> >> <https://example.test/source> "capture-1" .
	// << <https://example.test/s> <https://example.test/p> <https://example.test/o> >> <https://example.test/confidence> "0.9"^^<http://www.w3.org/2001/XMLSchema#decimal> .
}
