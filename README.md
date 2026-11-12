# rdf

This package introduces data structures for representing RDF resources, and includes functions for parsing and serialization of RDF data.

For complete documentation see [godoc](http://godoc.org/github.com/knakk/rdf).

## Data model

The package follows the RDF data model as described in http://www.w3.org/TR/rdf11-concepts/, with one extension: in addition to the three basic term types (IRI, blank node and literal), it supports quoted triples (RDF-star).

A `QuotedTriple` (term type `TermQuoted`) quotes a `Triple` and can itself be used as the subject or object of a statement. Construct one with `NewQuotedTriple(triple)`, and get the quoted triple back with `QuotedTriple.Triple()`. The quoted triple's subject must be an IRI, blank node or another quoted triple; the predicate must be an IRI; the object may be any term, including a quoted triple. A quoted triple only refers to the statement it quotes — it does not assert it — which makes it possible to attach metadata such as source, capture batch or confidence to one concrete statement:

```
<< <https://e/s> <https://e/p> <https://e/o> >> <https://e/src> "capture" .
```

`TermsEqual`, `TriplesEqual` and `QuadsEqual` compare quoted triples by their recursively compared content, not by object identity or surface syntax.

## Parsing and serialization

The N-Triples, N-Quads and Turtle decoders parse the quoted triple syntax `<< s p o >>`, including nested quoted triples, and the encoders serialize quoted terms in the same explicit syntax. Quoted triples are not valid as predicate or graph name. In Turtle, quoted triples can use the currently effective prefixes, `a`, relative IRIs and literals, but not blank node property lists (`[]`) or collections (`()`).

The Turtle decoder additionally supports annotation blocks:

```
s p o {| ap ao ; bp bo |} .
```

This is syntactic sugar for first stating `s p o`, and then one statement per annotation property with `<< s p o >>` as subject. Each comma-separated object can carry its own block, and annotation objects can themselves be annotated. The Turtle encoder always emits the explicit quoted triple syntax and does not compress statements back into annotation blocks.
