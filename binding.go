// Copyright 2013 Federico Sogaro. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package webdriver

import (
	"errors"
	"fmt"
	"math"
	"math/big"
	"math/bits"
	"strings"
)

// Locator describes how an element must be looked up. Its fields are the
// same arguments accepted by FindElement and FindElements.
type Locator struct {
	Using FindElementStrategy
	Value string
}

// FieldSlot describes one form field that BindFields must bind.
//
// Label locates the field's label and Candidates locates the input
// controls placed to its right. The binding process never fills, clicks,
// scrolls or executes JavaScript: it only performs element lookups and
// reads visibility, enabled state and geometry.
type FieldSlot struct {
	// Name identifies the slot in errors. It must be non empty and unique
	// inside one BindFields call.
	Name string
	// Label must resolve to exactly one visible element with a positive
	// area rectangle.
	Label Locator
	// Candidates may resolve to any number of elements. Only controls that
	// are displayed, enabled and have a positive area rectangle take part
	// in the assignment.
	Candidates Locator
}

// BindError reports a failed BindFields call. Field is the Name of the
// slot being processed (empty for call-wide failures), Stage names the
// phase that failed and Err is the original cause.
type BindError struct {
	Field string
	Stage string
	Err   error
}

func (e *BindError) Error() string {
	if e.Field != "" {
		return fmt.Sprintf("webdriver: BindFields field %q stage %q: %v", e.Field, e.Stage, e.Err)
	}
	return fmt.Sprintf("webdriver: BindFields stage %q: %v", e.Stage, e.Err)
}

// Unwrap exposes the original failure so errors.Is and errors.As keep
// working with the underlying protocol errors.
func (e *BindError) Unwrap() error { return e.Err }

func bindFail(field, stage string, err error) *BindError {
	return &BindError{Field: field, Stage: stage, Err: err}
}

// scopeFinder abstracts the element lookup entry point so that the same
// binding engine backs both Session.BindFields (document root lookups) and
// WebElement.BindFields (descendant-only lookups).
type scopeFinder interface {
	bindFindOne(l Locator) (WebElement, error)
	bindFindAll(l Locator) ([]WebElement, error)
	bindSession() *Session
}

func (s Session) bindFindOne(l Locator) (WebElement, error) {
	return s.FindElement(l.Using, l.Value)
}

func (s Session) bindFindAll(l Locator) ([]WebElement, error) {
	return s.FindElements(l.Using, l.Value)
}

func (s Session) bindSession() *Session { return &s }

func (e WebElement) bindFindOne(l Locator) (WebElement, error) {
	return e.FindElement(l.Using, l.Value)
}

func (e WebElement) bindFindAll(l Locator) ([]WebElement, error) {
	return e.FindElements(l.Using, l.Value)
}

func (e WebElement) bindSession() *Session { return e.s }

// rect64 is a document-space rectangle stored with checked int64 math.
type rect64 struct {
	x, y, w, h int64
}

// nodeSample caches every remote observation made about one element for
// the duration of a single BindFields call. A new sampler is created for
// every call, so geometry is never retained across calls.
type nodeSample struct {
	visDone bool
	visible bool
	enDone  bool
	enabled bool
	geoDone bool
	rect    rect64
}

type sampler struct {
	s     *Session
	nodes map[string]*nodeSample
}

// identity follows the current session and the remote element id, never
// the Go pointer address of a WebElement value.
func (sm *sampler) key(e WebElement) string {
	return sm.s.Id + "|" + e.id
}

func (sm *sampler) node(e WebElement) *nodeSample {
	k := sm.key(e)
	n := sm.nodes[k]
	if n == nil {
		n = &nodeSample{}
		sm.nodes[k] = n
	}
	return n
}

func (sm *sampler) visibility(e WebElement) (bool, error) {
	n := sm.node(e)
	if n.visDone {
		return n.visible, nil
	}
	v, err := e.IsDisplayed()
	if err != nil {
		return false, err
	}
	n.visible, n.visDone = v, true
	return v, nil
}

func (sm *sampler) enabled(e WebElement) (bool, error) {
	n := sm.node(e)
	if n.enDone {
		return n.enabled, nil
	}
	v, err := e.IsEnabled()
	if err != nil {
		return false, err
	}
	n.enabled, n.enDone = v, true
	return v, nil
}

func (sm *sampler) geometry(e WebElement) (rect64, error) {
	n := sm.node(e)
	if n.geoDone {
		return n.rect, nil
	}
	pos, err := e.GetLocation()
	if err != nil {
		return rect64{}, err
	}
	size, err := e.Size()
	if err != nil {
		return rect64{}, err
	}
	n.rect = rect64{
		x: int64(pos.X),
		y: int64(pos.Y),
		w: int64(size.Width),
		h: int64(size.Height),
	}
	n.geoDone = true
	return n.rect, nil
}

type bindEdge struct {
	rank int
	cost int64
}

// BindFields builds a complete one-to-one binding plan between the slots
// (their labels) and the controls returned by their Candidates locators.
// Lookups start from the document root. The returned slice follows the
// order of slots and contains ordinary WebElement values that can be used
// with SendKeys and every other existing element method.
//
// Binding only performs lookups, displayed/enabled checks and GetLocation
// /Size reads; it never fills, clicks, scrolls (GetLocationInView is not
// used) or runs JavaScript. An empty slots slice returns a non-nil empty
// slice. Empty or duplicate slot Names and a negative maxGap are rejected
// before any request is sent.
func (s Session) BindFields(slots []FieldSlot, maxGap int) ([]WebElement, error) {
	return bindFields(s, slots, maxGap)
}

// BindFields behaves like Session.BindFields but every label and candidate
// lookup is performed through the receiver element's descendant lookup
// endpoints. The search never falls back to document root lookups.
func (e WebElement) BindFields(slots []FieldSlot, maxGap int) ([]WebElement, error) {
	return bindFields(e, slots, maxGap)
}

func bindFields(f scopeFinder, slots []FieldSlot, maxGap int) ([]WebElement, error) {
	// Pure input validation: no protocol request may have been issued yet.
	if maxGap < 0 {
		return nil, bindFail("", "validation", errors.New("maxGap must not be negative"))
	}
	if len(slots) == 0 {
		return []WebElement{}, nil
	}
	names := make(map[string]bool, len(slots))
	for _, sl := range slots {
		if sl.Name == "" {
			return nil, bindFail("", "validation", errors.New("field slot Name must not be empty"))
		}
		if names[sl.Name] {
			return nil, bindFail(sl.Name, "validation", fmt.Errorf("duplicate field slot Name %q", sl.Name))
		}
		names[sl.Name] = true
	}

	sess := f.bindSession()
	sm := &sampler{s: sess, nodes: make(map[string]*nodeSample)}

	// controls holds one WebElement per distinct remote id, in global rank
	// order: first appearance across the Candidates results, field by field.
	controls := make([]WebElement, 0)
	ranks := make(map[string]int)
	edges := make([][]bindEdge, len(slots))

	rankOf := func(e WebElement) int {
		k := sm.key(e)
		if r, ok := ranks[k]; ok {
			return r
		}
		r := len(controls)
		ranks[k] = r
		controls = append(controls, e)
		return r
	}

	for i, sl := range slots {
		labelElem, err := f.bindFindOne(sl.Label)
		if err != nil {
			return nil, bindFail(sl.Name, "label lookup", err)
		}
		visible, err := sm.visibility(labelElem)
		if err != nil {
			return nil, bindFail(sl.Name, "label visibility", err)
		}
		if !visible {
			return nil, bindFail(sl.Name, "label visibility", errors.New("label element is not displayed"))
		}
		lr, err := sm.geometry(labelElem)
		if err != nil {
			return nil, bindFail(sl.Name, "label geometry", err)
		}
		if lr.w <= 0 || lr.h <= 0 {
			return nil, bindFail(sl.Name, "label geometry",
				fmt.Errorf("label rectangle has non-positive area: width=%d height=%d", lr.w, lr.h))
		}

		candElems, err := f.bindFindAll(sl.Candidates)
		if err != nil {
			return nil, bindFail(sl.Name, "candidate lookup", err)
		}
		seenInSlot := make(map[string]bool)
		for _, cand := range candElems {
			key := sm.key(cand)
			rank := rankOf(cand) // rank is assigned from lookup order, even if the control is later filtered
			if seenInSlot[key] {
				continue // the same remote id counts as a single control
			}
			seenInSlot[key] = true

			visible, err := sm.visibility(cand)
			if err != nil {
				return nil, bindFail(sl.Name, "candidate visibility", err)
			}
			if !visible {
				continue
			}
			enabled, err := sm.enabled(cand)
			if err != nil {
				return nil, bindFail(sl.Name, "candidate enabled", err)
			}
			if !enabled {
				continue
			}
			cr, err := sm.geometry(cand)
			if err != nil {
				return nil, bindFail(sl.Name, "candidate geometry", err)
			}
			if cr.w <= 0 || cr.h <= 0 {
				continue
			}
			cost, eligible, err := edgeCost(lr, cr, int64(maxGap))
			if err != nil {
				return nil, bindFail(sl.Name, "edge computation", err)
			}
			if !eligible {
				continue
			}
			edges[i] = append(edges[i], bindEdge{rank: rank, cost: cost})
		}
	}

	for i := range edges {
		if len(edges[i]) == 0 {
			return nil, bindFail(slots[i].Name, "assignment",
				errors.New("field has no eligible control"))
		}
	}

	assignment, ok := minimumAssignment(edges, len(slots), len(controls))
	if !ok {
		all := make([]string, len(slots))
		for i, sl := range slots {
			all[i] = sl.Name
		}
		return nil, bindFail("", "assignment",
			fmt.Errorf("no one-to-one assignment covers all fields: %s", strings.Join(all, ", ")))
	}

	plan := make([]WebElement, len(slots))
	for i, r := range assignment {
		plan[i] = controls[r]
	}
	return plan, nil
}

// edgeCost applies the binding rules. It returns ok=false when the label
// and control do not form an eligible edge. Every coordinate operation is
// checked for int64 overflow.
func edgeCost(label, control rect64, maxGap int64) (cost int64, ok bool, err error) {
	labelRight, overflow := add64(label.x, label.w)
	if overflow {
		return 0, false, errors.New("overflow computing label right edge")
	}
	labelBottom, overflow := add64(label.y, label.h)
	if overflow {
		return 0, false, errors.New("overflow computing label bottom edge")
	}
	controlBottom, overflow := add64(control.y, control.h)
	if overflow {
		return 0, false, errors.New("overflow computing control bottom edge")
	}

	gap, overflow := sub64(control.x, labelRight)
	if overflow {
		return 0, false, errors.New("overflow computing horizontal gap")
	}
	if gap < 0 || gap > maxGap {
		return 0, false, nil
	}

	// Positive-length vertical overlap only; touching boundaries do not count.
	top := label.y
	if control.y > top {
		top = control.y
	}
	bottom := labelBottom
	if controlBottom < bottom {
		bottom = controlBottom
	}
	if top >= bottom {
		return 0, false, nil
	}

	labelCenter2, overflow := mul64(2, label.y)
	if overflow {
		return 0, false, errors.New("overflow computing doubled label top")
	}
	labelCenter2, overflow = add64(labelCenter2, label.h)
	if overflow {
		return 0, false, errors.New("overflow computing label vertical center")
	}
	controlCenter2, overflow := mul64(2, control.y)
	if overflow {
		return 0, false, errors.New("overflow computing doubled control top")
	}
	controlCenter2, overflow = add64(controlCenter2, control.h)
	if overflow {
		return 0, false, errors.New("overflow computing control vertical center")
	}
	diff, overflow := sub64(controlCenter2, labelCenter2)
	if overflow {
		return 0, false, errors.New("overflow computing vertical center offset")
	}
	centerOffset, overflow := abs64(diff)
	if overflow {
		return 0, false, errors.New("overflow taking absolute vertical center offset")
	}
	twiceGap, overflow := mul64(2, gap)
	if overflow {
		return 0, false, errors.New("overflow doubling horizontal gap")
	}
	cost, overflow = add64(twiceGap, centerOffset)
	if overflow {
		return 0, false, errors.New("overflow computing edge cost")
	}
	return cost, true, nil
}

// minimumAssignment solves the minimum-weight one-to-one assignment with
// the Hungarian algorithm (O(n^3)); permutations are never enumerated.
//
// Equal-cost solutions are broken by lexicographically smallest assignment
// vector (field order, entries are global control ranks). Each edge weight
// is encoded as cost*B + rank*M^(n-1-i), with B = M^n, so minimizing the
// encoded big integer first minimizes total cost and then the rank vector.
func minimumAssignment(edges [][]bindEdge, n, m int) ([]int, bool) {
	if m < n {
		return nil, false
	}
	bigM := big.NewInt(int64(m))
	base := new(big.Int).Exp(bigM, big.NewInt(int64(n)), nil) // B = M^n
	powers := make([]*big.Int, n)
	for i := range powers {
		powers[i] = new(big.Int).Exp(bigM, big.NewInt(int64(n-1-i)), nil)
	}

	// A non-edge must outweigh every assignment made of real encoded
	// weights. The largest possible real plan totals n such weights, so
	// n times the maximum encoded weight plus one is always sufficient.
	var maxWeight big.Int
	edgeCount := 0
	for _, row := range edges {
		for _, e := range row {
			edgeCount++
			w := new(big.Int).Mul(big.NewInt(e.cost), base)
			w.Add(w, new(big.Int).Mul(big.NewInt(int64(e.rank)), powers[0]))
			if w.Cmp(&maxWeight) > 0 {
				maxWeight.Set(w)
			}
		}
	}
	if edgeCount == 0 {
		return nil, false
	}
	nonEdge := new(big.Int).Mul(big.NewInt(int64(n)), &maxWeight)
	nonEdge.Add(nonEdge, big.NewInt(1))

	matrix := make([][]*big.Int, n)
	for i := range matrix {
		matrix[i] = make([]*big.Int, m)
		known := make(map[int]int64, len(edges[i]))
		for _, e := range edges[i] {
			known[e.rank] = e.cost
		}
		for j := 0; j < m; j++ {
			c, found := known[j]
			if !found {
				matrix[i][j] = nonEdge
				continue
			}
			w := new(big.Int).Mul(big.NewInt(c), base)
			w.Add(w, new(big.Int).Mul(big.NewInt(int64(j)), powers[i]))
			matrix[i][j] = w
		}
	}

	// Hungarian method, 1-indexed columns (e-maxx layout), big.Int costs.
	u := make([]big.Int, n+1)
	v := make([]big.Int, m+1)
	p := make([]int, m+1)
	way := make([]int, m+1)
	for i := 1; i <= n; i++ {
		p[0] = i
		j0 := 0
		minv := make([]big.Int, m+1)
		used := make([]bool, m+1)
		for j := 1; j <= m; j++ {
			minv[j].Set(nonEdge)
		}
		var delta big.Int
		for {
			used[j0] = true
			i0 := p[j0]
			delta.Set(nonEdge)
			j1 := 0
			for j := 1; j <= m; j++ {
				if used[j] {
					continue
				}
				var cur big.Int
				cur.Sub(matrix[i0-1][j-1], &u[i0]).Sub(&cur, &v[j])
				if cur.Cmp(&minv[j]) < 0 {
					minv[j].Set(&cur)
					way[j] = j0
				}
				if minv[j].Cmp(&delta) < 0 {
					delta.Set(&minv[j])
					j1 = j
				}
			}
			for j := 0; j <= m; j++ {
				if used[j] {
					u[p[j]].Add(&u[p[j]], &delta)
					v[j].Sub(&v[j], &delta)
				} else {
					minv[j].Sub(&minv[j], &delta)
				}
			}
			j0 = j1
			if p[j0] == 0 {
				break
			}
		}
		for {
			j1 := way[j0]
			p[j0] = p[j1]
			j0 = j1
			if j0 == 0 {
				break
			}
		}
	}

	assignment := make([]int, n)
	for j := 1; j <= m; j++ {
		if p[j] != 0 {
			assignment[p[j]-1] = j - 1
		}
	}
	// The sentinel weights always yield a result, but a matching that had to
	// use one of them is not a real perfect matching.
	for i, r := range assignment {
		if matrix[i][r] == nonEdge {
			return nil, false
		}
	}
	return assignment, true
}

// Checked int64 arithmetic.

// int64SignMask has only the sign bit set; math.MinInt64 equals 1<<63.
const int64SignMask = math.MinInt64

func add64(a, b int64) (int64, bool) {
	s, _ := bits.Add64(uint64(a), uint64(b), 0)
	r := int64(s)
	if (r^a)&(r^b)&int64SignMask != 0 {
		return 0, true
	}
	return r, false
}

func sub64(a, b int64) (int64, bool) {
	d, _ := bits.Sub64(uint64(a), uint64(b), 0)
	r := int64(d)
	if (a^b)&(a^r)&int64SignMask != 0 {
		return 0, true
	}
	return r, false
}

func mul64(a, b int64) (int64, bool) {
	hi, lo := bits.Mul64(uint64(a), uint64(b))
	// Convert the unsigned high word into the high word of the signed
	// product (Hacker's Delight correction).
	if a < 0 {
		hi -= uint64(b)
	}
	if b < 0 {
		hi -= uint64(a)
	}
	// The signed product is valid exactly when hi is the sign extension
	// of the low 64 bits.
	if hi != uint64(int64(lo)>>63) {
		return 0, true
	}
	return int64(lo), false
}

func abs64(a int64) (int64, bool) {
	if a == math.MinInt64 {
		return 0, true
	}
	if a < 0 {
		return -a, false
	}
	return a, false
}
