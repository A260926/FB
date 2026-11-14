package cascadia

import (
	"fmt"
	"strings"
	"sync"
	"testing"

	"golang.org/x/net/html"
)

// positionalPlanHTML contains mixed sibling types, comments and text nodes,
// negative/odd coefficients, and two separate trees' worth of lists. It is
// deliberately irregular so that positional pseudo-classes cannot be right by
// accident.
const positionalPlanHTML = `<html><head></head><body>
<div id="wrap">
<ul id="list">
<li id="a1" class="visible">1</li><!-- between --><li id="a2">2</li><li id="a3" class="visible x">3</li><span id="sp1">s</span><li id="a4" class="visible">4</li><p id="p1" class="visible">p</p><li id="a5">5</li><li id="a6" class="visible">6</li>tail
</ul>
<ul id="list2">
<li id="b1" class="visible">1</li><li id="b2" class="visible">2</li>
<div id="mid"><p id="c1">x</p><!-- c --><p id="c2" class="visible">y</p><em id="c3">z</em><p id="c4" class="visible">w</p></div>
</ul>
<section id="siblings"><div id="root"><p id="r1" class="visible">1</p><!-- x --><p id="r2">2</p><span id="r3">3</span><p id="r4" class="visible">4</p></div></section>
</div>
</body></html>`

// positionalPlanSelectors are checked against a slow per-node reference.
var positionalPlanSelectors = []string{
	"li:nth-child(2n+1)",
	"li:nth-last-child(-n+2)",
	":nth-of-type(2)",
	":nth-last-of-type(2n)",
	"li:nth-child(-1n+3)",
	":nth-child(2 of .visible)",
	":nth-last-child(2n+1 of .visible)",
	":nth-child(-2n+5 of .visible)",
	"li:nth-child(2n of .visible):not(:nth-last-child(-n+2 of .visible))",
	":nth-child(3n-1 of li:nth-child(odd))",
	":nth-last-child(1 of .visible:nth-of-type(2))",
	":not(:nth-child(2n))",
	":is(:nth-child(2), :nth-last-child(1))",
	":where(:nth-of-type(1))",
	":has(> :nth-child(2n))",
	"div > :nth-child(2n+1)",
	":nth-child(2 of .visible, span)",
	"ul :nth-child(2)",
	"#root > :nth-last-child(2)",
	":nth-child(1 of :not(:nth-last-of-type(1)))",
	":nth-child(2 of :nth-child(odd):nth-last-child(n+2))",
	"li, :nth-child(3 of .visible)",
}

func parsePlanTestTree(t *testing.T, src string) *html.Node {
	t.Helper()
	doc, err := html.Parse(strings.NewReader(src))
	if err != nil {
		t.Fatalf("parse HTML: %v", err)
	}
	return doc
}

// descendants returns root's descendants in document order, excluding root.
func descendants(root *html.Node) (out []*html.Node) {
	var walk func(*html.Node)
	walk = func(n *html.Node) {
		for c := n.FirstChild; c != nil; c = c.NextSibling {
			out = append(out, c)
			walk(c)
		}
	}
	walk(root)
	return out
}

// slowQueryAll is the reference implementation: every candidate is matched on
// its own with the ordinary per-node Match, which performs the old
// candidate-by-candidate sibling scans.
func slowQueryAll(root *html.Node, m Matcher) (out []*html.Node) {
	for _, c := range descendants(root) {
		if m.Match(c) {
			out = append(out, c)
		}
	}
	return out
}

func slowFilter(nodes []*html.Node, m Matcher) (out []*html.Node) {
	for _, n := range nodes {
		if m.Match(n) {
			out = append(out, n)
		}
	}
	return out
}

func nodeID(n *html.Node) string {
	for _, a := range n.Attr {
		if a.Key == "id" {
			return a.Val
		}
	}
	return ""
}

func nodeIDs(nodes []*html.Node) []string {
	ids := make([]string, len(nodes))
	for i, n := range nodes {
		ids[i] = nodeID(n)
	}
	return ids
}

func sameNodes(a, b []*html.Node) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func findNode(root *html.Node, id string) *html.Node {
	for _, n := range descendants(root) {
		if nodeID(n) == id {
			return n
		}
	}
	return nil
}

func mustParseGroup(t *testing.T, sel string) SelectorGroup {
	t.Helper()
	s, err := ParseGroup(sel)
	if err != nil {
		t.Fatalf("ParseGroup(%q): %v", sel, err)
	}
	return s
}

func TestPositionPlanResults(t *testing.T) {
	doc := parsePlanTestTree(t, positionalPlanHTML)
	root := findNode(doc, "root")
	if root == nil {
		t.Fatal("missing #root")
	}

	for _, sel := range positionalPlanSelectors {
		s := mustParseGroup(t, sel)
		for _, queryRoot := range []*html.Node{doc, root} {
			got := QueryAll(queryRoot, s)
			want := slowQueryAll(queryRoot, s)
			if !sameNodes(got, want) {
				t.Errorf("%s (root %q): QueryAll ids %v, want %v",
					sel, nodeID(queryRoot), nodeIDs(got), nodeIDs(want))
			}
			// Selector.Match converted to the old-style function must agree.
			old := Selector(s.Match).Filter(slowQueryAll(queryRoot, s))
			if !sameNodes(old, got) {
				t.Errorf("%s (root %q): Selector.Filter ids %v, QueryAll %v",
					sel, nodeID(queryRoot), nodeIDs(old), nodeIDs(got))
			}
		}

		// Filter must preserve input order and duplicates, and count against
		// each node's real parent even when only a few siblings are supplied.
		all := descendants(doc)
		var input []*html.Node
		for _, n := range all {
			if n.Type == html.ElementNode {
				input = append(input, n)
			}
		}
		input = append(input, input[len(input)/3], input[len(input)/2], input[len(input)/3])
		got := Filter(input, s)
		want := slowFilter(input, s)
		if !sameNodes(got, want) {
			t.Errorf("%s: Filter ids %v, want %v", sel, nodeIDs(got), nodeIDs(want))
		}
	}
}

func TestPositionPlanFilterAcrossTrees(t *testing.T) {
	docA := parsePlanTestTree(t, positionalPlanHTML)
	docB := parsePlanTestTree(t, `<ul id="other"><li id="z1" class="visible">1</li><li id="z2">2</li><li id="z3" class="visible">3</li></ul>`)
	docC := parsePlanTestTree(t, `<div id="solo"><i id="q1" class="visible"></i><b id="q2"></b></div>`)

	var input []*html.Node
	for _, doc := range []*html.Node{docA, docB, docC} {
		for _, n := range descendants(doc) {
			if n.Type == html.ElementNode {
				input = append(input, n)
			}
		}
	}
	// Repeat a few nodes from different trees to exercise duplicates.
	input = append(input, findNode(docB, "z3"), findNode(docC, "q1"), findNode(docA, "a1"), findNode(docB, "z3"))

	for _, sel := range []string{
		":nth-child(2 of .visible)",
		":nth-last-child(1 of .visible)",
		":nth-child(2n+1)",
		":nth-of-type(2)",
		":not(:nth-last-child(1))",
	} {
		s := mustParseGroup(t, sel)
		got := Filter(input, s)
		want := slowFilter(input, s)
		if !sameNodes(got, want) {
			t.Errorf("%s: Filter across trees ids %v, want %v", sel, nodeIDs(got), nodeIDs(want))
		}
	}
}

// orderingMatcher records every Match call so the test can verify that custom
// Matcher implementations are never cached and keep their call order.
type orderingMatcher struct {
	inner Matcher
	calls []*html.Node
}

func (m *orderingMatcher) Match(n *html.Node) bool {
	m.calls = append(m.calls, n)
	return m.inner.Match(n)
}

func TestPositionPlanCustomMatcherOrder(t *testing.T) {
	doc := parsePlanTestTree(t, positionalPlanHTML)
	s := mustParseGroup(t, "li:nth-child(2n of .visible)")
	m := &orderingMatcher{inner: s}

	got := QueryAll(doc, m)
	want := slowQueryAll(doc, s)
	if !sameNodes(got, want) {
		t.Errorf("QueryAll through custom matcher ids %v, want %v", nodeIDs(got), nodeIDs(want))
	}
	wantCalls := descendants(doc)
	if len(m.calls) != len(wantCalls) {
		t.Fatalf("custom matcher called %d times, want %d", len(m.calls), len(wantCalls))
	}
	for i := range wantCalls {
		if m.calls[i] != wantCalls[i] {
			t.Fatalf("custom matcher call %d got %q, want %q in document order",
				i, nodeID(m.calls[i]), nodeID(wantCalls[i]))
		}
	}

	// Filter keeps the user matcher's call order too, and duplicates.
	input := append(descendants(doc), findNode(doc, "a2"), findNode(doc, "a2"))
	m.calls = nil
	got = Filter(input, m)
	want = slowFilter(input, s)
	if !sameNodes(got, want) {
		t.Errorf("Filter through custom matcher ids %v, want %v", nodeIDs(got), nodeIDs(want))
	}
	if len(m.calls) != len(input) {
		t.Fatalf("custom matcher called %d times in Filter, want %d", len(m.calls), len(input))
	}
	for i := range input {
		if m.calls[i] != input[i] {
			t.Fatalf("Filter custom matcher call %d got %q, want %q", i, nodeID(m.calls[i]), nodeID(input[i]))
		}
	}
}

// planWorkTree builds a wide list with comments interspersed so sibling-edge
// accounting sees the true walks.
func planWorkTree(t *testing.T, n int) *html.Node {
	t.Helper()
	var b strings.Builder
	b.WriteString("<html><body><ul id=\"wide\">")
	for i := 0; i < n; i++ {
		if i%2 == 0 {
			b.WriteString(`<li class="visible">`)
		} else {
			b.WriteString(`<li>`)
		}
		b.WriteString("x</li><!-- c -->")
	}
	b.WriteString("</ul></body></html>")
	return parsePlanTestTree(t, b.String())
}

func TestPositionPlanWork(t *testing.T) {
	sizes := []int{512, 1024, 2048}
	selectors := []string{
		"li:nth-child(2n+1)",
		"li:nth-last-child(-n+4)",
		"li:nth-of-type(2n)",
		"li:nth-last-of-type(3n+1)",
		"li:nth-child(2n of .visible)",
		"li:nth-child(2n of .visible):not(:nth-last-child(-n+2 of .visible))",
	}

	type work struct {
		edges, ofCandidates int64
		matches             int
	}
	results := make(map[string][]work)
	for _, sel := range selectors {
		s := mustParseGroup(t, sel)
		for _, n := range sizes {
			doc := planWorkTree(t, n)
			resetPositionPlanStats()
			got := QueryAll(doc, s)
			edges := positionPlanSiblingEdges()
			ofCandidates := positionPlanOfCandidates()
			if len(got) == 0 {
				t.Fatalf("%s n=%d: no matches", sel, n)
			}
			// The measured work must actually have been observed.
			if edges <= 0 {
				t.Fatalf("%s n=%d: no sibling edges recorded", sel, n)
			}
			results[sel] = append(results[sel], work{edges: edges, ofCandidates: ofCandidates, matches: len(got)})
		}
	}

	for _, sel := range selectors {
		w := results[sel]
		for i, n := range sizes {
			// The plan indexes each parent at most once: at most two passes
			// over the child list (index build plus an S ranking per distinct
			// "of" group) even for the nested positional selector.
			maxEdges := int64(4*n + 4)
			maxOf := int64(4*n + 4)
			if w[i].edges > maxEdges {
				t.Errorf("%s n=%d: %d sibling edge visits, want <= %d (linear)", sel, n, w[i].edges, maxEdges)
			}
			if w[i].ofCandidates > maxOf {
				t.Errorf("%s n=%d: %d filtered candidate evaluations, want <= %d (linear)", sel, n, w[i].ofCandidates, maxOf)
			}
			// Growth must track the child count, not its square. Allow a
			// small constant, but reject anything superlinear.
			if i > 0 {
				prev, cur := w[i-1], w[i]
				prevN := int64(sizes[i-1])
				curN := int64(sizes[i])
				if cur.edges > prev.edges*curN/prevN*3/2+16 {
					t.Errorf("%s: sibling edges grew from %d (n=%d) to %d (n=%d), worse than linear",
						sel, prev.edges, prevN, cur.edges, curN)
				}
				if cur.ofCandidates > prev.ofCandidates*curN/prevN*3/2+16 {
					t.Errorf("%s: filtered candidates grew from %d (n=%d) to %d (n=%d), worse than linear",
						sel, prev.ofCandidates, prevN, cur.ofCandidates, curN)
				}
			}
		}

		// The .visible selectors must actually evaluate S once per child, and
		// the nested case once per child per distinct S group, not once per
		// candidate sibling walk.
		if strings.Contains(sel, "of .visible") {
			for i, n := range sizes {
				if w[i].ofCandidates < int64(n) {
					t.Errorf("%s n=%d: only %d filtered candidates recorded, want at least %d",
						sel, n, w[i].ofCandidates, n)
				}
			}
		}
		// Results must remain identical to the slow reference at every size.
		for i, n := range sizes {
			doc := planWorkTree(t, n)
			want := len(slowQueryAll(doc, mustParseGroup(t, sel)))
			if w[i].matches != want {
				t.Errorf("%s n=%d: %d matches, want %d", sel, n, w[i].matches, want)
			}
		}
		t.Logf("%s: %v", sel, results[sel])
	}

	// A per-candidate PrevSibling scan would visit on the order of n*n/2
	// edges for the wide list; the shared index stays linear by comparison.
	doc := planWorkTree(t, sizes[len(sizes)-1])
	resetPositionPlanStats()
	QueryAll(doc, mustParseGroup(t, "li:nth-child(2n of .visible)"))
	if edges := positionPlanSiblingEdges(); edges >= int64(sizes[len(sizes)-1])*int64(sizes[len(sizes)-1])/8 {
		t.Errorf("sibling edge visits %d are close to quadratic for n=%d", edges, sizes[len(sizes)-1])
	}
}

func TestPositionPlanIsolation(t *testing.T) {
	t.Run("tree changes", func(t *testing.T) {
		doc := parsePlanTestTree(t, positionalPlanHTML)
		s := mustParseGroup(t, "li:nth-child(2n of .visible):not(:nth-last-child(-n+2 of .visible))")
		check := func(what string) {
			t.Helper()
			got := QueryAll(doc, s)
			want := slowQueryAll(doc, s)
			if !sameNodes(got, want) {
				t.Fatalf("%s: ids %v, want %v", what, nodeIDs(got), nodeIDs(want))
			}
		}
		check("initial")

		list := findNode(doc, "list")
		a1 := findNode(doc, "a1")
		a3 := findNode(doc, "a3")

		// Remove a visible sibling.
		list.RemoveChild(a1)
		check("after remove")

		// Insert it at the end.
		list.AppendChild(a1)
		check("after append")

		// Move a visible sibling to the front.
		list.RemoveChild(a3)
		list.InsertBefore(a3, list.FirstChild)
		check("after move")

		// Change class membership.
		for i := range a1.Attr {
			if a1.Attr[i].Key == "class" {
				a1.Attr[i].Val = "plain"
			}
		}
		check("after class change")

		// Insert a new visible element in the middle.
		extra := &html.Node{Type: html.ElementNode, Data: "li", Attr: []html.Attribute{{Key: "class", Val: "visible"}, {Key: "id", Val: "a7"}}}
		list.InsertBefore(extra, a3.NextSibling)
		check("after insert")
	})

	t.Run("concurrent queries", func(t *testing.T) {
		docA := parsePlanTestTree(t, positionalPlanHTML)
		docB := parsePlanTestTree(t, `<ul id="u"><li class="visible">1</li><li>2</li><li class="visible">3</li><li class="visible">4</li></ul>`)
		s := mustParseGroup(t, "li:nth-child(2n of .visible), :nth-last-child(1 of .visible)")

		wantA := slowQueryAll(docA, s)
		wantB := slowQueryAll(docB, s)

		var wg sync.WaitGroup
		errs := make(chan string, 64)
		for g := 0; g < 8; g++ {
			wg.Add(2)
			go func() {
				defer wg.Done()
				for i := 0; i < 20; i++ {
					if got := QueryAll(docA, s); !sameNodes(got, wantA) {
						errs <- fmt.Sprintf("tree A: ids %v, want %v", nodeIDs(got), nodeIDs(wantA))
						return
					}
				}
			}()
			go func() {
				defer wg.Done()
				for i := 0; i < 20; i++ {
					if got := QueryAll(docB, s); !sameNodes(got, wantB) {
						errs <- fmt.Sprintf("tree B: ids %v, want %v", nodeIDs(got), nodeIDs(wantB))
						return
					}
				}
			}()
		}
		wg.Wait()
		close(errs)
		for msg := range errs {
			t.Error(msg)
		}
	})
}
