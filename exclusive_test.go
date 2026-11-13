// Copyright 2015-2019 Brett Vickers.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package etree

import (
	"bufio"
	"bytes"
	"errors"
	"io"
	"testing"
)

// mustParse reads src into a fresh document and fails the test on error.
func mustParse(t *testing.T, src string) *Document {
	t.Helper()
	d := NewDocument()
	if err := d.ReadFromString(src); err != nil {
		t.Fatalf("failed to parse %q: %v", src, err)
	}
	return d
}

// exclusiveBytes runs one exclusive canonicalization and returns its
// output, failing the test on any error.
func exclusiveBytes(t *testing.T, root *Element, opts ExclusiveOptions) string {
	t.Helper()
	var buf bytes.Buffer
	n, err := CanonicalizeExclusive(&buf, root, opts)
	if err != nil {
		t.Fatalf("CanonicalizeExclusive failed: %v", err)
	}
	if n != int64(buf.Len()) {
		t.Fatalf("reported byte count %d does not match output length %d", n, buf.Len())
	}
	return buf.String()
}

// collectElements returns every element in root's subtree in document
// order; it is used to prove canonicalization does not alter identities.
func collectElements(root *Element) []*Element {
	var out []*Element
	var walk func(*Element)
	walk = func(e *Element) {
		out = append(out, e)
		for _, ch := range e.Child {
			if ce, ok := ch.(*Element); ok {
				walk(ce)
			}
		}
	}
	walk(root)
	return out
}

// attrKeys returns the attribute QNames of e in their stored order.
func attrKeys(e *Element) []string {
	keys := make([]string, len(e.Attr))
	for i, a := range e.Attr {
		keys[i] = a.FullKey()
	}
	return keys
}

// plainSerialization renders e through the ordinary WriteTo path, which
// must remain completely independent of exclusive canonicalization.
func plainSerialization(e *Element) string {
	var buf bytes.Buffer
	bw := bufio.NewWriter(&buf)
	e.WriteTo(bw, &WriteSettings{CanonicalEndTags: true, CanonicalText: true, CanonicalAttrVal: true})
	_ = bw.Flush()
	return buf.String()
}

// treeSnapshot captures the parts of a live element subtree that exclusive
// canonicalization must leave untouched: element identities, parent
// pointers, attribute order and ordinary serialization.
type treeSnapshot struct {
	elements []*Element
	parents  []*Element
	attrKeys [][]string
	plain    string
}

// snapshotTree records the current state of root.
func snapshotTree(root *Element) treeSnapshot {
	els := collectElements(root)
	s := treeSnapshot{
		elements: els,
		parents:  make([]*Element, len(els)),
		attrKeys: make([][]string, len(els)),
		plain:    plainSerialization(root),
	}
	for i, e := range els {
		s.parents[i] = e.Parent()
		s.attrKeys[i] = attrKeys(e)
	}
	return s
}

// assertUntouched fails if root's current state differs from the snapshot.
func assertUntouched(t *testing.T, root *Element, s treeSnapshot) {
	t.Helper()
	now := snapshotTree(root)
	if len(now.elements) != len(s.elements) {
		t.Fatalf("element count changed: %d before, %d after", len(s.elements), len(now.elements))
	}
	for i := range s.elements {
		if s.elements[i] != now.elements[i] {
			t.Fatalf("element identity changed at index %d", i)
		}
		if s.parents[i] != now.parents[i] {
			t.Fatalf("parent identity changed on %s", s.elements[i].FullTag())
		}
		bk, ak := s.attrKeys[i], now.attrKeys[i]
		if len(bk) != len(ak) {
			t.Fatalf("attribute count changed on %s", s.elements[i].FullTag())
		}
		for j := range bk {
			if bk[j] != ak[j] {
				t.Fatalf("attribute order changed on %s: %v -> %v",
					s.elements[i].FullTag(), bk, ak)
			}
		}
	}
	if s.plain != now.plain {
		t.Fatalf("ordinary serialization changed:\nbefore %q\nafter  %q", s.plain, now.plain)
	}
}

// TestExclusiveNamespaces covers the Exclusive C14N 1.0 namespace rules:
// visible utilization, ancestor bindings, rebinding and restoration,
// default namespace undeclaration, the inclusive prefix list, the implicit
// xml prefix, and the documented error conditions.
func TestExclusiveNamespaces(t *testing.T) {
	cases := []struct {
		name string
		xml  string
		pick func(d *Document) *Element
		opts ExclusiveOptions
		want string
	}{
		{
			// Exclusive C14N 1.0, section 2.1: the ancestor n0 binding is
			// not imported; only the visibly used n1 binding appears.
			name: "spec 2.1 simple enveloping",
			xml:  `<n0:pdu xmlns:n0="http://a.example"><n1:elem1 xmlns:n1="http://b.example">content</n1:elem1></n0:pdu>`,
			pick: func(d *Document) *Element { return d.Root().ChildElements()[0] },
			want: `<n1:elem1 xmlns:n1="http://b.example">content</n1:elem1>`,
		},
		{
			// Exclusive C14N 1.0, section 2.2, first enveloping document.
			name: "spec 2.2 elem2 first context",
			xml:  `<n0:local xmlns:n0="foo:bar" xmlns:n3="ftp://example.org"><n1:elem2 xmlns:n1="http://example.net" xml:lang="en"><n3:stuff xmlns:n3="ftp://example.org"/></n1:elem2></n0:local>`,
			pick: func(d *Document) *Element { return d.Root().ChildElements()[0] },
			want: `<n1:elem2 xmlns:n1="http://example.net" xml:lang="en"><n3:stuff xmlns:n3="ftp://example.org"></n3:stuff></n1:elem2>`,
		},
		{
			// Section 2.2, second enveloping document: the same subtree
			// must produce the identical octet sequence; the ancestor
			// xml:lang/xml:space attributes are not imported.
			name: "spec 2.2 elem2 second context",
			xml:  `<n2:pdu xmlns:n1="http://example.com" xmlns:n2="http://foo.example" xml:lang="fr" xml:space="retain"><n1:elem2 xmlns:n1="http://example.net" xml:lang="en"><n3:stuff xmlns:n3="ftp://example.org"/></n1:elem2></n2:pdu>`,
			pick: func(d *Document) *Element { return d.Root().ChildElements()[0] },
			want: `<n1:elem2 xmlns:n1="http://example.net" xml:lang="en"><n3:stuff xmlns:n3="ftp://example.org"></n3:stuff></n1:elem2>`,
		},
		{
			// The apex itself relies on a binding made only by an
			// ancestor; the effective binding is pulled onto the apex.
			name: "ancestor binding used by apex",
			xml:  `<outer xmlns:p="urn:p"><p:payload><p:child/></p:payload></outer>`,
			pick: func(d *Document) *Element { return d.Root().ChildElements()[0] },
			want: `<p:payload xmlns:p="urn:p"><p:child></p:child></p:payload>`,
		},
		{
			// Unused ancestor bindings never leak into the subtree.
			name: "unused ancestor binding excluded",
			xml:  `<outer xmlns:p="urn:p" xmlns:q="urn:q"><payload/></outer>`,
			pick: func(d *Document) *Element { return d.Root().ChildElements()[0] },
			want: `<payload></payload>`,
		},
		{
			// Two adjacent branches each rebind the same prefix.
			name: "two adjacent branches rebind prefix",
			xml:  `<root xmlns:n="urn:one"><a><n:x/></a><b><n:x xmlns:n="urn:two"/></b></root>`,
			pick: func(d *Document) *Element { return d.Root() },
			want: `<root><a><n:x xmlns:n="urn:one"></n:x></a><b><n:x xmlns:n="urn:two"></n:x></b></root>`,
		},
		{
			// After leaving a rebinding branch, the sibling sees the
			// original binding restored.
			name: "rebinding restored at sibling",
			xml:  `<r xmlns:n="urn:one"><a><n:x xmlns:n="urn:two"/></a><b><n:y/></b></r>`,
			pick: func(d *Document) *Element { return d.Root() },
			want: `<r><a><n:x xmlns:n="urn:two"></n:x></a><b><n:y xmlns:n="urn:one"></n:y></b></r>`,
		},
		{
			// A nested chain of rebindings unwinds correctly.
			name: "nested rebind restore chain",
			xml:  `<r xmlns:n="urn:1"><a xmlns:n="urn:2"><b xmlns:n="urn:3"><n:x/></b><n:y/></a></r>`,
			pick: func(d *Document) *Element { return d.Root() },
			want: `<r><a><b><n:x xmlns:n="urn:3"></n:x></b><n:y xmlns:n="urn:2"></n:y></a></r>`,
		},
		{
			// Default namespace undeclaration and later restoration.
			name: "default namespace undeclared and restored",
			xml:  `<r xmlns="urn:d"><a xmlns=""><b/></a><c/></r>`,
			pick: func(d *Document) *Element { return d.Root() },
			want: `<r xmlns="urn:d"><a xmlns=""><b></b></a><c></c></r>`,
		},
		{
			// An unprefixed apex inherits the ancestor default namespace.
			name: "apex inherits ancestor default",
			xml:  `<r xmlns="urn:d"><a><b/></a></r>`,
			pick: func(d *Document) *Element { return d.Root().ChildElements()[0].ChildElements()[0] },
			want: `<b xmlns="urn:d"></b>`,
		},
		{
			// A local declaration restating the rendered value is not
			// repeated, but descendants still see the binding.
			name: "restated local declaration omitted",
			xml:  `<r xmlns:n="urn:n"><a xmlns:n="urn:n"><n:x/></a></r>`,
			pick: func(d *Document) *Element { return d.Root() },
			want: `<r><a><n:x xmlns:n="urn:n"></n:x></a></r>`,
		},
		{
			// Inclusive prefixes force unused bindings onto the apex;
			// duplicates collapse and a prefix absent at this position is
			// ignored.
			name: "inclusive list duplicates and missing prefix",
			xml:  `<r xmlns:n="urn:n" xmlns:m="urn:m"><a/></r>`,
			pick: func(d *Document) *Element { return d.Root().ChildElements()[0] },
			opts: ExclusiveOptions{InclusivePrefixes: []string{"n", "n", "m", "ghost"}},
			want: `<a xmlns:m="urn:m" xmlns:n="urn:n"></a>`,
		},
		{
			// "#default" includes the inherited default namespace.
			name: "inclusive default token",
			xml:  `<r xmlns="urn:d"><a/></r>`,
			pick: func(d *Document) *Element { return d.Root().ChildElements()[0] },
			opts: ExclusiveOptions{InclusivePrefixes: []string{"#default"}},
			want: `<a xmlns="urn:d"></a>`,
		},
		{
			// "#default" is ignored when no default namespace is in
			// effect.
			name: "inclusive default token absent",
			xml:  `<r xmlns:p="urn:p"><a/></r>`,
			pick: func(d *Document) *Element { return d.Root().ChildElements()[0] },
			opts: ExclusiveOptions{InclusivePrefixes: []string{"#default", "p"}},
			want: `<a xmlns:p="urn:p"></a>`,
		},
		{
			// An inclusive prefix that has been undeclared here is
			// ignored rather than emitted as an empty declaration.
			name: "inclusive prefix undeclared in subtree",
			xml:  `<r xmlns:p="urn:p"><a xmlns:p=""><b/></a></r>`,
			pick: func(d *Document) *Element { return d.Root().ChildElements()[0] },
			opts: ExclusiveOptions{InclusivePrefixes: []string{"p"}},
			want: `<a><b></b></a>`,
		},
		{
			// Inclusive prefixes follow every rebinding down the tree.
			name: "inclusive prefix tracks rebinding",
			xml:  `<r xmlns:n="urn:one"><a xmlns:n="urn:two"><b/></a></r>`,
			pick: func(d *Document) *Element { return d.Root() },
			opts: ExclusiveOptions{InclusivePrefixes: []string{"n"}},
			want: `<r xmlns:n="urn:one"><a xmlns:n="urn:two"><b></b></a></r>`,
		},
		{
			// A prefix appearing only inside an attribute value string is
			// not visibly utilized; the unprefixed attribute is still
			// emitted, but the unused ancestor binding is not. Retaining
			// that binding requires the inclusive list.
			name: "qname string not visible without inclusive list",
			xml:  `<r xmlns:p="urn:p"><e a="p:Type"/></r>`,
			pick: func(d *Document) *Element { return d.Root().ChildElements()[0] },
			want: `<e a="p:Type"></e>`,
		},
		{
			name: "qname string retained via inclusive list",
			xml:  `<r xmlns:p="urn:p"><e a="p:Type"/></r>`,
			pick: func(d *Document) *Element { return d.Root().ChildElements()[0] },
			opts: ExclusiveOptions{InclusivePrefixes: []string{"p"}},
			want: `<e xmlns:p="urn:p" a="p:Type"></e>`,
		},
		{
			// The xml prefix uses its fixed implicit binding: no
			// xmlns:xml is emitted, and ancestor xml: attributes are not
			// copied onto the selected root.
			name: "implicit xml binding and no inherited xml attrs",
			xml:  `<outer xml:lang="fr" xml:space="preserve"><p:payload xmlns:p="urn:p" xml:lang="en"/></outer>`,
			pick: func(d *Document) *Element { return d.Root().ChildElements()[0] },
			want: `<p:payload xmlns:p="urn:p" xml:lang="en"></p:payload>`,
		},
		{
			// Unprefixed attributes stay in no namespace even when a
			// default namespace is in effect.
			name: "unprefixed attributes outside default namespace",
			xml:  `<e xmlns="urn:d" z="1" a="2"/>`,
			pick: func(d *Document) *Element { return d.Root() },
			want: `<e xmlns="urn:d" a="2" z="1"></e>`,
		},
		{
			// A local xmlns on a prefixed element is not visibly used by
			// the element, so it is not rendered there.
			name: "unused local default on prefixed element omitted",
			xml:  `<n:root xmlns:n="urn:n" xmlns="urn:d"><n:x/></n:root>`,
			pick: func(d *Document) *Element { return d.Root() },
			want: `<n:root xmlns:n="urn:n"><n:x></n:x></n:root>`,
		},
		{
			// The same local default is picked up by an unprefixed
			// descendant at its own position and rendered then.
			name: "local default rendered at unprefixed descendant",
			xml:  `<n:root xmlns:n="urn:n" xmlns="urn:d"><child/></n:root>`,
			pick: func(d *Document) *Element { return d.Root() },
			want: `<n:root xmlns:n="urn:n"><child xmlns="urn:d"></child></n:root>`,
		},
		{
			// "#default" forces the default node onto a prefixed element
			// that would otherwise not render it.
			name: "inclusive default forces node on prefixed element",
			xml:  `<n:root xmlns:n="urn:n" xmlns="urn:d"/>`,
			pick: func(d *Document) *Element { return d.Root() },
			opts: ExclusiveOptions{InclusivePrefixes: []string{"#default"}},
			want: `<n:root xmlns="urn:d" xmlns:n="urn:n"></n:root>`,
		},
		{
			// An element in the implicit xml namespace needs no
			// declaration at all.
			name: "xml prefixed element needs no declaration",
			xml:  `<xml:foo xml:lang="en"/>`,
			pick: func(d *Document) *Element { return d.Root() },
			want: `<xml:foo xml:lang="en"></xml:foo>`,
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			d := mustParse(t, tc.xml)
			root := tc.pick(d)
			before := snapshotTree(root)

			inclusive := append([]string(nil), tc.opts.InclusivePrefixes...)
			got := exclusiveBytes(t, root, tc.opts)
			if got != tc.want {
				t.Fatalf("exclusive output mismatch:\ngot  %q\nwant %q", got, tc.want)
			}
			// A second pass over the untouched tree is byte-stable.
			if again := exclusiveBytes(t, root, tc.opts); again != got {
				t.Fatalf("non-deterministic output:\nfirst %q\nthen  %q", got, again)
			}
			assertUntouched(t, root, before)
			assertStringSliceEqual(t, inclusive, tc.opts.InclusivePrefixes)
		})
	}

	// Re-mounting a subtree under a different ancestor must resolve
	// bindings at the new position without dragging in unrelated bindings.
	t.Run("subtree remounted under different ancestor", func(t *testing.T) {
		d1 := mustParse(t, `<wrap xmlns:p="urn:p"><p:child><x/></p:child></wrap>`)
		sub := d1.Root().ChildElements()[0]
		d2 := mustParse(t, `<other xmlns:q="urn:q" xmlns:p="urn:DIFFERENT"/>`)
		d2.Root().AddChild(sub)
		before := snapshotTree(sub)

		got := exclusiveBytes(t, sub, ExclusiveOptions{})
		want := `<p:child xmlns:p="urn:DIFFERENT"><x></x></p:child>`
		if got != want {
			t.Fatalf("remounted subtree:\ngot  %q\nwant %q", got, want)
		}
		assertUntouched(t, sub, before)
	})

	// Error conditions: partial output is acceptable, but the returned
	// error must never allow the result to be mistaken for canonical form.
	t.Run("error conditions", func(t *testing.T) {
		if _, err := CanonicalizeExclusive(io.Discard, nil, ExclusiveOptions{}); !errors.Is(err, ErrExclusiveNilRoot) {
			t.Fatalf("nil root: got %v, want ErrExclusiveNilRoot", err)
		}

		errCases := []struct {
			name string
			xml  string
			want error
		}{
			{"unbound element prefix", `<a><p:x/></a>`, ErrExclusiveUnboundPrefix},
			{"unbound attribute prefix", `<a><x p:k="v"/></a>`, ErrExclusiveUnboundPrefix},
			{"illegal xml binding", `<a xmlns:xml="http://example.com"><b/></a>`, ErrExclusiveIllegalBinding},
			{"xmlns prefix rebound", `<a xmlns:xmlns="urn:x"><b/></a>`, ErrExclusiveIllegalBinding},
			{"relative prefixed URI", `<a xmlns:p="rel/ns"><p:b/></a>`, ErrExclusiveRelativeURI},
			{"relative default URI", `<a xmlns="rel"><b/></a>`, ErrExclusiveRelativeURI},
		}
		for _, ec := range errCases {
			t.Run(ec.name, func(t *testing.T) {
				d := mustParse(t, ec.xml)
				var buf bytes.Buffer
				_, err := CanonicalizeExclusive(&buf, d.Root(), ExclusiveOptions{})
				if !errors.Is(err, ec.want) {
					t.Fatalf("got %v, want %v", err, ec.want)
				}
			})
		}

		// Two ordinary attributes with the same expanded name. The
		// parser deduplicates these, so construct the tree directly.
		d := mustParse(t, `<e xmlns:a="urn:x" xmlns:b="urn:x"/>`)
		e := d.Root()
		e.addAttr("a", "dup", "1")
		e.addAttr("b", "dup", "2")
		before := snapshotTree(e)
		var buf bytes.Buffer
		if _, err := CanonicalizeExclusive(&buf, e, ExclusiveOptions{}); !errors.Is(err, ErrExclusiveDuplicateAttr) {
			t.Fatalf("duplicate expanded-name attribute: got %v", err)
		}
		// The failed call left the tree exactly as it was.
		assertUntouched(t, e, before)
	})
}

func assertStringSliceEqual(t *testing.T, want, got []string) {
	t.Helper()
	if len(want) != len(got) {
		t.Fatalf("slice length changed: %v -> %v", want, got)
	}
	for i := range want {
		if want[i] != got[i] {
			t.Fatalf("slice content changed: %v -> %v", want, got)
		}
	}
}

// TestExclusiveOctets fixes the exact canonical bytes for escaping,
// ordering, CDATA handling, comments, processing instructions and UTF-8
// output.
func TestExclusiveOctets(t *testing.T) {
	cases := []struct {
		name       string
		xml        string
		preserveCD bool
		pick       func(d *Document) *Element
		opts       ExclusiveOptions
		want       string
	}{
		{
			// Canonical XML 1.0 section 3.3 ordering: namespace nodes
			// first (default, then prefixes lexicographically), then
			// attributes by namespace URI and local name.
			name: "attribute URI ordering",
			xml:  `<e5 b:attr="sorted" a:attr="out" attr2="all" xmlns:b="http://www.ietf.org" xmlns:a="http://www.w3.org" xmlns="http://example.org"/>`,
			pick: func(d *Document) *Element { return d.Root() },
			want: `<e5 xmlns="http://example.org" xmlns:a="http://www.w3.org" xmlns:b="http://www.ietf.org" attr2="all" b:attr="sorted" a:attr="out"></e5>`,
		},
		{
			name: "same-URI attributes sorted by local name",
			xml:  `<e xmlns:a="urn:a"><a:zebra a:apple="1" a:mango="2"/></e>`,
			pick: func(d *Document) *Element { return d.Root().ChildElements()[0] },
			want: `<a:zebra xmlns:a="urn:a" a:apple="1" a:mango="2"></a:zebra>`,
		},
		{
			// Canonical XML 1.0 section 3.4: CDATA is replaced by its
			// character content, with text escaping.
			name:       "cdata replaced by escaped text",
			xml:        `<compute><![CDATA[value>"0" && value<"10"]]></compute>`,
			preserveCD: true,
			pick:       func(d *Document) *Element { return d.Root() },
			want:       `<compute>value&gt;"0" &amp;&amp; value&lt;"10"</compute>`,
		},
		{
			// Even with PreserveCData off, a CDATA token renders as text.
			name: "cdata default read path",
			xml:  `<e><![CDATA[a <b> & c]]></e>`,
			pick: func(d *Document) *Element { return d.Root() },
			want: `<e>a &lt;b&gt; &amp; c</e>`,
		},
		{
			name: "text character escapes",
			xml:  `<e>a &amp; b &lt; c &gt; d</e>`,
			pick: func(d *Document) *Element { return d.Root() },
			want: `<e>a &amp; b &lt; c &gt; d</e>`,
		},
		{
			name: "attribute character escapes",
			xml:  `<e a="x &amp; &lt; &quot; y"/>`,
			pick: func(d *Document) *Element { return d.Root() },
			want: `<e a="x &amp; &lt; &quot; y"></e>`,
		},
		{
			// CR, LF and tab in attribute values become character
			// references; only CR does so in text (Canonical XML 1.0
			// section 3.4).
			name: "cr lf tab handling",
			xml:  `<norm attr="&#xD;&#xA;&#x9;"/>`,
			pick: func(d *Document) *Element { return d.Root() },
			want: "<norm attr=\"&#xD;&#xA;&#x9;\"></norm>",
		},
		{
			name: "comments omitted by default",
			xml:  `<doc>Hello<!-- c --><a/></doc>`,
			pick: func(d *Document) *Element { return d.Root() },
			want: `<doc>Hello<a></a></doc>`,
		},
		{
			name: "comments retained with option",
			xml:  `<doc>Hello<!-- c --><a/></doc>`,
			pick: func(d *Document) *Element { return d.Root() },
			opts: ExclusiveOptions{WithComments: true},
			want: `<doc>Hello<!-- c --><a></a></doc>`,
		},
		{
			// Processing instructions inside the subtree are retained
			// regardless of the comment option; directives are dropped.
			name: "processing instruction retained",
			xml:  `<root><?app go?><a/></root>`,
			pick: func(d *Document) *Element { return d.Root() },
			want: `<root><?app go?><a></a></root>`,
		},
		{
			name: "pi without instruction body",
			xml:  `<root><?app?><a/></root>`,
			pick: func(d *Document) *Element { return d.Root() },
			want: `<root><?app?><a></a></root>`,
		},
		{
			// Whitespace text nodes and child order are preserved
			// exactly.
			name: "whitespace and child order preserved",
			xml:  "<r>  text\n\t<a/>  <b/>tail</r>",
			pick: func(d *Document) *Element { return d.Root() },
			want: "<r>  text\n\t<a></a>  <b></b>tail</r>",
		},
		{
			// No declaration, no normalization, canonical UTF-8.
			name: "utf8 passthrough",
			xml:  `<e>© café — 日本語</e>`,
			pick: func(d *Document) *Element { return d.Root() },
			want: `<e>© café — 日本語</e>`,
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			d := NewDocument()
			d.ReadSettings.PreserveCData = tc.preserveCD
			if err := d.ReadFromString(tc.xml); err != nil {
				t.Fatal(err)
			}
			root := tc.pick(d)
			before := snapshotTree(root)

			var buf bytes.Buffer
			n, err := CanonicalizeExclusive(&buf, root, tc.opts)
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(buf.Bytes(), []byte(tc.want)) {
				t.Fatalf("canonical bytes mismatch:\ngot  %q\nwant %q", buf.String(), tc.want)
			}
			if n != int64(len(tc.want)) {
				t.Fatalf("byte count %d, want %d", n, len(tc.want))
			}

			// Idempotence: parsing the canonical bytes again and
			// re-canonicalizing them yields the same sequence. This is a
			// secondary check; the literal expectation above is primary.
			d2 := NewDocument()
			if err := d2.ReadFromString(tc.want); err != nil {
				t.Fatalf("expected output is not parseable XML: %v", err)
			}
			if again := exclusiveBytes(t, d2.Root(), tc.opts); again != tc.want {
				t.Fatalf("not idempotent:\ngot  %q\nwant %q", again, tc.want)
			}

			assertUntouched(t, root, before)
		})
	}

	// Document.WriteSettings and AttrSingleQuote must not influence the
	// canonical bytes. Indent settings are deliberately not exercised
	// here: Indent mutates the tree by adding whitespace text nodes,
	// which the canonical form correctly preserves; this check varies only
	// the serialization settings.
	t.Run("independent of document write settings", func(t *testing.T) {
		d := mustParse(t, `<r xmlns:p="urn:p"><p:a z="1" a="2">x &amp; y</p:a></r>`)
		d.WriteSettings.AttrSingleQuote = true
		d.WriteSettings.CanonicalEndTags = false
		d.WriteSettings.CanonicalText = false
		d.WriteSettings.CanonicalAttrVal = false
		root := d.Root()

		// The ordinary WriteTo path honors the document's live settings:
		// single quotes and a self-closing end tag appear here.
		var ordBuf bytes.Buffer
		bw := bufio.NewWriter(&ordBuf)
		root.WriteTo(bw, &d.WriteSettings)
		_ = bw.Flush()
		if ordBuf.String() != `<r xmlns:p='urn:p'><p:a z='1' a='2'>x &amp; y</p:a></r>` {
			t.Fatalf("ordinary serialization did not use the live settings: %q", ordBuf.String())
		}

		got := exclusiveBytes(t, root, ExclusiveOptions{})
		want := `<r><p:a xmlns:p="urn:p" a="2" z="1">x &amp; y</p:a></r>`
		if got != want {
			t.Fatalf("write settings leaked into canonical output:\ngot  %q\nwant %q", got, want)
		}
	})

	// Tab and newline survive literally in text but are encoded in
	// attributes; a literal CR in text is encoded.
	t.Run("literal control characters", func(t *testing.T) {
		d := NewDocument()
		if err := d.ReadFromString(`<e/>`); err != nil {
			t.Fatal(err)
		}
		root := d.Root()
		root.addAttr("", "a", "x\ty\nz\rw")
		root.CreateText("a\tb\nc\rd")
		got := exclusiveBytes(t, root, ExclusiveOptions{})
		want := "<e a=\"x&#x9;y&#xA;z&#xD;w\">a\tb\nc&#xD;d</e>"
		if got != want {
			t.Fatalf("control character encoding:\ngot  %q\nwant %q", got, want)
		}
	})
}

// limitWriter accepts at most limit bytes total. Once the limit is reached
// it returns errOnFail (which may be nil to model a short write).
type limitWriter struct {
	limit     int
	errOnFail error
	buf       bytes.Buffer
}

func (lw *limitWriter) Write(p []byte) (int, error) {
	avail := lw.limit - lw.buf.Len()
	if avail <= 0 {
		return 0, lw.errOnFail
	}
	if len(p) > avail {
		lw.buf.Write(p[:avail])
		return avail, lw.errOnFail
	}
	lw.buf.Write(p)
	return len(p), nil
}

// TestExclusiveWriter verifies incremental writing: failures at every cut
// point, exact byte counts, io.ErrShortWrite for error-free short writes,
// and successful re-output onto a replacement writer after a failure.
func TestExclusiveWriter(t *testing.T) {
	d := mustParse(t, `<r xmlns:p="urn:p" xml:lang="en"><p:a x="1">text</p:a><!-- c --><b/></r>`)
	root := d.Root()
	before := snapshotTree(root)

	var full bytes.Buffer
	n, err := CanonicalizeExclusive(&full, root, ExclusiveOptions{WithComments: true})
	if err != nil {
		t.Fatal(err)
	}
	if n != int64(full.Len()) {
		t.Fatalf("full byte count mismatch: %d vs %d", n, full.Len())
	}
	fullBytes := full.String()

	myErr := errors.New("synthetic writer failure")

	// Every cut point before the final byte must surface the custom
	// error, report exactly the bytes accepted, and leave a correct
	// prefix behind.
	for limit := 0; limit < len(fullBytes); limit++ {
		lw := &limitWriter{limit: limit, errOnFail: myErr}
		gotN, gotErr := CanonicalizeExclusive(lw, root, ExclusiveOptions{WithComments: true})
		if !errors.Is(gotErr, myErr) {
			t.Fatalf("limit %d: error %v, want %v", limit, gotErr, myErr)
		}
		if gotN != int64(limit) {
			t.Fatalf("limit %d: reported %d bytes", limit, gotN)
		}
		if lw.buf.String() != fullBytes[:limit] {
			t.Fatalf("limit %d: prefix mismatch\ngot  %q\nwant %q", limit, lw.buf.String(), fullBytes[:limit])
		}
	}

	// A writer that fails immediately returns zero bytes and the error.
	lw := &limitWriter{limit: 0, errOnFail: myErr}
	if gotN, gotErr := CanonicalizeExclusive(lw, root, ExclusiveOptions{}); gotN != 0 || !errors.Is(gotErr, myErr) {
		t.Fatalf("immediate failure: n=%d err=%v", gotN, gotErr)
	}

	// An error-free short write is translated to io.ErrShortWrite.
	sw := &limitWriter{limit: 7, errOnFail: nil}
	gotN, gotErr := CanonicalizeExclusive(sw, root, ExclusiveOptions{})
	if !errors.Is(gotErr, io.ErrShortWrite) || gotN != 7 {
		t.Fatalf("short write: n=%d err=%v", gotN, gotErr)
	}

	// After a mid-stream failure, re-output onto a fresh writer must
	// produce the complete canonical sequence.
	fail := &limitWriter{limit: 11, errOnFail: myErr}
	if n1, err1 := CanonicalizeExclusive(fail, root, ExclusiveOptions{WithComments: true}); !errors.Is(err1, myErr) || n1 != 11 {
		t.Fatalf("first attempt: n=%d err=%v", n1, err1)
	}
	var retry bytes.Buffer
	n2, err2 := CanonicalizeExclusive(&retry, root, ExclusiveOptions{WithComments: true})
	if err2 != nil || n2 != int64(len(fullBytes)) || retry.String() != fullBytes {
		t.Fatalf("retry after failure: n=%d err=%v out=%q", n2, err2, retry.String())
	}

	// The tree is still intact and ordinary serialization is unchanged.
	assertUntouched(t, root, before)
}
