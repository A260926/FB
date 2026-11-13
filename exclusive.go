// Copyright 2015-2019 Brett Vickers.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package etree

import (
	"errors"
	"fmt"
	"io"
	"sort"
	"strings"
	"unicode/utf8"
)

// Errors returned by [CanonicalizeExclusive]. They are returned unwrapped,
// so a single errors.Is call matches them.
var (
	// ErrExclusiveNilRoot is returned when CanonicalizeExclusive is called
	// with a nil root element.
	ErrExclusiveNilRoot = errors.New("etree: exclusive canonicalization requires a non-nil root element")

	// ErrExclusiveUnboundPrefix is returned when an element or prefixed
	// attribute visibly uses a namespace prefix that has no binding in
	// effect at its position.
	ErrExclusiveUnboundPrefix = errors.New("etree: unbound namespace prefix")

	// ErrExclusiveIllegalBinding is returned when a reserved namespace
	// prefix is bound to a value other than the one mandated by the XML
	// specification.
	ErrExclusiveIllegalBinding = errors.New("etree: illegal reserved namespace binding")

	// ErrExclusiveRelativeURI is returned when a namespace is bound to a
	// relative namespace URI reference.
	ErrExclusiveRelativeURI = errors.New("etree: relative namespace URI")

	// ErrExclusiveDuplicateAttr is returned when one element carries two
	// ordinary attributes with the same expanded name (namespace URI and
	// local name).
	ErrExclusiveDuplicateAttr = errors.New("etree: duplicate attribute with the same expanded name")
)

// The fixed binding of the "xml" prefix. It is always implicitly in scope
// and is never emitted as an explicit declaration.
const xmlNamespaceURI = "http://www.w3.org/XML/1998/namespace"

// ExclusiveOptions configure [CanonicalizeExclusive].
type ExclusiveOptions struct {
	// InclusivePrefixes lists namespace prefixes that are rendered as
	// namespace nodes wherever they are in effect, following the inclusive
	// rules of Canonical XML, instead of only where they are visibly
	// utilized. The token "#default" refers to the default namespace.
	// Prefixes that have no binding at the position where they would be
	// rendered are ignored. Duplicate entries do not produce duplicate
	// declarations. The slice is never modified.
	InclusivePrefixes []string

	// WithComments includes XML comments in the output when true.
	WithComments bool
}

// CanonicalizeExclusive writes the subtree rooted at root to w using the
// Exclusive XML Canonicalization Version 1.0 method
// (http://www.w3.org/2001/10/xml-exc-c14n#). root, all of its descendants,
// its attributes and its namespace axis are serialized; arbitrary XPath
// node-sets are not supported. root may still be attached to a larger
// document: namespace bindings made by its ancestors are consulted when
// resolving prefixes, but only the subtree itself is written.
//
// Namespace declarations are emitted only where a prefix is visibly
// utilized by an element or one of its prefixed attributes, or where the
// prefix appears in opts.InclusivePrefixes. Ancestor bindings that are not
// used by the subtree never appear in the output. Default namespace
// undeclarations (xmlns=""), prefix rebindings and restoration after a
// rebinding are handled per the exclusive rules. The "xml" prefix uses its
// fixed implicit binding and is never declared explicitly; xml: attributes
// on ancestors are never copied onto root.
//
// Output is canonical UTF-8 with double-quoted attribute values, full end
// tags, canonical character escaping and CDATA sections rendered as
// ordinary text. No XML declaration is written, prefix names are preserved
// and no Unicode normalization is performed. Document.WriteSettings and
// indentation have no effect on this function.
//
// The input tree and opts.InclusivePrefixes are never modified; missing
// namespace declarations are not inserted into the tree. Writing happens
// incrementally: on a writer error the function stops immediately and
// returns the number of bytes actually written together with the writer's
// error. A nil error together with a short write is reported as
// io.ErrShortWrite.
//
// An error is returned (and the partial output is not a canonical form) if
// root is nil, a visibly used prefix has no binding, a reserved binding is
// illegal, a relative namespace URI is encountered, or one element carries
// two ordinary attributes with the same expanded name.
func CanonicalizeExclusive(w io.Writer, root *Element, opts ExclusiveOptions) (int64, error) {
	if root == nil {
		return 0, ErrExclusiveNilRoot
	}

	cw := &exclusiveWriter{w: w}
	ec := &exclusiveCanonicalizer{
		w:        cw,
		comments: opts.WithComments,
		incl:     make(map[string]bool, len(opts.InclusivePrefixes)),
		rendered: make(map[string]string),
	}
	for _, p := range opts.InclusivePrefixes {
		ec.incl[p] = true
	}

	ec.canonicalizeElement(root)
	return cw.bytes, cw.err
}

// exclusiveCanonicalizer carries the state of one exclusive
// canonicalization pass.
type exclusiveCanonicalizer struct {
	w        *exclusiveWriter
	comments bool
	incl     map[string]bool // entries of ExclusiveOptions.InclusivePrefixes

	// rendered maps every non-default prefix already rendered by an output
	// ancestor to its bound URI. The default namespace needs separate state
	// because an empty URI (xmlns="") must be distinguishable from a
	// default namespace that has never been rendered.
	rendered map[string]string

	defRendered bool   // an output ancestor rendered a default namespace
	defValue    string // the value of that rendered default namespace
}

// exclusiveWriter forwards every write straight to the underlying writer
// while counting accepted bytes. No buffering is used so the reported
// count is exact at every failure point. Once an error occurs all further
// writes are no-ops. A write that reports no error but fewer bytes than
// offered becomes io.ErrShortWrite.
type exclusiveWriter struct {
	w     io.Writer
	bytes int64
	err   error
}

// fail records err as the first writer error.
func (cw *exclusiveWriter) fail(err error) {
	if cw.err == nil {
		cw.err = err
	}
}

// write sends p to the underlying writer.
func (cw *exclusiveWriter) write(p []byte) {
	if cw.err != nil || len(p) == 0 {
		return
	}
	n, err := cw.w.Write(p)
	if n > 0 {
		cw.bytes += int64(n)
	}
	switch {
	case err != nil:
		cw.fail(err)
	case n != len(p):
		cw.fail(io.ErrShortWrite)
	}
}

// writeString sends s to the underlying writer.
func (cw *exclusiveWriter) writeString(s string) {
	cw.write([]byte(s))
}

// writeByte sends one byte to the underlying writer.
func (cw *exclusiveWriter) writeByte(b byte) {
	cw.write([]byte{b})
}

// canonicalizeElement writes one element and, recursively, its subtree.
func (ec *exclusiveCanonicalizer) canonicalizeElement(e *Element) {
	// Snapshot the rendered-namespace state so sibling subtrees start from
	// the same state their parent established.
	savedRendered := ec.rendered
	savedDefRendered, savedDefValue := ec.defRendered, ec.defValue
	ec.rendered = cloneStringMap(savedRendered)
	defer func() {
		ec.rendered = savedRendered
		ec.defRendered, ec.defValue = savedDefRendered, savedDefValue
	}()

	ec.w.writeByte('<')
	ec.w.writeString(e.FullTag())

	ec.renderNamespaces(e)
	if ec.w.err != nil {
		return
	}
	ec.renderAttributes(e)
	if ec.w.err != nil {
		return
	}

	ec.w.writeByte('>')
	if ec.w.err != nil {
		return
	}

	for _, t := range e.Child {
		switch c := t.(type) {
		case *Element:
			ec.canonicalizeElement(c)
		case *CharData:
			// CDATA sections are replaced by their character content.
			ec.w.writeEscaped(c.Data, false)
		case *Comment:
			if ec.comments {
				ec.w.writeString("<!--")
				ec.w.writeString(c.Data)
				ec.w.writeString("-->")
			}
		case *ProcInst:
			ec.w.writeString("<?")
			ec.w.writeString(c.Target)
			if c.Inst != "" {
				ec.w.writeByte(' ')
				ec.w.writeString(c.Inst)
			}
			ec.w.writeString("?>")
		case *Directive:
			// Directives are DTD-level markup and have no place in the
			// canonical form of an element subtree.
		}
		if ec.w.err != nil {
			return
		}
	}

	ec.w.writeString("</")
	ec.w.writeString(e.FullTag())
	ec.w.writeByte('>')
}

// renderNamespaces writes the namespace declarations required on element e
// by the exclusive rules and updates the rendered-namespace state.
func (ec *exclusiveCanonicalizer) renderNamespaces(e *Element) {
	// Prefixes visibly utilized by the element itself or by its prefixed
	// attributes, with the URI in effect at the element's position. The
	// implicit "xml" prefix is never rendered.
	used := make(map[string]string)
	if e.Space != "" && e.Space != "xml" {
		uri, err := ec.resolvePrefix(e, e.Space)
		if err != nil {
			ec.w.fail(err)
			return
		}
		used[e.Space] = uri
	}
	for _, a := range e.Attr {
		if a.Space == "" || a.Space == "xmlns" || a.Space == "xml" {
			continue
		}
		if _, ok := used[a.Space]; ok {
			continue
		}
		uri, err := ec.resolvePrefix(e, a.Space)
		if err != nil {
			ec.w.fail(err)
			return
		}
		used[a.Space] = uri
	}

	// Validate every local namespace declaration. Whether a declaration
	// is rendered here is decided below from visible utilization and the
	// inclusive prefix list; the tree is never modified.
	for _, a := range e.Attr {
		if a.Space == "xmlns" {
			if err := ec.checkBinding(a.Key, a.Value); err != nil {
				ec.w.fail(err)
				return
			}
		} else if a.Space == "" && a.Key == "xmlns" {
			if err := ec.checkRelativeURI(a.Value); err != nil {
				ec.w.fail(err)
				return
			}
		}
	}

	// The effective default namespace at this element, used both for an
	// unprefixed element's visible utilization and for "#default".
	defURI, defFound, err := ec.lookupDefault(e)
	if err != nil {
		ec.w.fail(err)
		return
	}

	// Prefixes forced onto every element by the inclusive list: only those
	// actually bound at this position, never the implicit "xml" prefix.
	inclusive := make(map[string]string)
	for p := range ec.incl {
		switch {
		case p == "xml":
			continue
		case p == "#default":
			if defFound {
				inclusive[""] = defURI
			}
		default:
			if uri, ok, err := ec.lookupPrefix(e, p); err != nil {
				ec.w.fail(err)
				return
			} else if ok {
				inclusive[p] = uri
			}
		}
	}

	// Collect the non-default prefixes to render. A prefix is rendered when
	// visibly utilized or named on the inclusive list, and either it has
	// never been rendered by an output ancestor or its bound value changes.
	prefixes := make(map[string]string)
	for p, uri := range used {
		if ec.rendered[p] != uri {
			prefixes[p] = uri
		}
	}
	for p, uri := range inclusive {
		if p == "" {
			continue
		}
		if ec.rendered[p] != uri {
			prefixes[p] = uri
		}
	}

	// Decide whether a default-namespace node belongs on this element.
	// The default namespace is rendered only when this element visibly
	// uses it (it has no prefix) or when "#default" is on the inclusive
	// list. A local xmlns on a prefixed element is not visibly used, so it
	// is not rendered here; an unprefixed descendant resolves the binding
	// at its own position and renders it then. A non-empty default is
	// emitted unless an output ancestor already rendered the same value;
	// an empty value is an xmlns="" undeclaration, emitted only against a
	// non-empty default rendered by an output ancestor.
	defaultWanted := (e.Space == "" && defFound) || (ec.incl["#default"] && defFound)
	if defaultWanted {
		switch {
		case defURI != "":
			if !ec.defRendered || ec.defValue != defURI {
				prefixes[""] = defURI
			}
		case ec.defRendered && ec.defValue != "":
			prefixes[""] = ""
		}
	}

	// Namespace nodes are sorted lexicographically by prefix, with the
	// default namespace (empty prefix) first.
	keys := make([]string, 0, len(prefixes))
	for p := range prefixes {
		keys = append(keys, p)
	}
	sort.Strings(keys)

	for _, p := range keys {
		uri := prefixes[p]
		if p == "" {
			ec.w.writeString(` xmlns="`)
		} else {
			ec.w.writeByte(' ')
			ec.w.writeString("xmlns:")
			ec.w.writeString(p)
			ec.w.writeByte('=')
			ec.w.writeByte('"')
		}
		ec.w.writeEscaped(uri, true)
		ec.w.writeByte('"')
		if ec.w.err != nil {
			return
		}
		if p == "" {
			ec.defRendered, ec.defValue = true, uri
		} else {
			ec.rendered[p] = uri
		}
	}

	// A local declaration that merely restates an already rendered value
	// keeps the same state, so no update is needed. A declaration that was
	// not rendered (an unused, non-inclusive prefix) deliberately stays out
	// of the rendered state, otherwise a descendant would skip a
	// declaration its output ancestors never emitted.
}

// renderAttributes writes the ordinary (non-namespace) attributes of e in
// canonical order: namespace URI first, local name second. The tree's
// attribute order is left untouched.
func (ec *exclusiveCanonicalizer) renderAttributes(e *Element) {
	type sortedAttr struct {
		index int
		uri   string
	}
	ordinary := make([]sortedAttr, 0, len(e.Attr))
	for i, a := range e.Attr {
		if a.Space == "xmlns" || (a.Space == "" && a.Key == "xmlns") {
			continue
		}
		uri := ""
		if a.Space != "" {
			u, err := ec.resolvePrefix(e, a.Space)
			if err != nil {
				ec.w.fail(err)
				return
			}
			uri = u
		}
		ordinary = append(ordinary, sortedAttr{index: i, uri: uri})
	}

	sort.SliceStable(ordinary, func(i, j int) bool {
		if ordinary[i].uri != ordinary[j].uri {
			return ordinary[i].uri < ordinary[j].uri
		}
		return e.Attr[ordinary[i].index].Key < e.Attr[ordinary[j].index].Key
	})

	seen := make(map[[2]string]bool, len(ordinary))
	for _, sa := range ordinary {
		a := &e.Attr[sa.index]
		name := [2]string{sa.uri, a.Key}
		if seen[name] {
			ec.w.fail(fmt.Errorf("%w: %s", ErrExclusiveDuplicateAttr, a.FullKey()))
			return
		}
		seen[name] = true

		ec.w.writeByte(' ')
		ec.w.writeString(a.FullKey())
		ec.w.writeString(`="`)
		ec.w.writeEscaped(a.Value, true)
		ec.w.writeByte('"')
		if ec.w.err != nil {
			return
		}
	}
}

// resolvePrefix returns the namespace URI bound to prefix at the position
// of element e, walking e's ancestors when the binding is not local. The
// implicit "xml" binding is supplied automatically. An unbound prefix is an
// error.
func (ec *exclusiveCanonicalizer) resolvePrefix(e *Element, prefix string) (string, error) {
	if uri, ok, err := ec.lookupPrefix(e, prefix); err != nil {
		return "", err
	} else if ok {
		return uri, nil
	}
	return "", fmt.Errorf("%w: %s", ErrExclusiveUnboundPrefix, prefix)
}

// lookupPrefix reports the URI bound to prefix at the position of e and
// whether a binding exists. A binding to the empty string undeclares the
// prefix, so ok is false in that case.
func (ec *exclusiveCanonicalizer) lookupPrefix(e *Element, prefix string) (uri string, ok bool, err error) {
	if prefix == "xml" {
		return xmlNamespaceURI, true, nil
	}
	for seg := e; seg != nil; seg = seg.parent {
		for _, a := range seg.Attr {
			if a.Space == "xmlns" && a.Key == prefix {
				if err := ec.checkBinding(prefix, a.Value); err != nil {
					return "", false, err
				}
				if a.Value == "" {
					return "", false, nil
				}
				return a.Value, true, nil
			}
		}
	}
	return "", false, nil
}

// lookupDefault reports the default namespace URI in effect at e and
// whether a default declaration has been encountered. An explicit xmlns=""
// is reported as found with an empty URI; if no ancestor declares a
// default namespace, found is false.
func (ec *exclusiveCanonicalizer) lookupDefault(e *Element) (uri string, found bool, err error) {
	for seg := e; seg != nil; seg = seg.parent {
		for _, a := range seg.Attr {
			if a.Space == "" && a.Key == "xmlns" {
				if err := ec.checkRelativeURI(a.Value); err != nil {
					return "", false, err
				}
				return a.Value, true, nil
			}
		}
	}
	return "", false, nil
}

// checkBinding validates a single xmlns:prefix declaration.
func (ec *exclusiveCanonicalizer) checkBinding(prefix, uri string) error {
	if prefix == "xml" && uri != xmlNamespaceURI {
		return fmt.Errorf("%w: xmlns:xml must be %q, got %q",
			ErrExclusiveIllegalBinding, xmlNamespaceURI, uri)
	}
	if prefix == "xmlns" {
		return fmt.Errorf("%w: the xmlns prefix cannot be redeclared",
			ErrExclusiveIllegalBinding)
	}
	return ec.checkRelativeURI(uri)
}

// checkRelativeURI rejects relative namespace URI references. Namespace
// names must be absolute URIs; empty strings are allowed because they
// undeclare a binding.
func (ec *exclusiveCanonicalizer) checkRelativeURI(uri string) error {
	if uri == "" {
		return nil
	}
	i := strings.IndexByte(uri, ':')
	if i <= 0 || !isSchemeAlpha(uri[0]) {
		return fmt.Errorf("%w: %q", ErrExclusiveRelativeURI, uri)
	}
	for j := 1; j < i; j++ {
		c := uri[j]
		if !isSchemeAlpha(c) && !isSchemeDigit(c) &&
			c != '+' && c != '-' && c != '.' {
			return fmt.Errorf("%w: %q", ErrExclusiveRelativeURI, uri)
		}
	}
	return nil
}

func isSchemeAlpha(c byte) bool {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
}

func isSchemeDigit(c byte) bool {
	return c >= '0' && c <= '9'
}

// cloneStringMap returns a shallow copy of m.
func cloneStringMap(m map[string]string) map[string]string {
	c := make(map[string]string, len(m))
	for k, v := range m {
		c[k] = v
	}
	return c
}

// writeEscaped writes s with the canonical character escapes. In
// attribute values, tabs, line feeds and carriage returns become character
// references; in text, only the carriage return does.
func (cw *exclusiveWriter) writeEscaped(s string, attr bool) {
	last := 0
	for i := 0; i < len(s); {
		r, width := utf8.DecodeRuneInString(s[i:])
		next := i + width
		var esc string
		switch r {
		case '&':
			esc = "&amp;"
		case '<':
			esc = "&lt;"
		case '>':
			if !attr {
				esc = "&gt;"
			}
		case '"':
			if attr {
				esc = "&quot;"
			}
		case '\t':
			if attr {
				esc = "&#x9;"
			}
		case '\n':
			if attr {
				esc = "&#xA;"
			}
		case '\r':
			esc = "&#xD;"
		default:
			// Mirror the XML character range rules used by the rest of the
			// package: runes outside the legal range, plus decoding-error
			// replacement runes, become U+FFFD.
			if !isInCharacterRange(r) || (r == utf8.RuneError && width == 1) {
				esc = "�"
			}
		}
		if esc != "" {
			cw.writeString(s[last:i])
			cw.writeString(esc)
			last = next
			if cw.err != nil {
				return
			}
		}
		i = next
	}
	cw.writeString(s[last:])
}
