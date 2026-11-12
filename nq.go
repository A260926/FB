package rdf

import "io"

// parseNQ parses a line of N-Quads and returns a valid quad or an error.
func (d *QuadDecoder) parseNQ() (q Quad, err error) {
	defer d.recover(&err)

	for d.peek().typ == tokenEOL {
		d.next()
	}
	if d.peek().typ == tokenEOF {
		return q, io.EOF
	}

	// Set Quad context to default graph
	q.Ctx = d.DefaultGraph

	// parse quad subject
	tok := d.expectAs("subject", tokenIRIAbs, tokenBNode, tokenQuotedStart)
	switch tok.typ {
	case tokenIRIAbs:
		q.Subj = IRI{str: tok.text}
	case tokenBNode:
		q.Subj = Blank{id: tok.text}
	case tokenQuotedStart:
		q.Subj = d.parseQuotedTriple()
	}

	// parse quad predicate
	tok = d.expect1As("predicate", tokenIRIAbs)
	q.Pred = IRI{str: tok.text}

	// parse quad object
	tok = d.expectAs("object", tokenIRIAbs, tokenBNode, tokenLiteral, tokenQuotedStart)

	switch tok.typ {
	case tokenBNode:
		q.Obj = Blank{id: tok.text}
	case tokenLiteral:
		q.Obj = d.parseLiteralValue(tok.text)
	case tokenIRIAbs:
		q.Obj = IRI{str: tok.text}
	case tokenQuotedStart:
		q.Obj = d.parseQuotedTriple()
	}

	// parse optional graph
	p := d.peek()
	switch p.typ {
	case tokenIRIAbs:
		tok = d.next() // consume peeked token
		q.Ctx = IRI{str: tok.text}
	case tokenBNode:
		tok = d.next() // consume peeked token
		q.Ctx = Blank{id: tok.text}
	case tokenDot:
		break
	default:
		// NB: quoted triples are not valid graph names
		d.expectAs("graph", tokenIRIAbs, tokenBNode)
	}

	// parse final dot
	d.expect1As("dot (.)", tokenDot)

	// check for extra tokens, assert we reached end of line
	d.expect1As("end of line", tokenEOL)

	if d.peek().typ == tokenEOF {
		// drain lexer
		d.next()
	}
	return q, err
}

// parseQuotedTriple parses a quoted triple: the opening '<<' has allready been
// consumed, it parses subject, predicate, object and the closing '>>', and
// returns the QuotedTriple. Quoted triples can be nested.
func (d *QuadDecoder) parseQuotedTriple() QuotedTriple {
	var qt Triple

	// parse quoted triple subject
	tok := d.expectAs("quoted triple subject", tokenIRIAbs, tokenBNode, tokenQuotedStart)
	switch tok.typ {
	case tokenIRIAbs:
		qt.Subj = IRI{str: tok.text}
	case tokenBNode:
		qt.Subj = Blank{id: tok.text}
	case tokenQuotedStart:
		qt.Subj = d.parseQuotedTriple()
	}

	// parse quoted triple predicate
	tok = d.expect1As("quoted triple predicate", tokenIRIAbs)
	qt.Pred = IRI{str: tok.text}

	// parse quoted triple object
	tok = d.expectAs("quoted triple object", tokenIRIAbs, tokenBNode, tokenLiteral, tokenQuotedStart)
	switch tok.typ {
	case tokenIRIAbs:
		qt.Obj = IRI{str: tok.text}
	case tokenBNode:
		qt.Obj = Blank{id: tok.text}
	case tokenLiteral:
		qt.Obj = d.parseLiteralValue(tok.text)
	case tokenQuotedStart:
		qt.Obj = d.parseQuotedTriple()
	}

	// parse closing '>>'
	d.expect1As("quoted triple end (>>)", tokenQuotedEnd)

	return QuotedTriple{triple: qt}
}

// parseLiteralValue parses the optional language tag or datatype IRI following
// a literal token, and returns the complete Literal.
func (d *QuadDecoder) parseLiteralValue(val string) Literal {
	l := Literal{
		str:      val,
		DataType: xsdString,
	}
	p := d.peek()
	switch p.typ {
	case tokenLangMarker:
		d.next() // consume peeked token
		tok := d.expect1As("literal language", tokenLang)
		l.lang = tok.text
		l.DataType = rdfLangString
	case tokenDataTypeMarker:
		d.next() // consume peeked token
		tok := d.expect1As("literal datatype", tokenIRIAbs)
		l.DataType = IRI{str: tok.text}
	}
	return l
}
