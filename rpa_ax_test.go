package godet

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"

	"github.com/gorilla/websocket"
)

// axCall records one protocol request seen by the fake browser.
type axCall struct {
	Method string
	Params map[string]interface{}
}

// axBackend is a scripted DevTools endpoint. It speaks just enough HTTP and
// WebSocket for a real godet.RemoteDebugger connection: every request is
// answered by reply, requests and DOM.focus parameters are recorded, and a
// reply error is turned into a protocol-level "error" response.
type axBackend struct {
	mu      sync.Mutex
	calls   []axCall
	focuses []map[string]interface{}
	reply   func(method string, params map[string]interface{}) (map[string]interface{}, error)
}

func (b *axBackend) record(method string, params map[string]interface{}) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.calls = append(b.calls, axCall{Method: method, Params: params})
}

func (b *axBackend) callCount() int {
	b.mu.Lock()
	defer b.mu.Unlock()
	return len(b.calls)
}

func (b *axBackend) methods() []string {
	b.mu.Lock()
	defer b.mu.Unlock()
	out := make([]string, len(b.calls))
	for i, c := range b.calls {
		out[i] = c.Method
	}
	return out
}

func (b *axBackend) frameFor(method string) []string {
	b.mu.Lock()
	defer b.mu.Unlock()
	var frames []string
	for _, c := range b.calls {
		if c.Method != method {
			continue
		}
		if f, ok := c.Params["frameId"]; ok {
			frames = append(frames, f.(string))
		} else {
			frames = append(frames, "")
		}
	}
	return frames
}

func (b *axBackend) focusBackends() []int {
	b.mu.Lock()
	defer b.mu.Unlock()
	out := make([]int, 0, len(b.focuses))
	for _, p := range b.focuses {
		if v, ok := p["backendNodeId"]; ok {
			out = append(out, int(v.(float64)))
		}
	}
	return out
}

// axConnect stands up a local HTTP/WebSocket protocol endpoint and connects
// a real RemoteDebugger to it, exactly like a browser target.
func axConnect(t *testing.T, b *axBackend) (*RemoteDebugger, *httptest.Server) {
	t.Helper()
	mux := http.NewServeMux()
	server := httptest.NewServer(mux)
	mux.HandleFunc("/json/list", func(w http.ResponseWriter, r *http.Request) {
		json.NewEncoder(w).Encode([]map[string]interface{}{{
			"id": "page1", "type": "page",
			"webSocketDebuggerUrl": "ws" + strings.TrimPrefix(server.URL, "http") + "/devtools/page/page1",
		}})
	})
	mux.HandleFunc("/devtools/page/page1", func(w http.ResponseWriter, r *http.Request) {
		ws, err := (&websocket.Upgrader{}).Upgrade(w, r, nil)
		if err != nil {
			return
		}
		defer ws.Close()
		for {
			var q Params
			if err := ws.ReadJSON(&q); err != nil {
				return
			}
			method, _ := q["method"].(string)
			params, _ := q["params"].(map[string]interface{})
			b.record(method, params)

			if method == "DOM.focus" {
				b.mu.Lock()
				b.focuses = append(b.focuses, params)
				b.mu.Unlock()
			}

			result, err := b.reply(method, params)
			if err != nil {
				if ws.WriteJSON(Params{"id": q["id"], "error": Params{"code": 1, "message": err.Error()}}) != nil {
					return
				}
				continue
			}
			if ws.WriteJSON(Params{"id": q["id"], "result": result}) != nil {
				return
			}
		}
	})

	remote, err := Connect(strings.TrimPrefix(server.URL, "http://"), false)
	if err != nil {
		server.Close()
		t.Fatal(err)
	}
	t.Cleanup(func() { remote.Close(); server.Close() })
	return remote, server
}

// axn builds one AX node record as it would appear on the wire.
func axn(id, role, name string, ignored bool, parent string, kids []string, backend int) map[string]interface{} {
	m := map[string]interface{}{"nodeId": id, "ignored": ignored}
	if role != "" {
		m["role"] = map[string]interface{}{"type": "role", "value": role}
	}
	if name != "" {
		m["name"] = map[string]interface{}{"type": "computedString", "value": name}
	}
	if parent != "" {
		m["parentId"] = parent
	}
	if kids != nil {
		m["childIds"] = kids
	}
	if backend != 0 {
		m["backendDOMNodeId"] = backend
	}
	return m
}

func withFrame(m map[string]interface{}, frame string) map[string]interface{} {
	m["frameId"] = frame
	return m
}

func axName(s string) *string { return &s }

// treeResponder answers getRootAXNode/getChildAXNodes from a static node map,
// returning each parent's direct children in declared childIds order.
func treeResponder(rootID string, tree map[string]map[string]interface{}) func(string, map[string]interface{}) (map[string]interface{}, error) {
	return func(method string, params map[string]interface{}) (map[string]interface{}, error) {
		switch method {
		case "Accessibility.getRootAXNode":
			return map[string]interface{}{"node": tree[rootID]}, nil
		case "Accessibility.getChildAXNodes":
			id := params["id"].(string)
			parent, ok := tree[id]
			if !ok {
				return nil, fmt.Errorf("unknown node %q", id)
			}
			var nodes []interface{}
			if raw, ok := parent["childIds"]; ok {
				for _, cid := range raw.([]string) {
					nodes = append(nodes, tree[cid])
				}
			}
			return map[string]interface{}{"nodes": nodes}, nil
		case "DOM.focus":
			return map[string]interface{}{}, nil
		default:
			return nil, fmt.Errorf("unexpected method %q", method)
		}
	}
}

// TestAXClosure covers tree completion and every structural integrity rule:
// out-of-order fragments, missing children fetched on demand, identical and
// conflicting duplicates, duplicate edges, cycles, double parents, dangling
// links, frame boundaries, budgets and a long ignored chain.
func TestAXClosure(t *testing.T) {
	stdLimits := AXLimits{MaxNodes: 1000, MaxRequests: 1000, MaxDepth: 100}

	t.Run("out-of-order fragment with inline grandchild and duplicate", func(t *testing.T) {
		tree := map[string]map[string]interface{}{
			"r": withFrame(axn("r", "RootWebArea", "", false, "", []string{"f", "x"}, 0), "F1"),
			"f": axn("f", "form", "Invoice", false, "r", []string{"g"}, 0),
			"g": axn("g", "group", "Billing", false, "f", []string{"t"}, 0),
			"t": axn("t", "textbox", "Amount", false, "g", nil, 77),
			"x": axn("x", "button", "Go", false, "r", nil, 88),
		}
		backend := &axBackend{}
		backend.reply = func(method string, params map[string]interface{}) (map[string]interface{}, error) {
			switch method {
			case "Accessibility.getRootAXNode":
				return map[string]interface{}{"node": tree["r"]}, nil
			case "Accessibility.getChildAXNodes":
				id := params["id"].(string)
				if id == "r" {
					// Shuffled order, and g arrives early before f is expanded.
					return map[string]interface{}{"nodes": []interface{}{tree["x"], tree["f"], tree["g"]}}, nil
				}
				if id == "g" {
					// Identical duplicate of t is legal.
					return map[string]interface{}{"nodes": []interface{}{tree["t"], tree["t"]}}, nil
				}
				return map[string]interface{}{"nodes": []interface{}{}}, nil
			case "DOM.focus":
				return map[string]interface{}{}, nil
			}
			return nil, fmt.Errorf("unexpected %q", method)
		}
		remote, _ := axConnect(t, backend)

		matches, err := remote.FindAX("F1", []AXStep{
			{Role: "form", Name: axName("Invoice")},
			{Role: "group", Name: axName("Billing")},
			{Role: "textbox", Name: axName("Amount")},
		}, stdLimits)
		if err != nil {
			t.Fatal(err)
		}
		if len(matches) != 1 || matches[0].AXNodeID != "t" || matches[0].BackendNodeID != 77 {
			t.Fatalf("match %+v", matches)
		}
		// root + fetch(r) + fetch(g); f was never fetched because g arrived inline.
		if backend.callCount() != 3 {
			t.Fatalf("requests %d %v", backend.callCount(), backend.methods())
		}
		frames := backend.frameFor("Accessibility.getChildAXNodes")
		for _, f := range frames {
			if f != "F1" {
				t.Fatal("child fetch must stay inside document frame F1, got", f)
			}
		}
		if roots := backend.frameFor("Accessibility.getRootAXNode"); len(roots) != 1 || roots[0] != "F1" {
			t.Fatal("root frameId", roots)
		}
	})

	t.Run("main frame when frameID empty", func(t *testing.T) {
		tree := map[string]map[string]interface{}{
			"r": withFrame(axn("r", "RootWebArea", "", false, "", []string{"b"}, 0), "MAIN"),
			"b": axn("b", "button", "Ok", false, "r", nil, 5),
		}
		backend := &axBackend{reply: treeResponder("r", tree)}
		remote, _ := axConnect(t, backend)

		matches, err := remote.FindAX("", []AXStep{{Role: "button", Name: axName("Ok")}}, stdLimits)
		if err != nil {
			t.Fatal(err)
		}
		if len(matches) != 1 || matches[0].BackendNodeID != 5 {
			t.Fatalf("match %+v", matches)
		}
		roots := backend.frameFor("Accessibility.getRootAXNode")
		if len(roots) != 1 || roots[0] != "" {
			t.Fatal("main-frame root request must omit frameId, got", roots)
		}
		for _, f := range backend.frameFor("Accessibility.getChildAXNodes") {
			if f != "MAIN" {
				t.Fatal("child fetch should adopt root frame MAIN, got", f)
			}
		}
	})

	t.Run("dangling declared child is an error", func(t *testing.T) {
		tree := map[string]map[string]interface{}{
			"r": axn("r", "RootWebArea", "", false, "", []string{"a"}, 0),
		}
		backend := &axBackend{}
		backend.reply = func(method string, _ map[string]interface{}) (map[string]interface{}, error) {
			if method == "Accessibility.getRootAXNode" {
				return map[string]interface{}{"node": tree["r"]}, nil
			}
			// The single allowed child fetch never returns the declared child a.
			return map[string]interface{}{"nodes": []interface{}{}}, nil
		}
		remote, _ := axConnect(t, backend)
		if _, err := remote.FindAX("", []AXStep{{Role: "button"}}, stdLimits); err == nil {
			t.Fatal("expected missing-child error")
		}
		if backend.callCount() != 2 {
			t.Fatal("parent must be fetched exactly once before failing", backend.callCount())
		}
	})

	t.Run("conflicting duplicate records", func(t *testing.T) {
		good := axn("a", "button", "A", false, "r", nil, 1)
		bad := axn("a", "button", "A-CHANGED", false, "r", nil, 1)
		backend := &axBackend{}
		backend.reply = func(method string, _ map[string]interface{}) (map[string]interface{}, error) {
			if method == "Accessibility.getRootAXNode" {
				return map[string]interface{}{"node": axn("r", "RootWebArea", "", false, "", []string{"a"}, 0)}, nil
			}
			return map[string]interface{}{"nodes": []interface{}{good, bad}}, nil
		}
		remote, _ := axConnect(t, backend)
		if _, err := remote.FindAX("", []AXStep{{Role: "button"}}, stdLimits); err == nil {
			t.Fatal("expected conflict error")
		}
	})

	t.Run("duplicate child edge", func(t *testing.T) {
		tree := map[string]map[string]interface{}{
			"r": axn("r", "RootWebArea", "", false, "", []string{"a", "a"}, 0),
			"a": axn("a", "button", "A", false, "r", nil, 1),
		}
		backend := &axBackend{reply: treeResponder("r", tree)}
		remote, _ := axConnect(t, backend)
		if _, err := remote.FindAX("", []AXStep{{Role: "button"}}, stdLimits); err == nil {
			t.Fatal("expected duplicate-edge error")
		}
	})

	t.Run("cycle", func(t *testing.T) {
		tree := map[string]map[string]interface{}{
			"r": axn("r", "RootWebArea", "", false, "", []string{"a"}, 0),
			"a": axn("a", "group", "", false, "r", []string{"b"}, 0),
			"b": axn("b", "group", "", false, "a", []string{"a"}, 0),
		}
		backend := &axBackend{reply: treeResponder("r", tree)}
		remote, _ := axConnect(t, backend)
		if _, err := remote.FindAX("", []AXStep{{Role: "group"}}, stdLimits); err == nil {
			t.Fatal("expected cycle error")
		}
	})

	t.Run("node referenced by two parents", func(t *testing.T) {
		tree := map[string]map[string]interface{}{
			"r": axn("r", "RootWebArea", "", false, "", []string{"a", "b"}, 0),
			"a": axn("a", "group", "", false, "r", []string{"x"}, 0),
			"b": axn("b", "group", "", false, "r", []string{"x"}, 0),
			"x": axn("x", "button", "X", false, "a", nil, 1),
		}
		backend := &axBackend{reply: treeResponder("r", tree)}
		remote, _ := axConnect(t, backend)
		if _, err := remote.FindAX("", []AXStep{{Role: "button"}}, stdLimits); err == nil {
			t.Fatal("expected double-parent error")
		}
	})

	t.Run("parentId does not match referencing parent", func(t *testing.T) {
		tree := map[string]map[string]interface{}{
			"r": axn("r", "RootWebArea", "", false, "", []string{"a"}, 0),
			"a": axn("a", "button", "A", false, "somewhere-else", nil, 1),
		}
		backend := &axBackend{reply: treeResponder("r", tree)}
		remote, _ := axConnect(t, backend)
		if _, err := remote.FindAX("", []AXStep{{Role: "button"}}, stdLimits); err == nil {
			t.Fatal("expected parent-mismatch error")
		}
	})

	t.Run("unexplainable field type", func(t *testing.T) {
		bad := axn("r", "RootWebArea", "", false, "", nil, 0)
		bad["childIds"] = 42
		backend := &axBackend{}
		backend.reply = func(method string, _ map[string]interface{}) (map[string]interface{}, error) {
			return map[string]interface{}{"node": bad}, nil
		}
		remote, _ := axConnect(t, backend)
		if _, err := remote.FindAX("", []AXStep{{Role: "button"}}, stdLimits); err == nil {
			t.Fatal("expected decode error for wrong childIds type")
		}
	})

	t.Run("cross-frame document boundary is not entered", func(t *testing.T) {
		tree := map[string]map[string]interface{}{
			"r":    withFrame(axn("r", "RootWebArea", "", false, "", []string{"sub"}, 0), "MAIN"),
			"sub":  withFrame(axn("sub", "Iframe", "", false, "r", []string{"deep"}, 0), "CHILD"),
			"deep": axn("deep", "button", "Hidden", false, "sub", nil, 9),
		}
		backend := &axBackend{reply: treeResponder("r", tree)}
		remote, _ := axConnect(t, backend)
		matches, err := remote.FindAX("", []AXStep{{Role: "button", Name: axName("Hidden")}}, stdLimits)
		if err != nil {
			t.Fatal(err)
		}
		if len(matches) != 0 {
			t.Fatal("other-frame subtree must not be searchable", matches)
		}
		for _, m := range backend.methods() {
			if m == "Accessibility.getChildAXNodes" {
				if p := lastParams(backend, m); p["id"] == "sub" {
					t.Fatal("must not fetch across the frame boundary")
				}
			}
		}
	})

	t.Run("request budget refuses before an extra request", func(t *testing.T) {
		tree := map[string]map[string]interface{}{
			"r": axn("r", "RootWebArea", "", false, "", []string{"a"}, 0),
			"a": axn("a", "button", "A", false, "r", nil, 1),
		}
		backend := &axBackend{reply: treeResponder("r", tree)}
		remote, _ := axConnect(t, backend)
		if _, err := remote.FindAX("", []AXStep{{Role: "button"}}, AXLimits{MaxNodes: 100, MaxRequests: 1, MaxDepth: 10}); err == nil {
			t.Fatal("expected request-budget error")
		}
		if backend.callCount() != 1 {
			t.Fatal("must not send the request that would exceed the budget, got", backend.callCount())
		}
	})

	t.Run("depth and node budgets", func(t *testing.T) {
		tree := map[string]map[string]interface{}{
			"r": axn("r", "RootWebArea", "", false, "", []string{"a"}, 0),
			"a": axn("a", "group", "", false, "r", []string{"b"}, 0),
			"b": axn("b", "button", "B", false, "a", nil, 1),
		}
		backend := &axBackend{reply: treeResponder("r", tree)}
		remote, _ := axConnect(t, backend)
		if _, err := remote.FindAX("", []AXStep{{Role: "button"}}, AXLimits{MaxNodes: 100, MaxRequests: 100, MaxDepth: 2}); err == nil {
			t.Fatal("expected depth-budget error")
		}

		backend2 := &axBackend{reply: treeResponder("r", tree)}
		remote2, _ := axConnect(t, backend2)
		if _, err := remote2.FindAX("", []AXStep{{Role: "button"}}, AXLimits{MaxNodes: 2, MaxRequests: 100, MaxDepth: 10}); err == nil {
			t.Fatal("expected node-budget error")
		}
	})

	t.Run("invalid arguments fail before any request", func(t *testing.T) {
		backend := &axBackend{reply: treeResponder("r", map[string]map[string]interface{}{})}
		remote, _ := axConnect(t, backend)
		bad := []struct {
			path   []AXStep
			limits AXLimits
		}{
			{nil, stdLimits},
			{[]AXStep{}, stdLimits},
			{[]AXStep{{Role: ""}}, stdLimits},
			{[]AXStep{{Role: "button"}}, AXLimits{0, 1, 1}},
			{[]AXStep{{Role: "button"}}, AXLimits{1, 0, 1}},
			{[]AXStep{{Role: "button"}}, AXLimits{1, 1, 0}},
		}
		for i, tc := range bad {
			if _, err := remote.FindAX("", tc.path, tc.limits); err == nil {
				t.Fatalf("case %d expected error", i)
			}
		}
		if backend.callCount() != 0 {
			t.Fatal("validation must not send requests, got", backend.callCount())
		}
	})

	t.Run("long ignored chain stays bounded", func(t *testing.T) {
		const n = 1200
		tree := map[string]map[string]interface{}{
			"r": axn("r", "RootWebArea", "", false, "", []string{"i0"}, 0),
		}
		for k := 0; k < n; k++ {
			child := fmt.Sprintf("i%d", k+1)
			if k == n-1 {
				child = "leaf"
			}
			tree[fmt.Sprintf("i%d", k)] = axn(fmt.Sprintf("i%d", k), "generic", "", true,
				parentOf(k), []string{child}, 0)
		}
		tree["leaf"] = axn("leaf", "textbox", "Deep", false, fmt.Sprintf("i%d", n-1), nil, 7)

		backend := &axBackend{reply: treeResponder("r", tree)}
		remote, _ := axConnect(t, backend)
		matches, err := remote.FindAX("", []AXStep{{Role: "textbox", Name: axName("Deep")}},
			AXLimits{MaxNodes: n + 10, MaxRequests: n + 10, MaxDepth: n + 10})
		if err != nil {
			t.Fatal(err)
		}
		if len(matches) != 1 || matches[0].AXNodeID != "leaf" || matches[0].BackendNodeID != 7 {
			t.Fatalf("deep lift %+v", matches)
		}
		if len(matches[0].Ancestors) != 2 {
			t.Fatal("ignored chain must be lifted, ancestors", matches[0].Ancestors)
		}
	})

	t.Run("no match returns non-nil empty slice", func(t *testing.T) {
		tree := map[string]map[string]interface{}{
			"r": axn("r", "RootWebArea", "", false, "", []string{"b"}, 0),
			"b": axn("b", "button", "Ok", false, "r", nil, 1),
		}
		backend := &axBackend{reply: treeResponder("r", tree)}
		remote, _ := axConnect(t, backend)
		matches, err := remote.FindAX("", []AXStep{{Role: "textbox"}}, stdLimits)
		if err != nil {
			t.Fatal(err)
		}
		if matches == nil || len(matches) != 0 {
			t.Fatal("expected non-nil empty slice")
		}
	})
}

func parentOf(k int) string {
	if k == 0 {
		return "r"
	}
	return fmt.Sprintf("i%d", k-1)
}

func lastParams(b *axBackend, method string) map[string]interface{} {
	b.mu.Lock()
	defer b.mu.Unlock()
	for i := len(b.calls) - 1; i >= 0; i-- {
		if b.calls[i].Method == method {
			return b.calls[i].Params
		}
	}
	return nil
}

// TestAXProjection validates the exposed-tree projection on a small
// independent tree: ignored lifting across multiple levels, the rule that an
// exposed group cannot be skipped, two same-name forms, empty and wildcard
// names, identity preservation and DFS preorder.
func TestAXProjection(t *testing.T) {
	// r
	//  f1 form(Invoice)
	//    ig ignored
	//      ig2 ignored
	//        g1 group(Billing)
	//          t1 textbox(Amount) backend 101
	//          t3 textbox()        backend 103 (no name object)
	//  f2 form(Invoice)
	//    t2 textbox(Amount) backend 102
	tree := map[string]map[string]interface{}{
		"r":   axn("r", "RootWebArea", "", false, "", []string{"f1", "f2"}, 0),
		"f1":  axn("f1", "form", "Invoice", false, "r", []string{"ig"}, 0),
		"ig":  axn("ig", "generic", "", true, "f1", []string{"ig2"}, 0),
		"ig2": axn("ig2", "generic", "", true, "ig", []string{"g1"}, 0),
		"g1":  axn("g1", "group", "Billing", false, "ig2", []string{"t1", "t3"}, 0),
		"t1":  axn("t1", "textbox", "Amount", false, "g1", nil, 101),
		"t3":  axn("t3", "textbox", "", false, "g1", nil, 103),
		"f2":  axn("f2", "form", "Invoice", false, "r", []string{"t2"}, 0),
		"t2":  axn("t2", "textbox", "Amount", false, "f2", nil, 102),
	}
	backend := &axBackend{reply: treeResponder("r", tree)}
	remote, _ := axConnect(t, backend)
	limits := AXLimits{MaxNodes: 100, MaxRequests: 100, MaxDepth: 20}

	t.Run("full exposed path", func(t *testing.T) {
		m, err := remote.FindAX("", []AXStep{
			{Role: "form", Name: axName("Invoice")},
			{Role: "group", Name: axName("Billing")},
			{Role: "textbox", Name: axName("Amount")},
		}, limits)
		if err != nil {
			t.Fatal(err)
		}
		if len(m) != 1 || m[0].AXNodeID != "t1" {
			t.Fatalf("full path %+v", m)
		}
		// Exposed ancestor chain lifts both ignored nodes: r,f1,g1,t1.
		want := []string{"RootWebArea", "form", "group", "textbox"}
		if len(m[0].Ancestors) != len(want) {
			t.Fatalf("ancestors %+v", m[0].Ancestors)
		}
		for i, role := range want {
			if m[0].Ancestors[i].Role != role {
				t.Fatalf("ancestor %d role %s want %s", i, m[0].Ancestors[i].Role, role)
			}
		}
	})

	t.Run("cannot skip an exposed group", func(t *testing.T) {
		m, err := remote.FindAX("", []AXStep{
			{Role: "form", Name: axName("Invoice")},
			{Role: "textbox", Name: axName("Amount")},
		}, limits)
		if err != nil {
			t.Fatal(err)
		}
		// Only the second form matches; t1 is blocked by its exposed group.
		if len(m) != 1 || m[0].AXNodeID != "t2" {
			t.Fatalf("counterexample %+v", m)
		}
	})

	t.Run("same-name forms keep distinct identities in DFS order", func(t *testing.T) {
		m, err := remote.FindAX("", []AXStep{{Role: "textbox", Name: axName("Amount")}}, limits)
		if err != nil {
			t.Fatal(err)
		}
		if len(m) != 2 {
			t.Fatalf("identity not preserved %+v", m)
		}
		if m[0].AXNodeID != "t1" || m[1].AXNodeID != "t2" {
			t.Fatal("DFS preorder", m[0].AXNodeID, m[1].AXNodeID)
		}
	})

	t.Run("wildcard name and missing name as empty", func(t *testing.T) {
		all, err := remote.FindAX("", []AXStep{{Role: "textbox"}}, limits)
		if err != nil {
			t.Fatal(err)
		}
		if len(all) != 3 {
			t.Fatalf("wildcard %+v", all)
		}
		empty, err := remote.FindAX("", []AXStep{{Role: "textbox", Name: axName("")}}, limits)
		if err != nil {
			t.Fatal(err)
		}
		if len(empty) != 1 || empty[0].AXNodeID != "t3" || empty[0].Name != "" {
			t.Fatalf("empty-name match %+v", empty)
		}
	})

	t.Run("exact name does not fold into empty", func(t *testing.T) {
		m, err := remote.FindAX("", []AXStep{{Role: "group", Name: axName("")}}, limits)
		if err != nil {
			t.Fatal(err)
		}
		if len(m) != 0 {
			t.Fatal("named group must not match empty-name query", m)
		}
	})
}

// TestAXFocus covers the single DOM.focus rule, ambiguity and failure
// handling, the unchanged front-end Focus(nodeID), fresh collection between
// calls and between two independent debuggers.
func TestAXFocus(t *testing.T) {
	stdLimits := AXLimits{MaxNodes: 100, MaxRequests: 100, MaxDepth: 20}

	t.Run("unique match focuses backendNodeId once", func(t *testing.T) {
		tree := map[string]map[string]interface{}{
			"r": axn("r", "RootWebArea", "", false, "", []string{"t"}, 0),
			"t": axn("t", "textbox", "Amount", false, "r", nil, 321),
		}
		backend := &axBackend{reply: treeResponder("r", tree)}
		remote, _ := axConnect(t, backend)

		if err := remote.FocusAX("", []AXStep{{Role: "textbox", Name: axName("Amount")}}, stdLimits); err != nil {
			t.Fatal(err)
		}
		backends := backend.focusBackends()
		if len(backends) != 1 || backends[0] != 321 {
			t.Fatal("focus params", backends)
		}
		last := lastParams(backend, "DOM.focus")
		if _, usedNodeID := last["nodeId"]; usedNodeID {
			t.Fatal("FocusAX must not send nodeId")
		}
		if _, ok := last["backendNodeId"]; !ok {
			t.Fatal("FocusAX must send backendNodeId")
		}
	})

	t.Run("ambiguous match sends no focus", func(t *testing.T) {
		tree := map[string]map[string]interface{}{
			"r": axn("r", "RootWebArea", "", false, "", []string{"a", "b"}, 0),
			"a": axn("a", "textbox", "Amount", false, "r", nil, 1),
			"b": axn("b", "textbox", "Amount", false, "r", nil, 2),
		}
		backend := &axBackend{reply: treeResponder("r", tree)}
		remote, _ := axConnect(t, backend)
		if err := remote.FocusAX("", []AXStep{{Role: "textbox", Name: axName("Amount")}}, stdLimits); err == nil {
			t.Fatal("expected ambiguity error")
		}
		if len(backend.focuses) != 0 {
			t.Fatal("no focus allowed on ambiguity", backend.focuses)
		}
	})

	t.Run("zero match sends no focus", func(t *testing.T) {
		tree := map[string]map[string]interface{}{
			"r": axn("r", "RootWebArea", "", false, "", []string{"b"}, 0),
			"b": axn("b", "button", "Ok", false, "r", nil, 1),
		}
		backend := &axBackend{reply: treeResponder("r", tree)}
		remote, _ := axConnect(t, backend)
		if err := remote.FocusAX("", []AXStep{{Role: "textbox"}}, stdLimits); err == nil {
			t.Fatal("expected no-match error")
		}
		if len(backend.focuses) != 0 {
			t.Fatal("no focus allowed with no match")
		}
	})

	t.Run("missing backend id sends no focus", func(t *testing.T) {
		tree := map[string]map[string]interface{}{
			"r": axn("r", "RootWebArea", "", false, "", []string{"t"}, 0),
			"t": axn("t", "textbox", "Amount", false, "r", nil, 0),
		}
		backend := &axBackend{reply: treeResponder("r", tree)}
		remote, _ := axConnect(t, backend)
		if err := remote.FocusAX("", []AXStep{{Role: "textbox", Name: axName("Amount")}}, stdLimits); err == nil {
			t.Fatal("expected missing-backend error")
		}
		if len(backend.focuses) != 0 {
			t.Fatal("no focus allowed without backend id")
		}
	})

	t.Run("DOM.focus failure is returned and not replayed", func(t *testing.T) {
		tree := map[string]map[string]interface{}{
			"r": axn("r", "RootWebArea", "", false, "", []string{"t"}, 0),
			"t": axn("t", "textbox", "Amount", false, "r", nil, 321),
		}
		backend := &axBackend{}
		backend.reply = func(method string, params map[string]interface{}) (map[string]interface{}, error) {
			if method == "DOM.focus" {
				return nil, fmt.Errorf("node is not focusable")
			}
			return treeResponder("r", tree)(method, params)
		}
		remote, _ := axConnect(t, backend)
		err := remote.FocusAX("", []AXStep{{Role: "textbox", Name: axName("Amount")}}, stdLimits)
		if err == nil {
			t.Fatal("expected DOM.focus error")
		}
		if pe, ok := err.(*ProtocolError); !ok || !strings.Contains(pe.Message, "not focusable") {
			t.Fatal("protocol error must be preserved, got", err)
		}
		if len(backend.focuses) != 1 {
			t.Fatal("failed focus must not be replayed", len(backend.focuses))
		}
	})

	t.Run("original Focus still sends front-end nodeId", func(t *testing.T) {
		backend := &axBackend{reply: func(method string, _ map[string]interface{}) (map[string]interface{}, error) {
			return map[string]interface{}{}, nil
		}}
		remote, _ := axConnect(t, backend)
		if err := remote.Focus(11); err != nil {
			t.Fatal(err)
		}
		last := lastParams(backend, "DOM.focus")
		if last == nil || int(last["nodeId"].(float64)) != 11 {
			t.Fatal("Focus must send nodeId 11", last)
		}
		if _, leak := last["backendNodeId"]; leak {
			t.Fatal("Focus must not send backendNodeId")
		}
	})

	t.Run("tree swapped between calls is recollected", func(t *testing.T) {
		phase := 0
		treeAt := func(backend int) map[string]map[string]interface{} {
			id := fmt.Sprintf("t%d", backend)
			return map[string]map[string]interface{}{
				"r": axn("r", "RootWebArea", "", false, "", []string{id}, 0),
				id:  axn(id, "textbox", "Amount", false, "r", nil, backend),
			}
		}
		backend := &axBackend{}
		backend.reply = func(method string, params map[string]interface{}) (map[string]interface{}, error) {
			return treeResponder("r", treeAt(5+phase))(method, params)
		}
		remote, _ := axConnect(t, backend)
		path := []AXStep{{Role: "textbox", Name: axName("Amount")}}

		if err := remote.FocusAX("", path, stdLimits); err != nil {
			t.Fatal(err)
		}
		phase = 1
		if err := remote.FocusAX("", path, stdLimits); err != nil {
			t.Fatal(err)
		}
		got := backend.focusBackends()
		if len(got) != 2 || got[0] != 5 || got[1] != 6 {
			t.Fatal("each call must collect fresh tree, got", got)
		}
		if backend.callCount() < 4 {
			t.Fatal("expected two full collections, calls", backend.callCount())
		}
	})

	t.Run("two debuggers keep independent collections", func(t *testing.T) {
		mk := func(backend int) (*RemoteDebugger, *axBackend) {
			id := fmt.Sprintf("t%d", backend)
			tree := map[string]map[string]interface{}{
				"r": axn("r", "RootWebArea", "", false, "", []string{id}, 0),
				id:  axn(id, "textbox", "Amount", false, "r", nil, backend),
			}
			b := &axBackend{reply: treeResponder("r", tree)}
			r, _ := axConnect(t, b)
			return r, b
		}
		remote1, b1 := mk(1)
		remote2, b2 := mk(2)
		path := []AXStep{{Role: "textbox", Name: axName("Amount")}}

		m1, err := remote1.FindAX("", path, stdLimits)
		if err != nil {
			t.Fatal(err)
		}
		m2, err := remote2.FindAX("", path, stdLimits)
		if err != nil {
			t.Fatal(err)
		}
		m1again, err := remote1.FindAX("", path, stdLimits)
		if err != nil {
			t.Fatal(err)
		}
		if m1[0].BackendNodeID != 1 || m2[0].BackendNodeID != 2 || m1again[0].BackendNodeID != 1 {
			t.Fatal("cross-debugger leakage", m1, m2, m1again)
		}
		if b1.callCount() != 4 || b2.callCount() != 2 {
			t.Fatal("per-debugger request accounting", b1.callCount(), b2.callCount())
		}
	})
}
