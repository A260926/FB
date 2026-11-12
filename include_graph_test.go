// Copyright (c) The microformats project authors.
// SPDX-License-Identifier: MIT

// Tests for the backwards-compatible include pattern (a.include,
// object.include, itemref, and td headers) treated as a reference graph:
// expansion order, shared fragments, duplicate references, cycles, and
// parse stability.  All tests go through the public Parse/ParseNode entry
// points and verify both the parsed output and that the input tree is left
// structurally unmodified.
package microformats_test

import (
	"bytes"
	"encoding/json"
	"net/url"
	"strings"
	"testing"

	"github.com/google/go-cmp/cmp"
	"golang.org/x/net/html"
	"willnorris.com/go/microformats"
)

var includeTestBase = mustURL("https://capture.test/dir/page")

func mustURL(s string) *url.URL {
	u, err := url.Parse(s)
	if err != nil {
		panic(err)
	}
	return u
}

func parseIncludeDoc(t *testing.T, s string) *html.Node {
	t.Helper()
	doc, err := html.Parse(strings.NewReader(s))
	if err != nil {
		t.Fatalf("error parsing HTML: %v", err)
	}
	return doc
}

func renderTree(t *testing.T, n *html.Node) string {
	t.Helper()
	var b bytes.Buffer
	if err := html.Render(&b, n); err != nil {
		t.Fatalf("error rendering tree: %v", err)
	}
	return b.String()
}

func mustJSON(t *testing.T, v any) string {
	t.Helper()
	b, err := json.Marshal(v)
	if err != nil {
		t.Fatalf("error marshaling JSON: %v", err)
	}
	return string(b)
}

// checkUnmodified fails the test if the tree rooted at doc differs from the
// rendering captured before parsing.
func checkUnmodified(t *testing.T, doc *html.Node, before string) {
	t.Helper()
	if after := renderTree(t, doc); after != before {
		t.Errorf("input tree modified by parsing:\nbefore: %s\nafter:  %s", before, after)
	}
}

func attrVal(n *html.Node, key string) string {
	for _, a := range n.Attr {
		if a.Key == key {
			return a.Val
		}
	}
	return ""
}

// findByID returns the first element in n's subtree with the given id, in
// document order.
func findByID(n *html.Node, id string) *html.Node {
	if n.Type == html.ElementNode && attrVal(n, "id") == id {
		return n
	}
	for c := n.FirstChild; c != nil; c = c.NextSibling {
		if f := findByID(c, id); f != nil {
			return f
		}
	}
	return nil
}

// findByClass returns the first element in n's subtree carrying class, in
// document order.
func findByClass(n *html.Node, class string) *html.Node {
	if n.Type == html.ElementNode {
		for _, c := range strings.Fields(attrVal(n, "class")) {
			if c == class {
				return n
			}
		}
	}
	for c := n.FirstChild; c != nil; c = c.NextSibling {
		if f := findByClass(c, class); f != nil {
			return f
		}
	}
	return nil
}

// findByTag returns the first element in n's subtree with the given tag
// name, in document order.
func findByTag(n *html.Node, tag string) *html.Node {
	if n.Type == html.ElementNode && n.Data == tag {
		return n
	}
	for c := n.FirstChild; c != nil; c = c.NextSibling {
		if f := findByTag(c, tag); f != nil {
			return f
		}
	}
	return nil
}

// propStrings returns the string values of the named property of item.
func propStrings(item *microformats.Microformat, name string) []string {
	var values []string
	for _, v := range item.Properties[name] {
		if s, ok := v.(string); ok {
			values = append(values, s)
		}
	}
	return values
}

// singleItem returns the sole top-level microformat item in data.
func singleItem(t *testing.T, data *microformats.Data) *microformats.Microformat {
	t.Helper()
	if len(data.Items) != 1 {
		t.Fatalf("got %d top-level items, want 1: %s", len(data.Items), mustJSON(t, data))
	}
	return data.Items[0]
}

// checkProp asserts the string values of the named property on item.
func checkProp(t *testing.T, item *microformats.Microformat, name string, want []string) {
	t.Helper()
	if got := propStrings(item, name); !cmp.Equal(got, want) {
		t.Errorf("property %q = %v, want %v (item: %s)", name, got, want, mustJSON(t, item))
	}
}

// TestIncludeGraphItemrefAppendsInTokenOrder verifies that itemref
// references are appended after the node's own children, in the order the
// tokens appear in the attribute rather than in document order.
func TestIncludeGraphItemrefAppendsInTokenOrder(t *testing.T) {
	doc := parseIncludeDoc(t, `<div class="vcard" itemref="z a"><span class="fn">Ann</span><span class="nickname">N</span></div><div id="z"><span class="nickname">Z</span></div><div id="a"><span class="nickname">A</span></div>`)
	before := renderTree(t, doc)

	data := microformats.ParseNode(doc, includeTestBase)
	item := singleItem(t, data)
	checkProp(t, item, "name", []string{"Ann"})
	checkProp(t, item, "nickname", []string{"N", "Z", "A"})

	checkUnmodified(t, doc, before)
}

// TestIncludeGraphTableHeaders verifies that a td headers reference appends
// the referenced content to the node, like itemref does.
func TestIncludeGraphTableHeaders(t *testing.T) {
	doc := parseIncludeDoc(t, `<table><tr><th id="org"><span class="org">Opera</span></th></tr><tr><td class="vcard" headers="org"><span class="fn">Chris</span></td></tr></table>`)
	before := renderTree(t, doc)

	data := microformats.ParseNode(doc, includeTestBase)
	item := singleItem(t, data)
	checkProp(t, item, "name", []string{"Chris"})
	checkProp(t, item, "org", []string{"Opera"})

	checkUnmodified(t, doc, before)
}

// TestIncludeGraphIncludeReplacesPlaceholder verifies that a.include and
// object.include replace the placeholder element with the referenced
// element: the replacement's own v1 properties (fn, url) take effect, not
// just its children's, while the placeholder's text and rel no longer
// contribute.
func TestIncludeGraphIncludeReplacesPlaceholder(t *testing.T) {
	doc := parseIncludeDoc(t, `<div class="vcard"><a class="include" rel="me" href="#x">placeholder text</a><object class="include" data="#y"></object></div><span id="x" class="fn">Ann</span><a id="y" class="url" href="/ann">site</a>`)
	before := renderTree(t, doc)

	data := microformats.ParseNode(doc, includeTestBase)
	item := singleItem(t, data)

	// the replacement targets carry fn and url on themselves.
	checkProp(t, item, "name", []string{"Ann"})
	checkProp(t, item, "url", []string{"https://capture.test/ann"})

	// the replaced placeholder's rel contributes nothing.
	if len(data.Rels) != 0 {
		t.Errorf("Rels = %v, want empty (replaced placeholder's rel must not contribute)", data.Rels)
	}

	// the placeholder is only gone from the expanded copy, not the input.
	checkUnmodified(t, doc, before)
	if !strings.Contains(before, "placeholder text") {
		t.Errorf("placeholder text missing from input tree after parse: %s", before)
	}
}

// TestIncludeGraphSharedFragmentAcrossRootTypes verifies that a fragment
// shared by different v1 roots is parsed fresh under each root's own
// property mapping, contributing to both roots.
func TestIncludeGraphSharedFragmentAcrossRootTypes(t *testing.T) {
	doc := parseIncludeDoc(t, `<div class="hentry" itemref="shared"><span class="entry-title">T</span></div><div class="vevent" itemref="shared"><time class="dtstart" datetime="2020-01-02">d</time></div><div id="shared"><span class="summary">S</span></div>`)
	before := renderTree(t, doc)

	data := microformats.ParseNode(doc, includeTestBase)
	if len(data.Items) != 2 {
		t.Fatalf("got %d top-level items, want 2: %s", len(data.Items), mustJSON(t, data))
	}
	entry, event := data.Items[0], data.Items[1]
	if !cmp.Equal(entry.Type, []string{"h-entry"}) || !cmp.Equal(event.Type, []string{"h-event"}) {
		t.Fatalf("item types = %v, %v, want [h-entry], [h-event]", entry.Type, event.Type)
	}

	// the shared "summary" class maps to p-summary under h-entry, but to
	// p-name under h-event.
	checkProp(t, entry, "name", []string{"T"})
	checkProp(t, entry, "summary", []string{"S"})
	checkProp(t, event, "start", []string{"2020-01-02"})
	checkProp(t, event, "name", []string{"S"})

	checkUnmodified(t, doc, before)
}

// TestIncludeGraphDuplicateTokens verifies that listing the same reference
// twice in one itemref attribute is a legitimate duplicate: the fragment
// contributes once per token, identically on every parse, without growing
// the input tree.
func TestIncludeGraphDuplicateTokens(t *testing.T) {
	doc := parseIncludeDoc(t, `<div class="vcard" itemref="shared shared"><span class="fn">Ann</span></div><div id="shared"><span class="nickname">A</span></div>`)
	before := renderTree(t, doc)

	one := microformats.ParseNode(doc, includeTestBase)
	two := microformats.ParseNode(doc, includeTestBase)

	for _, data := range []*microformats.Data{one, two} {
		item := singleItem(t, data)
		checkProp(t, item, "name", []string{"Ann"})
		checkProp(t, item, "nickname", []string{"A", "A"})
	}
	checkUnmodified(t, doc, before)
}

// TestIncludeGraphDiamond verifies that a fragment reached through two
// different reference paths (a diamond) contributes once per path.
func TestIncludeGraphDiamond(t *testing.T) {
	doc := parseIncludeDoc(t, `<div class="vcard" itemref="x y"><span class="fn">Ann</span></div><div id="x"><span class="nickname">NX</span><object class="include" data="#z"></object></div><div id="y"><span class="nickname">NY</span><object class="include" data="#z"></object></div><div id="z"><span class="title">T</span></div>`)
	before := renderTree(t, doc)

	data := microformats.ParseNode(doc, includeTestBase)
	item := singleItem(t, data)
	checkProp(t, item, "name", []string{"Ann"})
	checkProp(t, item, "nickname", []string{"NX", "NY"})
	checkProp(t, item, "job-title", []string{"T", "T"})

	checkUnmodified(t, doc, before)
}

// TestIncludeGraphCyclesTerminate verifies that reference cycles terminate
// by skipping only the back edge: the rest of the referencing node's normal
// content and its sibling references are still processed.
func TestIncludeGraphCyclesTerminate(t *testing.T) {
	tests := []struct {
		name string
		html string
		want map[string][]string // property name to values on the single h-card
	}{
		{
			name: "direct self reference",
			html: `<div class="vcard" id="v" itemref="v"><span class="fn">Ann</span></div>`,
			want: map[string][]string{"name": {"Ann"}},
		},
		{
			name: "cycle across two ids",
			html: `<div class="vcard" itemref="a"><span class="fn">Ann</span></div><div id="a"><span class="nickname">A</span><object class="include" data="#b"></object></div><div id="b"><span class="org">O</span><object class="include" data="#a"></object></div>`,
			want: map[string][]string{"name": {"Ann"}, "nickname": {"A"}, "org": {"O"}},
		},
		{
			name: "cycle through normal descendants",
			html: `<div id="wrap"><div class="vcard" itemref="inner"><span class="fn">Ann</span></div></div><div id="inner"><span class="nickname">A</span><object class="include" data="#wrap"></object></div>`,
			want: map[string][]string{"name": {"Ann"}, "nickname": {"A"}},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			doc := parseIncludeDoc(t, tt.html)
			before := renderTree(t, doc)

			data := microformats.ParseNode(doc, includeTestBase)
			item := singleItem(t, data)
			for name, want := range tt.want {
				checkProp(t, item, name, want)
			}

			checkUnmodified(t, doc, before)
		})
	}
}

// TestIncludeGraphNoReferencesControl is the control case: a page without
// any references parses normally and is left untouched.
func TestIncludeGraphNoReferencesControl(t *testing.T) {
	doc := parseIncludeDoc(t, `<div class="vcard"><span class="fn">Ann</span><span class="nickname">A</span></div>`)
	before := renderTree(t, doc)

	one := microformats.ParseNode(doc, includeTestBase)
	item := singleItem(t, one)
	checkProp(t, item, "name", []string{"Ann"})
	checkProp(t, item, "nickname", []string{"A"})

	two := microformats.ParseNode(doc, includeTestBase)
	if got, want := mustJSON(t, two), mustJSON(t, one); got != want {
		t.Errorf("repeated parse differs:\nfirst:  %s\nsecond: %s", want, got)
	}
	checkUnmodified(t, doc, before)
}

// TestIncludeGraphV2RootsUnaffected is the control case for v2: plain
// microformats2 roots do not opt into the v1 include rules, so neither
// itemref nor a.include pull referenced content into them.
func TestIncludeGraphV2RootsUnaffected(t *testing.T) {
	doc := parseIncludeDoc(t, `<div class="h-card" itemref="shared"><span class="p-name">Ann</span><a class="include" href="#shared">inc</a></div><div id="shared"><span class="p-nickname">A</span></div>`)
	before := renderTree(t, doc)

	data := microformats.ParseNode(doc, includeTestBase)
	item := singleItem(t, data)
	checkProp(t, item, "name", []string{"Ann"})
	if _, ok := item.Properties["nickname"]; ok {
		t.Errorf("v2 root unexpectedly expanded v1 include references: %s", mustJSON(t, item))
	}

	checkUnmodified(t, doc, before)
}

// TestIncludeGraphRepeatedParseStableAndNonMutating is the reported
// reproduction: parsing the same tree twice must give identical results,
// and parsing must not modify the input tree's attributes or node links.
func TestIncludeGraphRepeatedParseStableAndNonMutating(t *testing.T) {
	doc := parseIncludeDoc(t, `<div class="vcard" itemref="shared"><span class="fn">Ann</span></div><div id="shared"><span class="nickname">A</span></div>`)

	vcard := findByClass(doc, "vcard")
	shared := findByID(doc, "shared")
	if vcard == nil || shared == nil {
		t.Fatal("test document missing expected nodes")
	}
	before := renderTree(t, doc)

	one := microformats.ParseNode(doc, includeTestBase)
	mid := renderTree(t, doc)
	two := microformats.ParseNode(doc, includeTestBase)

	if mid != before {
		t.Errorf("input tree modified by first parse:\nbefore: %s\nafter:  %s", before, mid)
	}
	checkUnmodified(t, doc, before)

	for _, data := range []*microformats.Data{one, two} {
		item := singleItem(t, data)
		checkProp(t, item, "name", []string{"Ann"})
		checkProp(t, item, "nickname", []string{"A"})
	}
	if got, want := mustJSON(t, two), mustJSON(t, one); got != want {
		t.Errorf("repeated parse differs:\nfirst:  %s\nsecond: %s", want, got)
	}

	// no copies were appended to the original nodes, and their links are
	// intact.
	if vcard.FirstChild == nil || vcard.FirstChild != vcard.LastChild {
		t.Errorf("vcard node children changed: FirstChild=%p LastChild=%p", vcard.FirstChild, vcard.LastChild)
	}
	if shared.Parent != vcard.Parent {
		t.Errorf("shared node reparented: Parent=%p, want %p", shared.Parent, vcard.Parent)
	}
	if shared.PrevSibling != vcard || vcard.NextSibling != shared {
		t.Errorf("sibling links changed: shared.PrevSibling=%p vcard.NextSibling=%p", shared.PrevSibling, vcard.NextSibling)
	}
	if shared.FirstChild == nil || shared.FirstChild.NextSibling != nil {
		t.Errorf("shared node children changed: FirstChild=%p", shared.FirstChild)
	}
}

// TestIncludeGraphEContentAndRelativeURLs verifies that e-content HTML
// output and relative URL resolution still work for referenced fragments,
// without the serialization rewriting the shared fragment in the input
// tree.
func TestIncludeGraphEContentAndRelativeURLs(t *testing.T) {
	doc := parseIncludeDoc(t, `<div class="hentry" itemref="ec"><span class="entry-title">T</span></div><div id="ec"><div class="entry-content"><a href="/rel">x</a><img src="pic.png"></div></div><div class="vcard" itemref="card"><span class="fn">F</span></div><div id="card"><a class="url" href="/u">u</a></div>`)
	before := renderTree(t, doc)

	data := microformats.ParseNode(doc, includeTestBase)
	if len(data.Items) != 2 {
		t.Fatalf("got %d top-level items, want 2: %s", len(data.Items), mustJSON(t, data))
	}
	entry, card := data.Items[0], data.Items[1]

	contents := entry.Properties["content"]
	if len(contents) != 1 {
		t.Fatalf("content property = %v, want a single value", contents)
	}
	content, ok := contents[0].(map[string]string)
	if !ok {
		t.Fatalf("content property has unexpected type %T", contents[0])
	}
	for _, want := range []string{`href="https://capture.test/rel"`, `src="https://capture.test/dir/pic.png"`} {
		if !strings.Contains(content["html"], want) {
			t.Errorf("e-content html %q missing %q", content["html"], want)
		}
	}
	checkProp(t, card, "url", []string{"https://capture.test/u"})

	// the shared fragments in the input tree keep their original relative
	// attributes.
	if got := attrVal(findByTag(findByID(doc, "ec"), "a"), "href"); got != "/rel" {
		t.Errorf("original tree href = %q, want %q", got, "/rel")
	}
	if got := attrVal(findByTag(findByID(doc, "ec"), "img"), "src"); got != "pic.png" {
		t.Errorf("original tree img src = %q, want %q", got, "pic.png")
	}
	if got := attrVal(findByTag(findByID(doc, "card"), "a"), "href"); got != "/u" {
		t.Errorf("original tree href = %q, want %q", got, "/u")
	}
	checkUnmodified(t, doc, before)
}

// TestIncludeGraphDuplicateIDsAndUnresolvableRefs verifies that a duplicate
// ID resolves to the first node in document order, and that unresolvable,
// empty, and external references are ignored without fetching anything.
func TestIncludeGraphDuplicateIDsAndUnresolvableRefs(t *testing.T) {
	t.Run("duplicate id uses first in document order", func(t *testing.T) {
		doc := parseIncludeDoc(t, `<div class="vcard" itemref="dup missing"><span class="fn">Ann</span></div><div id="dup"><span class="nickname">First</span></div><div id="dup"><span class="nickname">Second</span></div>`)
		before := renderTree(t, doc)

		data := microformats.ParseNode(doc, includeTestBase)
		item := singleItem(t, data)
		checkProp(t, item, "name", []string{"Ann"})
		checkProp(t, item, "nickname", []string{"First"})

		checkUnmodified(t, doc, before)
	})

	t.Run("unresolvable and external references ignored", func(t *testing.T) {
		doc := parseIncludeDoc(t, `<div class="vcard"><span class="fn">Ann</span><a class="include" href="#missing">m</a><a class="include" href="#">e</a><a class="include" href="https://other.test/p#x">x</a><object class="include" data="https://other.test/o#y"></object></div>`)
		before := renderTree(t, doc)

		data := microformats.ParseNode(doc, includeTestBase)
		item := singleItem(t, data)
		checkProp(t, item, "name", []string{"Ann"})
		if len(data.Rels) != 0 {
			t.Errorf("Rels = %v, want empty", data.Rels)
		}

		checkUnmodified(t, doc, before)
	})
}

// TestIncludeGraphIndependentDocuments verifies that two documents using
// the same ID do not affect each other, even when parsed alternately.
func TestIncludeGraphIndependentDocuments(t *testing.T) {
	docA := parseIncludeDoc(t, `<div class="vcard" itemref="shared"><span class="fn">Ann</span></div><div id="shared"><span class="nickname">A</span></div>`)
	docB := parseIncludeDoc(t, `<div class="vcard" itemref="shared"><span class="fn">Bob</span></div><div id="shared"><span class="nickname">B</span><span class="nickname">B2</span></div>`)

	oneA := microformats.ParseNode(docA, includeTestBase)
	oneB := microformats.ParseNode(docB, includeTestBase)
	twoA := microformats.ParseNode(docA, includeTestBase)

	checkProp(t, singleItem(t, oneA), "nickname", []string{"A"})
	checkProp(t, singleItem(t, oneB), "nickname", []string{"B", "B2"})
	checkProp(t, singleItem(t, twoA), "nickname", []string{"A"})
}
