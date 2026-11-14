package cascadia

import (
	"sync/atomic"

	"golang.org/x/net/html"
)

// This file implements a query-scoped execution plan for positional
// pseudo-classes. QueryAll and the package-level Filter build one planContext
// per call. The context lazily indexes each distinct parent's element children
// once, so a positional selector evaluated against many candidates shares one
// sibling walk instead of walking PrevSibling/NextSibling for every candidate.
//
// The plan holds no state on the DOM, on parsed selectors, or in package
// globals. A fresh planContext is created for every QueryAll/Filter call, so
// concurrent queries of the same parsed selector are independent and tree
// mutations between queries are observed normally.

// planMatcher is implemented by the selector types produced by the parser.
// A Matcher that does not implement it is evaluated with its own Match method.
type planMatcher interface {
	matchWith(*planContext, *html.Node) bool
}

func planMatch(pc *planContext, m Matcher, n *html.Node) bool {
	if pm, ok := m.(planMatcher); ok {
		return pm.matchWith(pc, n)
	}
	return m.Match(n)
}

// Package-internal instrumentation. The counters observe the real work done by
// the shared sibling index and are used by the tests to assert that work grows
// linearly with the number of children. They never influence query results.
var planStats struct {
	siblingEdges  atomic.Int64
	ofCandidates  atomic.Int64
	parentIndexes atomic.Int64
	ofIndexBuilds atomic.Int64
}

type typeName struct {
	namespace string
	data      string
}

// ofGroupKey identifies an :nth-child(... of S) selector group by the address
// of its first element. Parsed selector groups own their backing array, so the
// address is stable for the lifetime of the parsed selector and needs no
// mutation of the selector object.
type ofGroupKey struct {
	first  *Sel
	length int
}

func ofKeyFor(of SelectorGroup) ofGroupKey {
	if len(of) == 0 {
		return ofGroupKey{}
	}
	return ofGroupKey{first: &of[0], length: len(of)}
}

type ofGroupRanks struct {
	// rank is parallel to parentPlan.children. A zero entry means the child
	// does not match S; otherwise it is the 1-based rank among matching
	// children, counted from the left.
	rank  []int
	total int
}

type parentPlan struct {
	children []*html.Node
	// pos maps an element child to its zero-based position in children.
	pos map[*html.Node]int
	// typeOrdinals[i] is the 1-based rank of children[i] among siblings with
	// the same namespace and tag, counted from the left.
	typeOrdinals []int
	// typeTotals records how many children have each namespace/tag.
	typeTotals map[typeName]int
	// ofRanks caches the filtered sibling ranking for each :nth-child(... of S)
	// group used with this parent.
	ofRanks map[ofGroupKey]*ofGroupRanks
}

type planContext struct {
	parents map[*html.Node]*parentPlan
}

func newPlanContext() *planContext {
	return &planContext{parents: make(map[*html.Node]*parentPlan)}
}

// parentFor returns the sibling index for parent, building it on first use.
// The index covers all of parent's element children, including children
// outside whatever query root the candidates came from.
func (pc *planContext) parentFor(parent *html.Node) *parentPlan {
	if p, ok := pc.parents[parent]; ok {
		return p
	}
	p := &parentPlan{
		pos:        make(map[*html.Node]int),
		typeTotals: make(map[typeName]int),
		ofRanks:    make(map[ofGroupKey]*ofGroupRanks),
	}
	// Publish the index before building any derived ranking: evaluating S may
	// itself contain positional selectors that need this same parent's index.
	pc.parents[parent] = p

	for c := parent.FirstChild; c != nil; c = c.NextSibling {
		planStats.siblingEdges.Add(1)
		if c.Type != html.ElementNode {
			continue
		}
		p.pos[c] = len(p.children)
		p.children = append(p.children, c)
		tn := typeName{namespace: c.Namespace, data: c.Data}
		p.typeTotals[tn]++
		p.typeOrdinals = append(p.typeOrdinals, p.typeTotals[tn])
	}
	planStats.parentIndexes.Add(1)
	return p
}

// ranksFor returns (building once per parent and S) the ranks of this parent's
// element children filtered by the :nth-child(an+b of S) selector group.
func (p *parentPlan) ranksFor(pc *planContext, of SelectorGroup) *ofGroupRanks {
	key := ofKeyFor(of)
	if r, ok := p.ofRanks[key]; ok {
		return r
	}
	r := &ofGroupRanks{rank: make([]int, len(p.children))}
	p.ofRanks[key] = r
	for i, c := range p.children {
		planStats.ofCandidates.Add(1)
		if planMatch(pc, of, c) {
			r.total++
			r.rank[i] = r.total
		}
	}
	planStats.ofIndexBuilds.Add(1)
	return r
}

func nthPositionMatches(a, b, rank int) bool {
	i := rank - b
	if a == 0 {
		return i == 0
	}
	return i%a == 0 && i/a >= 0
}

func (s nthPseudoClassSelector) matchWith(pc *planContext, n *html.Node) bool {
	if n.Type != html.ElementNode {
		return false
	}
	parent := n.Parent
	if parent == nil {
		return false
	}
	p := pc.parentFor(parent)
	pos, ok := p.pos[n]
	if !ok {
		// The node is not one of its parent's children (a malformed tree). Let
		// the ordinary implementation decide, as it did before.
		return s.Match(n)
	}

	var rank int
	switch {
	case s.of != nil:
		r := p.ranksFor(pc, s.of)
		if r.rank[pos] == 0 {
			return false
		}
		rank = r.rank[pos]
		if s.last {
			rank = r.total - rank + 1
		}
	case s.ofType:
		tn := typeName{namespace: n.Namespace, data: n.Data}
		rank = p.typeOrdinals[pos]
		if s.last {
			rank = p.typeTotals[tn] - rank + 1
		}
	default:
		rank = pos + 1
		if s.last {
			rank = len(p.children) - pos
		}
	}

	return nthPositionMatches(s.a, s.b, rank)
}

// The following plan-aware match implementations mirror the semantics of the
// ordinary Match methods but route nested selectors through the shared plan.
// In particular, positional conditions inside :not/:is/:where/:has and inside
// an "of S" group use the same per-query sibling index.

func (t compoundSelector) matchWith(pc *planContext, n *html.Node) bool {
	if len(t.selectors) == 0 {
		return n.Type == html.ElementNode
	}
	for _, sel := range t.selectors {
		if !planMatch(pc, sel, n) {
			return false
		}
	}
	return true
}

func (t combinedSelector) matchWith(pc *planContext, n *html.Node) bool {
	if t.first == nil {
		return false
	}
	switch t.combinator {
	case 0:
		return planMatch(pc, t.first, n)
	case ' ':
		return descendantMatchPlan(pc, t.first, t.second, n)
	case '>':
		return childMatchPlan(pc, t.first, t.second, n)
	case '+':
		return siblingMatchPlan(pc, t.first, t.second, true, n)
	case '~':
		return siblingMatchPlan(pc, t.first, t.second, false, n)
	default:
		panic("unknown combinator")
	}
}

func descendantMatchPlan(pc *planContext, a, d Matcher, n *html.Node) bool {
	if !planMatch(pc, d, n) {
		return false
	}
	for p := n.Parent; p != nil; p = p.Parent {
		if planMatch(pc, a, p) {
			return true
		}
	}
	return false
}

func childMatchPlan(pc *planContext, a, d Matcher, n *html.Node) bool {
	return planMatch(pc, d, n) && n.Parent != nil && planMatch(pc, a, n.Parent)
}

func siblingMatchPlan(pc *planContext, s1, s2 Matcher, adjacent bool, n *html.Node) bool {
	if !planMatch(pc, s2, n) {
		return false
	}
	if adjacent {
		for n = n.PrevSibling; n != nil; n = n.PrevSibling {
			if n.Type == html.TextNode || n.Type == html.CommentNode {
				continue
			}
			return planMatch(pc, s1, n)
		}
		return false
	}
	for c := n.PrevSibling; c != nil; c = c.PrevSibling {
		if planMatch(pc, s1, c) {
			return true
		}
	}
	return false
}

func (s SelectorGroup) matchWith(pc *planContext, n *html.Node) bool {
	for _, sel := range s {
		if planMatch(pc, sel, n) {
			return true
		}
	}
	return false
}

func (s notPseudoClassSelector) matchWith(pc *planContext, n *html.Node) bool {
	if n.Type != html.ElementNode {
		return false
	}
	return !planMatch(pc, s.match, n)
}

func (s isPseudoClassSelector) matchWith(pc *planContext, n *html.Node) bool {
	if n.Type != html.ElementNode {
		return false
	}
	return planMatch(pc, s.match, n)
}

func (s relativePseudoClassSelector) matchWith(pc *planContext, n *html.Node) bool {
	if n.Type != html.ElementNode {
		return false
	}
	switch s.name {
	case "has", "haschild":
		for _, r := range s.rels {
			comb := r.combinator
			if comb == 0 {
				if s.name == "haschild" {
					comb = '>'
				} else {
					comb = ' '
				}
			}
			if relativeMatchPlan(pc, n, comb, r.sel) {
				return true
			}
		}
		return false
	default:
		panic("unsupported relative pseudo class selector")
	}
}

func relativeMatchPlan(pc *planContext, n *html.Node, combinator byte, sel Sel) bool {
	sg := SelectorGroup{sel}
	switch combinator {
	case ' ':
		return hasDescendantMatchPlan(pc, n, sg)
	case '>':
		return hasChildMatchPlan(pc, n, sg)
	case '+':
		for c := n.NextSibling; c != nil; c = c.NextSibling {
			if c.Type == html.ElementNode {
				return planMatch(pc, sg, c)
			}
		}
		return false
	case '~':
		for c := n.NextSibling; c != nil; c = c.NextSibling {
			if c.Type == html.ElementNode && planMatch(pc, sg, c) {
				return true
			}
		}
		return false
	default:
		return false
	}
}

func hasChildMatchPlan(pc *planContext, n *html.Node, a SelectorGroup) bool {
	for c := n.FirstChild; c != nil; c = c.NextSibling {
		if planMatch(pc, a, c) {
			return true
		}
	}
	return false
}

func hasDescendantMatchPlan(pc *planContext, n *html.Node, a SelectorGroup) bool {
	for c := n.FirstChild; c != nil; c = c.NextSibling {
		if planMatch(pc, a, c) || (c.Type == html.ElementNode && hasDescendantMatchPlan(pc, c, a)) {
			return true
		}
	}
	return false
}

// Statistics accessors used by the positional plan tests.
func resetPositionPlanStats() {
	planStats.siblingEdges.Store(0)
	planStats.ofCandidates.Store(0)
	planStats.parentIndexes.Store(0)
	planStats.ofIndexBuilds.Store(0)
}

func positionPlanSiblingEdges() int64  { return planStats.siblingEdges.Load() }
func positionPlanOfCandidates() int64  { return planStats.ofCandidates.Load() }
func positionPlanParentIndexes() int64 { return planStats.parentIndexes.Load() }
func positionPlanOfIndexBuilds() int64 { return planStats.ofIndexBuilds.Load() }
