// Copyright 2013 Federico Sogaro. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package webdriver

import (
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"math/rand"
	"net/http"
	"net/http/httptest"
	"sort"
	"strings"
	"testing"
)

// fakeNode is one remote element as modeled by the test protocol server.
type fakeNode struct {
	id        string
	displayed bool
	enabled   bool
	rect      [4]int // x, y, width, height
	// finds maps a locator Value to the element ids returned when this node
	// is the lookup scope. The document root uses the root node's finds.
	finds map[string][]string
	// failing endpoints ("displayed", "enabled", "location", "size") return
	// a protocol error instead of their normal reply.
	failing map[string]string
}

// fakeWorld is a deterministic in-memory JSON Wire Protocol endpoint.
type fakeWorld struct {
	t      *testing.T
	nodes  map[string]*fakeNode
	root   *fakeNode
	calls  []string
	counts map[string]int
	typed  map[string][]string
}

func newFakeWorld(t *testing.T, root *fakeNode, nodes ...*fakeNode) *fakeWorld {
	w := &fakeWorld{
		t:      t,
		nodes:  make(map[string]*fakeNode),
		root:   root,
		counts: make(map[string]int),
		typed:  make(map[string][]string),
	}
	w.nodes[root.id] = root
	for _, n := range nodes {
		w.nodes[n.id] = n
	}
	return w
}

func (w *fakeWorld) server() *httptest.Server {
	return httptest.NewServer(http.HandlerFunc(w.handle))
}

func (w *fakeWorld) writeErr(rw http.ResponseWriter, status int, message string) {
	w.t.Helper()
	rw.WriteHeader(http.StatusOK)
	json.NewEncoder(rw).Encode(map[string]interface{}{
		"sessionId": "s",
		"status":    status,
		"value":     map[string]string{"message": message},
	})
}

func (w *fakeWorld) writeValue(rw http.ResponseWriter, value interface{}) {
	w.t.Helper()
	json.NewEncoder(rw).Encode(map[string]interface{}{
		"sessionId": "s",
		"status":    0,
		"value":     value,
	})
}

func (w *fakeWorld) handle(rw http.ResponseWriter, r *http.Request) {
	w.calls = append(w.calls, r.Method+" "+r.URL.Path)
	path := strings.TrimPrefix(r.URL.Path, "/session/s")
	switch {
	case r.URL.Path == "/session":
		w.writeValue(rw, map[string]interface{}{})
		return
	case path == "/element" && r.Method == "POST":
		w.respondFind(rw, r, w.root, false)
	case path == "/elements" && r.Method == "POST":
		w.respondFind(rw, r, w.root, true)
	case strings.HasSuffix(path, "/element") && r.Method == "POST":
		scope := w.scopeOf(path)
		w.respondFind(rw, r, scope, false)
	case strings.HasSuffix(path, "/elements") && r.Method == "POST":
		scope := w.scopeOf(path)
		w.respondFind(rw, r, scope, true)
	default:
		w.respondElement(rw, r, path)
	}
}

// scopeOf extracts the scope element id from /element/:id/element[s].
func (w *fakeWorld) scopeOf(path string) *fakeNode {
	parts := strings.Split(strings.TrimPrefix(path, "/element/"), "/")
	n := w.nodes[parts[0]]
	if n == nil {
		w.t.Fatalf("test server: unknown scope element %q", parts[0])
	}
	return n
}

func (w *fakeWorld) respondFind(rw http.ResponseWriter, r *http.Request, scope *fakeNode, multi bool) {
	var q struct {
		Using string `json:"using"`
		Value string `json:"value"`
	}
	if err := json.NewDecoder(r.Body).Decode(&q); err != nil {
		w.t.Fatal(err)
	}
	ids := scope.finds[q.Value]
	if !multi {
		if len(ids) != 1 {
			w.writeErr(rw, NoSuchElement, fmt.Sprintf("remote: no such element for %q", q.Value))
			return
		}
		ids = ids[:1]
	}
	out := make([]map[string]string, len(ids))
	for i, id := range ids {
		out[i] = map[string]string{"ELEMENT": id}
	}
	if multi {
		w.writeValue(rw, out)
	} else {
		w.writeValue(rw, out[0])
	}
}

func (w *fakeWorld) respondElement(rw http.ResponseWriter, r *http.Request, path string) {
	parts := strings.Split(strings.Trim(path, "/"), "/")
	// element/:id/<endpoint>
	if len(parts) < 3 || parts[0] != "element" {
		w.writeErr(rw, UnknownCommand, "remote: unknown path "+path)
		return
	}
	id, endpoint := parts[1], parts[2]
	node := w.nodes[id]
	if node == nil {
		w.writeErr(rw, StaleElementReference, "remote: stale node "+id)
		return
	}
	w.counts[id+"|"+endpoint]++
	if reason, bad := node.failing[endpoint]; bad {
		w.writeErr(rw, UnknownError, "remote: "+endpoint+" failed: "+reason)
		return
	}
	switch endpoint {
	case "displayed":
		w.writeValue(rw, node.displayed)
	case "enabled":
		w.writeValue(rw, node.enabled)
	case "location":
		w.writeValue(rw, map[string]int{"x": node.rect[0], "y": node.rect[1]})
	case "size":
		w.writeValue(rw, map[string]int{"width": node.rect[2], "height": node.rect[3]})
	case "value":
		var q struct {
			Value []string `json:"value"`
		}
		if err := json.NewDecoder(r.Body).Decode(&q); err != nil {
			w.t.Fatal(err)
		}
		w.typed[id] = append(w.typed[id], strings.Join(q.Value, ""))
		w.writeValue(rw, nil)
	default:
		w.writeErr(rw, UnknownCommand, "remote: unknown endpoint "+endpoint)
	}
}

func (w *fakeWorld) newSession(t *testing.T) (*httptest.Server, *Session) {
	t.Helper()
	server := w.server()
	d := &ChromeDriver{WebDriverCore: WebDriverCore{url: server.URL}}
	s, err := d.NewSession(nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	return server, s
}

func labelSlot(name, labelValue, candidateValue string) FieldSlot {
	return FieldSlot{
		Name:       name,
		Label:      Locator{Using: XPath, Value: labelValue},
		Candidates: Locator{Using: CSS_Selector, Value: candidateValue},
	}
}

func assertBindErr(t *testing.T, err error, field, stage, reason string) {
	t.Helper()
	var be *BindError
	if !errors.As(err, &be) {
		t.Fatalf("expected *BindError, got %T: %v", err, err)
	}
	if be.Field != field {
		t.Fatalf("expected error field %q, got %q", field, be.Field)
	}
	if be.Stage != stage {
		t.Fatalf("expected error stage %q, got %q", stage, be.Stage)
	}
	if reason != "" && !strings.Contains(be.Error(), reason) {
		t.Fatalf("error %q does not contain %q", be.Error(), reason)
	}
}

// bruteForceBest enumerates every permutation of a small graph and returns
// the minimum-cost one-to-one assignment, breaking equal costs with the
// lexicographically smallest rank vector. It is test-only cross checking.
func bruteForceBest(edges [][]bindEdge, n, m int) (vector []int, cost int64, ok bool) {
	costAt := make([][]int64, n)
	has := make([][]bool, n)
	for i := 0; i < n; i++ {
		costAt[i] = make([]int64, m)
		has[i] = make([]bool, m)
		for _, e := range edges[i] {
			costAt[i][e.rank] = e.cost
			has[i][e.rank] = true
		}
	}
	bestCost := int64(math.MaxInt64)
	var bestVector []int
	used := make([]bool, m)
	current := make([]int, n)
	var visit func(i int, total int64)
	visit = func(i int, total int64) {
		if total > bestCost {
			return
		}
		if i == n {
			vec := append([]int(nil), current...)
			if total < bestCost || (total == bestCost && (bestVector == nil || lexLess(vec, bestVector))) {
				bestCost, bestVector = total, vec
			}
			return
		}
		for j := 0; j < m; j++ {
			if used[j] || !has[i][j] {
				continue
			}
			used[j], current[i] = true, j
			visit(i+1, total+costAt[i][j])
			used[j] = false
		}
	}
	visit(0, 0)
	if bestVector == nil {
		return nil, 0, false
	}
	return bestVector, bestCost, true
}

func lexLess(a, b []int) bool {
	for i := range a {
		if a[i] != b[i] {
			return a[i] < b[i]
		}
	}
	return false
}

// TestRPABindMatching covers assignment quality: greedy traps, global
// optimum, equal-cost tie breaking and dense graphs, cross checked against
// exhaustive enumeration on the small cases.
func TestRPABindMatching(t *testing.T) {
	t.Run("greedy fails but global assignment exists", func(t *testing.T) {
		// L1 band [0,20], L2 band [40,60].
		// c2 spans both bands and is L1's cheapest control, but it is L2's
		// only eligible control. A per-field nearest choice strands L2.
		root := &fakeNode{id: "root", finds: map[string][]string{
			"l1": {"l1"}, "l2": {"l2"}, "in": {"c1", "c2"},
		}}
		world := newFakeWorld(t, root,
			&fakeNode{id: "l1", displayed: true, enabled: true, rect: [4]int{0, 0, 10, 20}},
			&fakeNode{id: "l2", displayed: true, enabled: true, rect: [4]int{0, 40, 10, 20}},
			&fakeNode{id: "c1", displayed: true, enabled: true, rect: [4]int{40, 0, 10, 20}},
			&fakeNode{id: "c2", displayed: true, enabled: true, rect: [4]int{11, 0, 10, 60}},
		)
		server, sess := world.newSession(t)
		defer server.Close()

		slots := []FieldSlot{
			labelSlot("first", "l1", "in"),
			labelSlot("second", "l2", "in"),
		}
		plan, err := sess.BindFields(slots, 100)
		if err != nil {
			t.Fatal(err)
		}
		if got := []string{plan[0].id, plan[1].id}; got[0] != "c1" || got[1] != "c2" {
			t.Fatalf("greedy trap: expected [c1 c2], got %v", got)
		}
	})

	t.Run("global minimum instead of first feasible", func(t *testing.T) {
		root := &fakeNode{id: "root", finds: map[string][]string{
			"l1": {"l1"}, "l2": {"l2"}, "in": {"ca", "cb"},
		}}
		// Both controls are reachable from both labels, so two complete
		// plans exist. The lexicographically first plan [ca cb] costs 295,
		// while [cb ca] costs 265 and must be selected.
		world := newFakeWorld(t, root,
			&fakeNode{id: "l1", displayed: true, enabled: true, rect: [4]int{0, 0, 30, 20}},
			&fakeNode{id: "l2", displayed: true, enabled: true, rect: [4]int{0, 40, 10, 20}},
			&fakeNode{id: "ca", displayed: true, enabled: true, rect: [4]int{100, 15, 10, 50}},
			&fakeNode{id: "cb", displayed: true, enabled: true, rect: [4]int{40, 5, 10, 55}},
		)
		server, sess := world.newSession(t)
		defer server.Close()

		slots := []FieldSlot{
			labelSlot("first", "l1", "in"),
			labelSlot("second", "l2", "in"),
		}
		plan, err := sess.BindFields(slots, 1000)
		if err != nil {
			t.Fatal(err)
		}
		if got := []string{plan[0].id, plan[1].id}; got[0] != "cb" || got[1] != "ca" {
			t.Fatalf("expected optimum [cb ca], got %v", got)
		}

		// Exhaustive cross check on the same edge graph.
		lr1 := rect64{0, 0, 30, 20}
		lr2 := rect64{0, 40, 10, 20}
		crA := rect64{100, 15, 10, 50}
		crB := rect64{40, 5, 10, 55}
		edges := make([][]bindEdge, 2)
		for i, lr := range []rect64{lr1, lr2} {
			for rank, cr := range []rect64{crA, crB} {
				cost, eligible, err := edgeCost(lr, cr, 1000)
				if err != nil || !eligible {
					t.Fatalf("edge unexpectedly missing: %v %v", eligible, err)
				}
				edges[i] = append(edges[i], bindEdge{rank: rank, cost: cost})
			}
		}
		vector, bruteCost, ok := bruteForceBest(edges, 2, 2)
		if !ok {
			t.Fatal("brute force found no assignment")
		}
		var total int64
		for i, e := range edges {
			for _, cand := range e {
				if cand.rank == vector[i] {
					total += cand.cost
				}
			}
		}
		if total != bruteCost {
			t.Fatalf("cost mismatch: engine %d brute %d", total, bruteCost)
		}
		wantIDs := []string{"ca", "cb"}
		for i, r := range vector {
			if plan[i].id != wantIDs[r] {
				t.Fatalf("brute vector %v does not match plan", vector)
			}
		}
	})

	t.Run("equal costs break to lexicographically smallest vector", func(t *testing.T) {
		// Uniform 2x2 graph: both perfect matches cost exactly 20.
		edges := [][]bindEdge{
			{{rank: 0, cost: 10}, {rank: 1, cost: 10}},
			{{rank: 0, cost: 10}, {rank: 1, cost: 10}},
		}
		vector, cost, ok := bruteForceBest(edges, 2, 2)
		if !ok || cost != 20 {
			t.Fatalf("brute force setup wrong: %v %d %v", vector, cost, ok)
		}
		got, found := minimumAssignment(edges, 2, 2)
		if !found {
			t.Fatal("minimumAssignment reported no match")
		}
		want := []int{0, 1} // [ca cb], lexicographically below [cb ca]
		for i := range want {
			if got[i] != want[i] {
				t.Fatalf("tie break: expected %v, got %v (brute %v)", want, got, vector)
			}
		}

		// Three-way tie on a 3x3 graph: expect the globally smallest vector.
		edges3 := [][]bindEdge{
			{{rank: 0, cost: 1}, {rank: 1, cost: 1}, {rank: 2, cost: 1}},
			{{rank: 0, cost: 1}, {rank: 1, cost: 1}, {rank: 2, cost: 1}},
			{{rank: 0, cost: 1}, {rank: 1, cost: 1}, {rank: 2, cost: 1}},
		}
		got3, found := minimumAssignment(edges3, 3, 3)
		if !found {
			t.Fatal("3x3 match missing")
		}
		want3 := []int{0, 1, 2}
		for i := range want3 {
			if got3[i] != want3[i] {
				t.Fatalf("3x3 tie break: expected %v, got %v", want3, got3)
			}
		}
	})

	t.Run("edge geometry rules", func(t *testing.T) {
		label := rect64{0, 0, 10, 20} // right 10, band [0,20]
		cases := []struct {
			name     string
			control  rect64
			maxGap   int64
			eligible bool
		}{
			{"touching right edge gap zero", rect64{10, 0, 10, 20}, 0, true},
			{"gap within maxGap", rect64{15, 0, 10, 20}, 5, true},
			{"gap beyond maxGap", rect64{16, 0, 10, 20}, 5, false},
			{"control left of label", rect64{9, 0, 10, 20}, 100, false},
			{"vertical boundaries only touch", rect64{11, 20, 10, 10}, 100, false},
			{"negative coordinates but left of label", rect64{-100, -50, 10, 20}, 100, false},
			{"negative coordinates with gap and overlap", rect64{12, -10, 10, 15}, 5, true},
		}
		for _, c := range cases {
			_, eligible, err := edgeCost(label, c.control, c.maxGap)
			if err != nil {
				t.Fatalf("%s: unexpected error %v", c.name, err)
			}
			if eligible != c.eligible {
				t.Fatalf("%s: expected eligible=%v", c.name, c.eligible)
			}
		}
		// Entirely negative document coordinates are legal: label right edge
		// is -10, control left is -10 (gap zero), bands intersect on (-10, 0).
		negLabel := rect64{x: -20, y: -20, w: 10, h: 20}
		negControl := rect64{x: -10, y: -10, w: 10, h: 20}
		if _, eligible, err := edgeCost(negLabel, negControl, 0); err != nil || !eligible {
			t.Fatalf("negative-coordinate edge missing: eligible=%v err=%v", eligible, err)
		}
		// Cost formula check: gap 5, vertical center2 delta 4 -> 2*5+4 = 14.
		control := rect64{15, 2, 10, 20} // center2 = 24, label center2 = 20
		cost, eligible, err := edgeCost(label, control, 5)
		if err != nil || !eligible {
			t.Fatalf("cost case missing: %v %v", eligible, err)
		}
		if cost != 14 {
			t.Fatalf("expected cost 14, got %d", cost)
		}
	})

	t.Run("random small graphs match exhaustive search", func(t *testing.T) {
		rng := rand.New(rand.NewSource(20261114))
		for trial := 0; trial < 200; trial++ {
			n := 2 + rng.Intn(4)
			m := n + rng.Intn(3)
			edges := make([][]bindEdge, n)
			for i := 0; i < n; i++ {
				for j := 0; j < m; j++ {
					if rng.Intn(3) == 0 {
						continue // sparse graph
					}
					edges[i] = append(edges[i], bindEdge{rank: j, cost: int64(rng.Intn(20))})
				}
			}
			wantVector, _, bruteOK := bruteForceBest(edges, n, m)
			got, engineOK := minimumAssignment(edges, n, m)
			if bruteOK != engineOK {
				t.Fatalf("trial %d: feasibility mismatch brute=%v engine=%v", trial, bruteOK, engineOK)
			}
			if bruteOK {
				for i := range wantVector {
					if got[i] != wantVector[i] {
						t.Fatalf("trial %d: engine %v != brute %v", trial, got, wantVector)
					}
				}
			}
		}
	})

	t.Run("dense 48 fields and 48 controls", func(t *testing.T) {
		finds := map[string][]string{"in": {}}
		nodes := []*fakeNode{
			{id: "root", finds: map[string][]string{}},
		}
		for i := 0; i < 48; i++ {
			id := fmt.Sprintf("l%02d", i)
			nodes[0].finds[id] = []string{id}
			nodes = append(nodes, &fakeNode{
				id: id, displayed: true, enabled: true,
				rect: [4]int{0, 2 * i, 10, 200},
			})
		}
		for j := 0; j < 48; j++ {
			id := fmt.Sprintf("c%02d", j)
			finds["in"] = append(finds["in"], id)
			nodes = append(nodes, &fakeNode{
				id: id, displayed: true, enabled: true,
				rect: [4]int{20, 2 * j, 10, 200},
			})
		}
		nodes[0].finds["in"] = finds["in"]
		world := newFakeWorld(t, nodes[0], nodes[1:]...)
		server, sess := world.newSession(t)
		defer server.Close()

		slots := make([]FieldSlot, 48)
		for i := range slots {
			slots[i] = labelSlot(fmt.Sprintf("field%02d", i), fmt.Sprintf("l%02d", i), "in")
		}
		plan, err := sess.BindFields(slots, 100)
		if err != nil {
			t.Fatal(err)
		}
		if len(plan) != 48 {
			t.Fatalf("expected 48 bound controls, got %d", len(plan))
		}
		seen := make(map[string]bool)
		for i, e := range plan {
			want := fmt.Sprintf("c%02d", i)
			if e.id != want {
				t.Fatalf("field %d: expected %s, got %s", i, want, e.id)
			}
			if seen[e.id] {
				t.Fatalf("control %s assigned twice", e.id)
			}
			seen[e.id] = true
			if e.s == nil || e.s.Id != sess.Id || e.s.wd != sess.wd {
				t.Fatal("returned element lost its session or id")
			}
		}
		// Every remote control is looked at exactly once despite appearing in
		// all 48 candidate lists.
		for j := 0; j < 48; j++ {
			id := fmt.Sprintf("c%02d", j)
			for _, endpoint := range []string{"displayed", "enabled", "location", "size"} {
				if world.counts[id+"|"+endpoint] != 1 {
					t.Fatalf("%s %s sampled %d times", id, endpoint, world.counts[id+"|"+endpoint])
				}
			}
		}
	})
}

// TestRPABindProtocol covers wire-level behavior: lookup scopes, remote id
// identity, single sampling, validation before any request and real
// SendKeys traffic on the returned elements.
func TestRPABindProtocol(t *testing.T) {
	t.Run("session scope uses document lookup endpoints", func(t *testing.T) {
		root := &fakeNode{id: "root", finds: map[string][]string{
			"l1": {"l1"}, "l2": {"l2"}, "in": {"c1", "c2"},
		}}
		world := newFakeWorld(t, root,
			&fakeNode{id: "l1", displayed: true, enabled: true, rect: [4]int{0, 0, 10, 20}},
			&fakeNode{id: "l2", displayed: true, enabled: true, rect: [4]int{0, 40, 10, 20}},
			&fakeNode{id: "c1", displayed: true, enabled: true, rect: [4]int{11, 0, 10, 20}},
			&fakeNode{id: "c2", displayed: true, enabled: true, rect: [4]int{11, 40, 10, 20}},
		)
		server, sess := world.newSession(t)
		defer server.Close()

		slots := []FieldSlot{
			labelSlot("first", "l1", "in"),
			labelSlot("second", "l2", "in"),
		}
		plan, err := sess.BindFields(slots, 100)
		if err != nil {
			t.Fatal(err)
		}
		// State and geometry naturally address /element/:id/<endpoint>; only
		// the lookup endpoints prove which scope was used.
		for _, call := range world.calls {
			path := strings.SplitN(call, " ", 2)[1]
			rel := strings.TrimPrefix(path, "/session/s")
			if strings.HasSuffix(rel, "/element") || strings.HasSuffix(rel, "/elements") {
				if rel != "/element" && rel != "/elements" {
					t.Fatalf("document-scope binding issued a descendant lookup: %s", call)
				}
			}
		}
		if plan[0].id != "c1" || plan[1].id != "c2" {
			t.Fatalf("unexpected plan %s %s", plan[0].id, plan[1].id)
		}
	})

	t.Run("element scope never falls back to document lookups", func(t *testing.T) {
		// Document root deliberately cannot resolve the slot locators.
		root := &fakeNode{id: "root", finds: map[string][]string{
			"card": {"card"},
		}}
		card := &fakeNode{
			id: "card",
			finds: map[string][]string{
				"l1": {"l1"}, "l2": {"l2"}, "in": {"c1", "c2"},
			},
		}
		world := newFakeWorld(t, root, card,
			&fakeNode{id: "l1", displayed: true, enabled: true, rect: [4]int{0, 0, 10, 20}},
			&fakeNode{id: "l2", displayed: true, enabled: true, rect: [4]int{0, 40, 10, 20}},
			&fakeNode{id: "c1", displayed: true, enabled: true, rect: [4]int{11, 0, 10, 20}},
			&fakeNode{id: "c2", displayed: true, enabled: true, rect: [4]int{11, 40, 10, 20}},
		)
		server, sess := world.newSession(t)
		defer server.Close()

		container, err := sess.FindElement(ID, "card")
		if err != nil {
			t.Fatal(err)
		}
		slots := []FieldSlot{
			labelSlot("first", "l1", "in"),
			labelSlot("second", "l2", "in"),
		}
		before := len(world.calls)
		plan, err := container.BindFields(slots, 100)
		if err != nil {
			t.Fatal(err)
		}
		// Only requests issued by the binding itself are inspected.
		for _, call := range world.calls[before:] {
			path := strings.SplitN(call, " ", 2)[1]
			if !strings.HasPrefix(path, "/session/s/") {
				continue
			}
			rel := strings.TrimPrefix(path, "/session/s")
			if rel == "/element" || rel == "/elements" {
				t.Fatalf("binding fell back to a document-root lookup: %s", call)
			}
			if strings.HasSuffix(rel, "/element") || strings.HasSuffix(rel, "/elements") {
				if !strings.HasPrefix(rel, "/element/card/") {
					t.Fatalf("lookup outside container scope: %s", call)
				}
			}
		}
		if plan[0].id != "c1" || plan[1].id != "c2" {
			t.Fatalf("unexpected scoped plan %s %s", plan[0].id, plan[1].id)
		}
	})

	t.Run("duplicate remote ids share one identity and one sample", func(t *testing.T) {
		root := &fakeNode{id: "root", finds: map[string][]string{
			"l1": {"l1"}, "l2": {"l2"},
			// Duplicates inside one list and across lists.
			"in1": {"c1", "c1", "c2"},
			"in2": {"c2", "c1"},
		}}
		world := newFakeWorld(t, root,
			&fakeNode{id: "l1", displayed: true, enabled: true, rect: [4]int{0, 0, 10, 20}},
			&fakeNode{id: "l2", displayed: true, enabled: true, rect: [4]int{0, 40, 10, 20}},
			&fakeNode{id: "c1", displayed: true, enabled: true, rect: [4]int{11, 0, 10, 20}},
			&fakeNode{id: "c2", displayed: true, enabled: true, rect: [4]int{11, 40, 10, 20}},
		)
		server, sess := world.newSession(t)
		defer server.Close()

		slots := []FieldSlot{
			labelSlot("first", "l1", "in1"),
			labelSlot("second", "l2", "in2"),
		}
		plan, err := sess.BindFields(slots, 100)
		if err != nil {
			t.Fatal(err)
		}
		if plan[0].id == plan[1].id {
			t.Fatal("two fields bound to the same control")
		}
		for _, id := range []string{"c1", "c2"} {
			for _, endpoint := range []string{"displayed", "enabled", "location", "size"} {
				if world.counts[id+"|"+endpoint] != 1 {
					t.Fatalf("%s %s sampled %d times, want 1", id, endpoint, world.counts[id+"|"+endpoint])
				}
			}
		}
	})

	t.Run("label reused as candidate samples visibility once", func(t *testing.T) {
		root := &fakeNode{id: "root", finds: map[string][]string{
			"l1": {"shared"}, "in": {"shared", "c2"},
		}}
		// "shared" is both the label and a candidate: its own control copy
		// sits left of the label edge so it cannot be bound, but every
		// observation is still made exactly once for that remote id.
		world := newFakeWorld(t, root,
			&fakeNode{id: "shared", displayed: true, enabled: true, rect: [4]int{15, 0, 10, 20}},
			&fakeNode{id: "c2", displayed: true, enabled: true, rect: [4]int{30, 5, 10, 10}},
		)
		server, sess := world.newSession(t)
		defer server.Close()

		plan, err := sess.BindFields([]FieldSlot{labelSlot("first", "l1", "in")}, 100)
		if err != nil {
			t.Fatal(err)
		}
		if plan[0].id != "c2" {
			t.Fatalf("expected c2, got %s", plan[0].id)
		}
		if world.counts["shared|displayed"] != 1 {
			t.Fatalf("shared node displayed sampled %d times", world.counts["shared|displayed"])
		}
		if world.counts["shared|enabled"] != 1 {
			t.Fatalf("shared node enabled sampled %d times", world.counts["shared|enabled"])
		}
		if world.counts["shared|location"] != 1 || world.counts["shared|size"] != 1 {
			t.Fatalf("shared node geometry sampled multiple times: loc=%d size=%d",
				world.counts["shared|location"], world.counts["shared|size"])
		}
	})

	t.Run("validation rejects before any request", func(t *testing.T) {
		root := &fakeNode{id: "root", finds: map[string][]string{}}
		world := newFakeWorld(t, root)
		server, sess := world.newSession(t)
		defer server.Close()

		before := len(world.calls)
		good := labelSlot("first", "l1", "in")
		if _, err := sess.BindFields(nil, 0); err != nil {
			t.Fatalf("empty slots must succeed: %v", err)
		}
		empty, err := sess.BindFields([]FieldSlot{}, 0)
		if err != nil || empty == nil || len(empty) != 0 {
			t.Fatalf("empty slots must return non-nil empty slice: %v %v", empty, err)
		}
		if _, err := sess.BindFields([]FieldSlot{good, {Name: "", Label: Locator{Value: "x"}, Candidates: Locator{Value: "y"}}}, 0); err == nil {
			t.Fatal("empty Name must be rejected")
		} else {
			assertBindErr(t, err, "", "validation", "")
		}
		if _, err := sess.BindFields([]FieldSlot{good, good}, 0); err == nil {
			t.Fatal("duplicate Name must be rejected")
		} else {
			assertBindErr(t, err, "first", "validation", "duplicate")
		}
		if _, err := sess.BindFields([]FieldSlot{good}, -1); err == nil {
			t.Fatal("negative maxGap must be rejected")
		} else {
			assertBindErr(t, err, "", "validation", "negative")
		}
		if len(world.calls) != before {
			t.Fatalf("validation issued protocol requests: %v", world.calls[before:])
		}
	})

	t.Run("returned elements drive SendKeys on distinct controls", func(t *testing.T) {
		root := &fakeNode{id: "root", finds: map[string][]string{
			"l1": {"l1"}, "l2": {"l2"}, "in": {"c1", "c2"},
		}}
		world := newFakeWorld(t, root,
			&fakeNode{id: "l1", displayed: true, enabled: true, rect: [4]int{0, 0, 10, 20}},
			&fakeNode{id: "l2", displayed: true, enabled: true, rect: [4]int{0, 40, 10, 20}},
			&fakeNode{id: "c1", displayed: true, enabled: true, rect: [4]int{11, 0, 10, 20}},
			&fakeNode{id: "c2", displayed: true, enabled: true, rect: [4]int{11, 40, 10, 20}},
		)
		server, sess := world.newSession(t)
		defer server.Close()

		slots := []FieldSlot{
			labelSlot("user", "l1", "in"),
			labelSlot("city", "l2", "in"),
		}
		plan, err := sess.BindFields(slots, 100)
		if err != nil {
			t.Fatal(err)
		}
		values := []string{"alice", "Ravenna"}
		for i, e := range plan {
			if err := e.SendKeys(values[i]); err != nil {
				t.Fatal(err)
			}
		}
		got := []string{strings.Join(world.typed["c1"], ""), strings.Join(world.typed["c2"], "")}
		sort.Strings(got)
		want := append([]string(nil), values...)
		sort.Strings(want)
		if got[0] != want[0] || got[1] != want[1] {
			t.Fatalf("SendKeys landed on %v, want %v", got, want)
		}
	})

	t.Run("geometry is resampled on every call", func(t *testing.T) {
		root := &fakeNode{id: "root", finds: map[string][]string{
			"l1": {"l1"}, "in": {"c1"},
		}}
		world := newFakeWorld(t, root,
			&fakeNode{id: "l1", displayed: true, enabled: true, rect: [4]int{0, 0, 10, 20}},
			&fakeNode{id: "c1", displayed: true, enabled: true, rect: [4]int{11, 0, 10, 20}},
		)
		server, sess := world.newSession(t)
		defer server.Close()

		slot := labelSlot("first", "l1", "in")
		for k := 0; k < 3; k++ {
			if _, err := sess.BindFields([]FieldSlot{slot}, 100); err != nil {
				t.Fatal(err)
			}
		}
		for _, endpoint := range []string{"displayed", "enabled", "location", "size"} {
			if world.counts["c1|"+endpoint] != 3 {
				t.Fatalf("%s sampled %d times across 3 calls, want 3", endpoint, world.counts["c1|"+endpoint])
			}
		}
	})
}

// TestRPABindFailure covers every failure path: lookup/state/geometry
// protocol errors, hidden or disabled or zero-area controls, label input
// errors, impossible matchings and int64 overflow.
func TestRPABindFailure(t *testing.T) {
	baseNodes := func(overrides ...*fakeNode) (*fakeNode, []*fakeNode) {
		root := &fakeNode{id: "root", finds: map[string][]string{
			"l1": {"l1"}, "l2": {"l2"}, "in1": {"c1", "c2"}, "in2": {"c2"},
		}}
		nodes := []*fakeNode{
			{id: "l1", displayed: true, enabled: true, rect: [4]int{0, 0, 10, 20}},
			{id: "l2", displayed: true, enabled: true, rect: [4]int{0, 40, 10, 20}},
			{id: "c1", displayed: true, enabled: true, rect: [4]int{11, 0, 10, 20}},
			{id: "c2", displayed: true, enabled: true, rect: [4]int{11, 40, 10, 20}},
		}
		byID := map[string]*fakeNode{}
		for _, n := range nodes {
			byID[n.id] = n
		}
		for _, o := range overrides {
			byID[o.id] = o
		}
		out := make([]*fakeNode, 0, len(byID))
		for _, n := range byID {
			out = append(out, n)
		}
		return root, out
	}
	slots := []FieldSlot{
		labelSlot("first", "l1", "in1"),
		labelSlot("second", "l2", "in2"),
	}

	t.Run("label lookup protocol error", func(t *testing.T) {
		root, nodes := baseNodes()
		delete(root.finds, "l1")
		world := newFakeWorld(t, root, nodes...)
		server, sess := world.newSession(t)
		defer server.Close()
		_, err := sess.BindFields(slots, 100)
		assertBindErr(t, err, "first", "label lookup", "no such element")
		var ce *CommandError
		if !errors.As(err, &ce) {
			t.Fatal("original protocol error must remain unwrappable")
		}
	})

	t.Run("label not displayed", func(t *testing.T) {
		root, nodes := baseNodes(&fakeNode{id: "l1", displayed: false, enabled: true, rect: [4]int{0, 0, 10, 20}})
		world := newFakeWorld(t, root, nodes...)
		server, sess := world.newSession(t)
		defer server.Close()
		_, err := sess.BindFields(slots, 100)
		assertBindErr(t, err, "first", "label visibility", "not displayed")
	})

	t.Run("label zero area is an input error", func(t *testing.T) {
		for _, bad := range [][4]int{{0, 0, 0, 20}, {0, 0, 10, 0}, {0, 0, -5, 20}} {
			root, nodes := baseNodes(&fakeNode{id: "l1", displayed: true, enabled: true, rect: bad})
			world := newFakeWorld(t, root, nodes...)
			server, sess := world.newSession(t)
			defer server.Close()
			_, err := sess.BindFields(slots, 100)
			assertBindErr(t, err, "first", "label geometry", "non-positive area")
		}
	})

	t.Run("invisible disabled zero-area candidates are excluded", func(t *testing.T) {
		// Three controls, each rejected for a different reason: hidden,
		// disabled, zero area. Field "first" loses every control, so no
		// complete plan can be delivered even though lookups succeeded.
		root := &fakeNode{id: "root", finds: map[string][]string{
			"l1": {"l1"}, "l2": {"l2"}, "in1": {"c1", "c2", "c3"}, "in2": {"c2"},
		}}
		world := newFakeWorld(t, root,
			&fakeNode{id: "l1", displayed: true, enabled: true, rect: [4]int{0, 0, 10, 20}},
			&fakeNode{id: "l2", displayed: true, enabled: true, rect: [4]int{0, 40, 10, 20}},
			&fakeNode{id: "c1", displayed: false, enabled: true, rect: [4]int{11, 0, 10, 20}},
			&fakeNode{id: "c2", displayed: true, enabled: false, rect: [4]int{11, 0, 10, 20}},
			&fakeNode{id: "c3", displayed: true, enabled: true, rect: [4]int{11, 0, 0, 20}},
		)
		server, sess := world.newSession(t)
		defer server.Close()
		plan, err := sess.BindFields(slots, 100)
		if err == nil {
			t.Fatal("expected failure, got plan", plan)
		}
		if plan != nil {
			t.Fatal("failed binding must return nil plan")
		}
		assertBindErr(t, err, "first", "assignment", "")
		// The visible, enabled zero-area control is still fully observed:
		// a filtered node must be sampled, not silently skipped.
		if world.counts["c3|location"] != 1 || world.counts["c3|size"] != 1 {
			t.Fatalf("zero-area control geometry not sampled: loc=%d size=%d",
				world.counts["c3|location"], world.counts["c3|size"])
		}
	})

	t.Run("partial feasibility still returns nil plan", func(t *testing.T) {
		// first can only take c1, second can only take c1 too: no matching.
		root := &fakeNode{id: "root", finds: map[string][]string{
			"l1": {"l1"}, "l2": {"l2"}, "in1": {"c1"}, "in2": {"c1"},
		}}
		world := newFakeWorld(t, root,
			&fakeNode{id: "l1", displayed: true, enabled: true, rect: [4]int{0, 0, 10, 20}},
			&fakeNode{id: "l2", displayed: true, enabled: true, rect: [4]int{0, 0, 10, 20}},
			&fakeNode{id: "c1", displayed: true, enabled: true, rect: [4]int{11, 0, 10, 20}},
		)
		server, sess := world.newSession(t)
		defer server.Close()
		plan, err := sess.BindFields(slots, 100)
		if plan != nil || err == nil {
			t.Fatalf("expected nil plan and error, got %v %v", plan, err)
		}
		assertBindErr(t, err, "", "assignment", "")
	})

	t.Run("remote state and geometry errors abort", func(t *testing.T) {
		cases := []struct {
			node  string
			endp  string
			stage string
		}{
			{"l1", "displayed", "label visibility"},
			{"l1", "location", "label geometry"},
			{"l1", "size", "label geometry"},
			{"c1", "displayed", "candidate visibility"},
			{"c1", "enabled", "candidate enabled"},
			{"c1", "location", "candidate geometry"},
			{"c1", "size", "candidate geometry"},
		}
		for _, c := range cases {
			root, nodes := baseNodes(&fakeNode{
				id: c.node, displayed: true, enabled: true,
				rect:    [4]int{0, 0, 10, 20},
				failing: map[string]string{c.endp: "boom-" + c.endp},
			})
			world := newFakeWorld(t, root, nodes...)
			server, sess := world.newSession(t)
			_, err := sess.BindFields(slots, 100)
			server.Close()
			field := "first"
			assertBindErr(t, err, field, c.stage, "boom-"+c.endp)
			var ce *CommandError
			if !errors.As(err, &ce) {
				t.Fatalf("%s: original error not preserved: %v", c.endp, err)
			}
		}
	})

	t.Run("stale node is not dropped from the graph", func(t *testing.T) {
		root := &fakeNode{id: "root", finds: map[string][]string{
			"l1": {"l1"}, "in1": {"ghost"},
		}}
		world := newFakeWorld(t, root,
			&fakeNode{id: "l1", displayed: true, enabled: true, rect: [4]int{0, 0, 10, 20}},
		)
		server, sess := world.newSession(t)
		defer server.Close()
		_, err := sess.BindFields([]FieldSlot{labelSlot("first", "l1", "in1")}, 100)
		assertBindErr(t, err, "first", "candidate visibility", "stale")
	})

	t.Run("int64 overflow and illegal coordinates carry field name", func(t *testing.T) {
		maxInt := math.MaxInt64
		root := &fakeNode{id: "root", finds: map[string][]string{
			"l1": {"l1"}, "in1": {"c1"},
		}}
		world := newFakeWorld(t, root,
			&fakeNode{id: "l1", displayed: true, enabled: true, rect: [4]int{maxInt - 1, 0, 10, 20}},
			&fakeNode{id: "c1", displayed: true, enabled: true, rect: [4]int{maxInt, 0, 10, 20}},
		)
		server, sess := world.newSession(t)
		defer server.Close()
		_, err := sess.BindFields([]FieldSlot{labelSlot("first", "l1", "in1")}, 100)
		assertBindErr(t, err, "first", "edge computation", "overflow")

		// Pure arithmetic unit checks for the checked helpers.
		max64 := int64(maxInt)
		min64 := int64(math.MinInt64)
		if _, overflow := add64(max64, 1); !overflow {
			t.Fatal("add64 missed overflow")
		}
		if _, overflow := sub64(min64, 1); !overflow {
			t.Fatal("sub64 missed overflow")
		}
		if _, overflow := mul64(max64, 2); !overflow {
			t.Fatal("mul64 missed overflow")
		}
		if _, overflow := abs64(min64); !overflow {
			t.Fatal("abs64 missed overflow")
		}
		if r, overflow := add64(-1, 1); overflow || r != 0 {
			t.Fatal("add64 normal case wrong")
		}
		if r, overflow := sub64(min64, -1); overflow || r != min64+1 {
			t.Fatal("sub64 normal case wrong")
		}
	})
}
