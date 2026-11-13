// Copyright 2018 Netflix, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package expect

import (
	"bufio"
	"bytes"
	"errors"
	"fmt"
	"io"
	"os"
	"regexp"
	"time"
	"unicode/utf8"
)

// MatchStats reports the matching work a single ExpectPlan call performed.
// The counters are cumulative for the call that returned them.
type MatchStats struct {
	// DecodedBytes is the number of bytes written into the matched text,
	// counted with utf8.RuneLen for each rune read, so invalid UTF-8 bytes
	// decoded as utf8.RuneError each contribute one byte.
	DecodedBytes uint64

	// LiteralWork is the number of literal-matching operations performed:
	// one unit per Aho-Corasick transition-table lookup (including each
	// suffix-fallback lookup) and one unit per output-chain entry examined.
	LiteralWork uint64

	// LiteralSignals is the number of leaf completion notifications, one
	// per leaf position that became satisfied during the call. Repeated
	// occurrences of an already satisfied literal are not re-notified.
	LiteralSignals uint64

	// RegexChecks is the number of times a Regexp/RegexpPattern leaf ran
	// its matcher against the accumulated prefix. Leaves whose enclosing
	// All has already completed are not checked.
	RegexChecks uint64
}

// ExpectPlan is a compiled, reusable set of Expect conditions. A plan is
// immutable after CompileExpect returns and may be shared between Consoles
// and goroutines; every (*Console).ExpectPlan call keeps its own matching
// progress, All completion state, counters and observer snapshots.
type ExpectPlan struct {
	roots []*planNode
	nodes []*planNode

	// parents[id] are the All nodes that contain node id. Completion
	// propagates from a node up these dependency edges.
	parents [][]int

	// ac matches every non-empty String leaf in a single shared pass.
	ac *acAutomaton

	regexLeaves   []int
	errorLeaves   []int
	emptyLiterals []int
	emptyAlls     []int

	readTimeout *time.Duration
}

// planNode is the compiled form of one matcher position. A node is created
// per declaration, so the same condition declared twice gets two nodes and
// each position completes independently.
type planNode struct {
	id   int
	kind nodeKind

	// nAll only, in declaration order.
	children []*planNode

	// cb are the Then wrappers around this position, outermost wrapper
	// first. Only the callbacks on the selected top-level node ever run.
	cb []ConsoleCallback

	// criteria is the value the original matcher's Criteria() would return.
	criteria interface{}

	// nString only.
	literal string

	// nRegexp only.
	re *regexp.Regexp

	// nError only.
	errValue    error
	pathError   os.PathError
	isPathError bool
}

type nodeKind int

const (
	nAll nodeKind = iota
	nString
	nRegexp
	nError
)

// CompileExpect compiles the built-in Expect conditions (String, Regexp,
// RegexpPattern, All, Error, EOF, PTSClosed, WithTimeout and any Then
// wrappers around them) into a reusable ExpectPlan. The condition tree and
// option values (such as the read timeout) are captured at compile time.
//
// Custom Matcher implementations cannot be compiled and make CompileExpect
// return an error; Expect still supports them through its original
// match-per-rune behavior.
func CompileExpect(opts ...ExpectOpt) (*ExpectPlan, error) {
	var options ExpectOpts
	for _, opt := range opts {
		if err := opt(&options); err != nil {
			return nil, err
		}
	}
	return compileExpectOptions(&options)
}

func compileExpectOptions(options *ExpectOpts) (*ExpectPlan, error) {
	plan := &ExpectPlan{
		readTimeout: options.ReadTimeout,
		ac:          newACAutomaton(),
	}

	for _, matcher := range options.Matchers {
		node, err := plan.compileMatcher(matcher)
		if err != nil {
			return nil, err
		}
		plan.roots = append(plan.roots, node)
	}

	plan.parents = make([][]int, len(plan.nodes))
	terminal := map[int][]int{}
	for _, root := range plan.roots {
		plan.index(root, terminal)
	}
	plan.ac.build(terminal)

	return plan, nil
}

func (p *ExpectPlan) addNode(kind nodeKind) *planNode {
	node := &planNode{
		id:   len(p.nodes),
		kind: kind,
	}
	p.nodes = append(p.nodes, node)
	return node
}

// compileMatcher turns one matcher produced by the ExpectOpt builders into a
// plan node. callbackMatcher wrappers are unwrapped into the node's callback
// chain instead of becoming nodes.
func (p *ExpectPlan) compileMatcher(m Matcher) (*planNode, error) {
	switch t := m.(type) {
	case *callbackMatcher:
		node, err := p.compileMatcher(t.matcher)
		if err != nil {
			return nil, err
		}
		node.cb = append(node.cb, t.f)
		return node, nil

	case *allMatcher:
		node := p.addNode(nAll)
		for _, child := range t.options.Matchers {
			compiled, err := p.compileMatcher(child)
			if err != nil {
				return nil, err
			}
			node.children = append(node.children, compiled)
		}
		return node, nil

	case *stringMatcher:
		node := p.addNode(nString)
		node.literal = t.str
		node.criteria = t.str
		return node, nil

	case *regexpMatcher:
		node := p.addNode(nRegexp)
		node.re = t.re
		node.criteria = t.re
		return node, nil

	case *errorMatcher:
		node := p.addNode(nError)
		node.errValue = t.err
		node.criteria = t.err
		return node, nil

	case *pathErrorMatcher:
		node := p.addNode(nError)
		node.isPathError = true
		node.pathError = t.pathError
		node.criteria = t.pathError
		return node, nil

	default:
		return nil, fmt.Errorf("expect: %T is a custom Matcher and cannot be compiled into an ExpectPlan; use Expect instead", m)
	}
}

// index fills the cross-reference tables: parent edges, leaf lists, empty
// positions and the trie terminals for non-empty String leaves.
func (p *ExpectPlan) index(node *planNode, terminal map[int][]int) {
	switch node.kind {
	case nAll:
		if len(node.children) == 0 {
			p.emptyAlls = append(p.emptyAlls, node.id)
		}
		for _, child := range node.children {
			p.parents[child.id] = append(p.parents[child.id], node.id)
			p.index(child, terminal)
		}

	case nString:
		if node.literal == "" {
			// Empty patterns only become true after the first rune, mirroring
			// strings.Contains on a non-empty buffer, so they are kept out of
			// the trie and completed when the runner is primed.
			p.emptyLiterals = append(p.emptyLiterals, node.id)
		} else {
			state := p.ac.add(node.literal)
			terminal[state] = append(terminal[state], node.id)
		}

	case nRegexp:
		p.regexLeaves = append(p.regexLeaves, node.id)

	case nError:
		p.errorLeaves = append(p.errorLeaves, node.id)
	}
}

func (p *ExpectPlan) matchError(id int, err error) bool {
	node := p.nodes[id]
	if node.isPathError {
		pathError, ok := err.(*os.PathError)
		return ok && *pathError == node.pathError
	}
	return err == node.errValue
}

// ExpectPlan runs a compiled ExpectPlan against the Console's tty. It reads
// rune by rune exactly like Expect: each complete rune is written to the
// configured Stdouts and the internal buffer before any condition is
// evaluated, and no rune past the first matching prefix is consumed.
//
// The returned string is the first prefix that satisfies any top-level
// condition, choosing the earliest declaration when several are satisfied on
// the same prefix. On EOF or timeout, error conditions are matched first;
// when none matches, the prefix read so far is returned together with the
// original error.
func (c *Console) ExpectPlan(plan *ExpectPlan) (string, MatchStats, error) {
	if plan == nil {
		return "", MatchStats{}, errors.New("expect: ExpectPlan requires a non-nil *ExpectPlan")
	}
	runner := newPlanRunner(plan)
	buf := runner.buf

	writer := io.MultiWriter(append(c.opts.Stdouts, buf)...)
	runeWriter := bufio.NewWriterSize(writer, utf8.UTFMax)

	readTimeout := c.opts.ReadTimeout
	if plan.readTimeout != nil {
		readTimeout = plan.readTimeout
	}

	selected := -1
	var resultErr error

	defer func() {
		var matchers []Matcher
		if selected >= 0 {
			matchers = []Matcher{runner.snapshotSelected(selected)}
		} else {
			matchers = runner.snapshotActive()
		}
		for _, observer := range c.opts.ExpectObservers {
			observer(matchers, buf.String(), resultErr)
		}
	}()

	for {
		if readTimeout != nil {
			err := c.passthroughPipe.SetReadDeadline(time.Now().Add(*readTimeout))
			if err != nil {
				resultErr = err
				return buf.String(), runner.stats, err
			}
		}

		r, _, err := c.runeReader.ReadRune()
		if err != nil {
			if idx := runner.terminal(err); idx >= 0 {
				selected = idx
				break
			}
			resultErr = err
			return buf.String(), runner.stats, err
		}

		c.Logf("expect read: %q", string(r))
		_, err = runeWriter.WriteRune(r)
		if err != nil {
			resultErr = err
			return buf.String(), runner.stats, err
		}

		// Immediately flush rune to the underlying writers.
		err = runeWriter.Flush()
		if err != nil {
			resultErr = err
			return buf.String(), runner.stats, err
		}

		runner.stats.DecodedBytes += uint64(utf8.RuneLen(r))
		if idx := runner.advance(r); idx >= 0 {
			selected = idx
			break
		}
	}

	// Only the selected top-level matcher's Then chain ever runs. Then
	// wrappers nested inside an All that itself was not wrapped are not
	// reachable through the original dispatch either.
	for _, callback := range plan.roots[selected].cb {
		if err := callback(buf); err != nil {
			resultErr = err
			return buf.String(), runner.stats, err
		}
	}

	return buf.String(), runner.stats, nil
}

// planRunner is the per-call mutable state for one ExpectPlan execution. It
// never touches the shared plan, so one ExpectPlan can be run concurrently by
// many Consoles with independent progress.
type planRunner struct {
	plan *ExpectPlan

	// done[id] mirrors whether node id has been satisfied and removed from
	// its parent's remaining list in the original allMatcher behavior.
	done []bool

	acState int
	buf     *bytes.Buffer

	primed  bool
	pending []int

	stats MatchStats
}

func newPlanRunner(plan *ExpectPlan) *planRunner {
	return &planRunner{
		plan: plan,
		done: make([]bool, len(plan.nodes)),
		buf:  new(bytes.Buffer),
	}
}

// prime evaluates the tautological positions (empty String leaves and empty
// All nodes). It runs when the first rune of the call is processed, never
// before it, so empty conditions keep the old behavior of succeeding on the
// first read prefix rather than before any rune was read.
func (r *planRunner) prime() {
	if r.primed {
		return
	}
	r.primed = true

	for _, id := range r.plan.emptyLiterals {
		r.signal(id)
	}
	for _, id := range r.plan.emptyAlls {
		r.signal(id)
	}
	r.propagate()
}

// signal marks a node satisfied once and queues it for parent propagation.
// Each leaf position produces at most one LiteralSignals count.
func (r *planRunner) signal(id int) {
	if r.done[id] {
		return
	}
	r.done[id] = true
	if r.plan.nodes[id].kind != nAll {
		r.stats.LiteralSignals++
	}
	r.pending = append(r.pending, id)
}

// propagate pushes completion up the All dependency edges. An All is only
// revisited when one of its children signaled, so All nodes are not walked on
// every rune.
func (r *planRunner) propagate() {
	for len(r.pending) > 0 {
		id := r.pending[0]
		r.pending = r.pending[1:]

		for _, parent := range r.plan.parents[id] {
			if r.done[parent] {
				continue
			}
			allDone := true
			for _, child := range r.plan.nodes[parent].children {
				if !r.done[child.id] {
					allDone = false
					break
				}
			}
			if allDone {
				r.signal(parent)
			}
		}
	}
}

// advance feeds one rune, already written to the buffer and Stdouts, through
// the literal automaton and the still-necessary regexp leaves, and returns
// the selected top-level index or -1.
func (r *planRunner) advance(rn rune) int {
	r.prime()
	r.stepLiteral(rn)
	r.propagate()

	for _, id := range r.plan.regexLeaves {
		if r.done[id] || !r.necessary(id) {
			continue
		}
		r.stats.RegexChecks++
		if r.plan.nodes[id].re.Match(r.buf.Bytes()) {
			r.signal(id)
		}
	}
	r.propagate()

	return r.pick()
}

// stepLiteral performs one shared Aho-Corasick transition and notifies the
// leaves on the resulting output chain, suppressing already satisfied
// positions so shared suffixes do not re-notify them.
func (r *planRunner) stepLiteral(rn rune) {
	automaton := r.plan.ac
	state := r.acState

	for {
		r.stats.LiteralWork++
		next, ok := automaton.states[state].next[rn]
		if ok {
			state = next
			break
		}
		if state == 0 {
			break
		}
		state = automaton.states[state].link
	}
	r.acState = state

	for _, leaf := range automaton.states[state].out {
		r.stats.LiteralWork++
		r.signal(leaf)
	}
}

// necessary reports whether a leaf can still change the result: false once an
// All anywhere on its ancestor chain has completed.
func (r *planRunner) necessary(id int) bool {
	stack := append([]int(nil), r.plan.parents[id]...)
	for len(stack) > 0 {
		parent := stack[len(stack)-1]
		stack = stack[:len(stack)-1]
		if r.done[parent] {
			return false
		}
		stack = append(stack, r.plan.parents[parent]...)
	}
	return true
}

// terminal applies the error leaves to a read error and propagates their
// completion, returning the selected top-level index or -1. Content leaves
// are never evaluated against errors, matching the original Expect. Empty
// All nodes match every value, including errors, so they are signaled here
// exactly as the original allMatcher would have matched the read error.
func (r *planRunner) terminal(err error) int {
	for _, id := range r.plan.emptyAlls {
		r.signal(id)
	}
	r.propagate()

	for _, id := range r.plan.errorLeaves {
		if r.done[id] {
			continue
		}
		if r.plan.matchError(id, err) {
			r.signal(id)
		}
	}
	r.propagate()
	return r.pick()
}

// pick returns the first satisfied top-level condition in declaration order.
func (r *planRunner) pick() int {
	for i, root := range r.plan.roots {
		if r.done[root.id] {
			return i
		}
	}
	return -1
}

// snapshotMatcher exposes a frozen copy of one node's end-of-call state to
// ExpectObservers. The done slice is copied, so later ExpectPlan calls can
// never mutate a snapshot handed out earlier.
type snapshotMatcher struct {
	node *planNode
	done []bool
}

func (m *snapshotMatcher) Match(v interface{}) bool {
	return false
}

func (m *snapshotMatcher) Criteria() interface{} {
	return m.criteria(m.node)
}

func (m *snapshotMatcher) criteria(node *planNode) interface{} {
	if node.kind != nAll {
		return node.criteria
	}

	var criterias []interface{}
	for _, child := range node.children {
		if !m.done[child.id] {
			criterias = append(criterias, m.criteria(child))
		}
	}
	return criterias
}

func (r *planRunner) snapshot() []bool {
	done := make([]bool, len(r.done))
	copy(done, r.done)
	return done
}

func (r *planRunner) snapshotSelected(index int) Matcher {
	return &snapshotMatcher{
		node: r.plan.roots[index],
		done: r.snapshot(),
	}
}

func (r *planRunner) snapshotActive() []Matcher {
	done := r.snapshot()
	var matchers []Matcher
	for _, root := range r.plan.roots {
		if done[root.id] {
			continue
		}
		matchers = append(matchers, &snapshotMatcher{
			node: root,
			done: done,
		})
	}
	return matchers
}

// acAutomaton is a shared Aho-Corasick matcher for every non-empty String
// leaf of one ExpectPlan. All literals are matched in a single pass over the
// input runes: each rune costs one trie transition (amortized O(1) with
// suffix links) plus examination of the state's precomputed output chain.
// Work therefore grows with the decoded text length and total literal length
// (trie size), not with text length times the number of String conditions.
type acAutomaton struct {
	states []*acState
}

type acState struct {
	next map[rune]int
	link int
	// out is the closed output chain: every pattern id that ends at this
	// state, including patterns reached through suffix links.
	out []int
}

func newACAutomaton() *acAutomaton {
	return &acAutomaton{
		states: []*acState{{
			next: map[rune]int{},
		}},
	}
}

// add inserts a pattern and returns the trie state at which it ends.
func (a *acAutomaton) add(pattern string) int {
	cur := 0
	for _, r := range pattern {
		next, ok := a.states[cur].next[r]
		if !ok {
			next = len(a.states)
			a.states[cur].next[r] = next
			a.states = append(a.states, &acState{
				next: map[rune]int{},
			})
		}
		cur = next
	}
	return cur
}

// build computes suffix links and closed output chains in breadth-first
// order. terminal maps trie states to the pattern (leaf) ids ending there.
func (a *acAutomaton) build(terminal map[int][]int) {
	queue := []int{}
	for _, next := range a.states[0].next {
		queue = append(queue, next)
	}

	for len(queue) > 0 {
		u := queue[0]
		queue = queue[1:]
		su := a.states[u]

		su.out = append(su.out, terminal[u]...)
		su.out = append(su.out, a.states[su.link].out...)

		for r, v := range su.next {
			// Walk suffix links until an ancestor has an edge labeled r;
			// the check at f == 0 also handles root edges, in one loop.
			f := su.link
			for {
				if next, ok := a.states[f].next[r]; ok {
					f = next
					break
				}
				if f == 0 {
					break
				}
				f = a.states[f].link
			}
			a.states[v].link = f
			queue = append(queue, v)
		}
	}
}
