package agouti_test

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"sync"
	"testing"

	"github.com/sclevine/agouti"
	"github.com/sclevine/agouti/api"
)

// -----------------------------------------------------------------------------
// Deterministic in-memory WebDriver fixture.
//
// The fixture speaks the same wire protocol as a real WebDriver: selectors are
// evaluated by the "server" against an element-scoped descendant context.
// -----------------------------------------------------------------------------

type fxNode struct {
	id      string
	tag     string
	classes []string
	parent  string
	text    string
	stale   bool
}

type findRecord struct {
	Root     string
	Selector string
}

type actionRecord struct {
	Session string
	Kind    string
	Element string
	Value   string
}

type fxSession struct {
	nodes    []*fxNode
	byID     map[string]*fxNode
	finds    []findRecord
	actions  []actionRecord
	sharedID string
}

type fixture struct {
	mu       sync.Mutex
	sessions map[string]*fxSession
}

func newFixture() *fixture {
	return &fixture{sessions: map[string]*fxSession{}}
}

func (f *fixture) addSession(id string) *fxSession {
	session := &fxSession{byID: map[string]*fxNode{}}
	f.sessions[id] = session
	return session
}

func (s *fxSession) add(id, tag, classes, parent, text string) *fxNode {
	node := &fxNode{id: id, tag: tag, parent: parent, text: text}
	if classes != "" {
		node.classes = strings.Fields(classes)
	}
	s.nodes = append(s.nodes, node)
	s.byID[id] = node
	return node
}

func (s *fxSession) hasClass(node *fxNode, class string) bool {
	for _, candidate := range node.classes {
		if candidate == class {
			return true
		}
	}
	return false
}

func matchCompound(compound string, node *fxNode, session *fxSession) bool {
	i := 0
	for i < len(compound) && compound[i] != '.' && compound[i] != '#' {
		i++
	}
	if i > 0 && compound[:i] != "*" && node.tag != compound[:i] {
		return false
	}
	for i < len(compound) {
		switch compound[i] {
		case '.':
			j := i + 1
			for j < len(compound) && compound[j] != '.' && compound[j] != '#' {
				j++
			}
			if !session.hasClass(node, compound[i+1:j]) {
				return false
			}
			i = j
		case '#':
			j := i + 1
			for j < len(compound) && compound[j] != '.' && compound[j] != '#' {
				j++
			}
			if node.id != compound[i+1:j] {
				return false
			}
			i = j
		default:
			return false
		}
	}
	return true
}

func (s *fxSession) matchFrom(compounds []string, index int, node *fxNode) bool {
	if !matchCompound(compounds[index], node, s) {
		return false
	}
	if index == 0 {
		return true
	}
	for parent := node.parent; parent != ""; parent = s.byID[parent].parent {
		if s.matchFrom(compounds, index-1, s.byID[parent]) {
			return true
		}
	}
	return false
}

func (s *fxSession) isDescendant(node *fxNode, root string) bool {
	if root == "" {
		return true
	}
	for parent := node.parent; parent != ""; parent = s.byID[parent].parent {
		if parent == root {
			return true
		}
	}
	return false
}

func (s *fxSession) find(root, selector string) ([]*fxNode, error) {
	if strings.Contains(selector, "invalid") {
		return nil, fmt.Errorf("invalid selector")
	}
	if root != "" {
		context, ok := s.byID[root]
		if !ok {
			return nil, fmt.Errorf("no such element")
		}
		if context.stale {
			return nil, fmt.Errorf("stale element reference")
		}
	}

	if selector == "shared" {
		shared, ok := s.byID[s.sharedID]
		if !ok || !s.isDescendant(shared, root) {
			return []*fxNode{}, nil
		}
		return []*fxNode{shared}, nil
	}

	compounds := strings.Fields(selector)
	if len(compounds) == 0 {
		return nil, fmt.Errorf("invalid selector")
	}

	var matches []*fxNode
	for _, node := range s.nodes {
		if !s.isDescendant(node, root) {
			continue
		}
		if s.matchFrom(compounds, len(compounds)-1, node) {
			matches = append(matches, node)
		}
	}
	return matches, nil
}

func jsonResponse(value interface{}) *http.Response {
	body, _ := json.Marshal(map[string]interface{}{"value": value})
	return &http.Response{
		StatusCode: 200,
		Header:     http.Header{"Content-Type": []string{"application/json"}},
		Body:       io.NopCloser(strings.NewReader(string(body))),
	}
}

func errorResponse(message string) *http.Response {
	inner, _ := json.Marshal(map[string]string{"errorMessage": message})
	body, _ := json.Marshal(map[string]interface{}{
		"status": 500,
		"value":  map[string]string{"message": string(inner)},
	})
	return &http.Response{
		StatusCode: 500,
		Header:     http.Header{"Content-Type": []string{"application/json"}},
		Body:       io.NopCloser(strings.NewReader(string(body))),
	}
}

func decodeBody(request *http.Request) (map[string]interface{}, error) {
	body := map[string]interface{}{}
	if request.Body == nil {
		return body, nil
	}
	if err := json.NewDecoder(request.Body).Decode(&body); err != nil && err != io.EOF {
		return nil, err
	}
	return body, nil
}

func (f *fixture) RoundTrip(request *http.Request) (*http.Response, error) {
	f.mu.Lock()
	defer f.mu.Unlock()

	parts := strings.Split(strings.Trim(request.URL.Path, "/"), "/")
	if len(parts) < 3 || parts[0] != "session" {
		return errorResponse("unknown command"), nil
	}
	sessionID := parts[1]
	session, ok := f.sessions[sessionID]
	if !ok {
		return errorResponse("unknown session"), nil
	}
	rest := parts[2:]

	body, err := decodeBody(request)
	if err != nil {
		return errorResponse("malformed request body"), nil
	}

	selectorValue, _ := body["value"].(string)
	using, _ := body["using"].(string)

	find := func(root string) (*http.Response, error) {
		selector := selectorValue
		switch using {
		case "css selector":
		case "xpath":
			selector = strings.TrimPrefix(selectorValue, ".//")
		default:
			return errorResponse("unsupported selector type"), nil
		}
		session.finds = append(session.finds, findRecord{Root: root, Selector: selectorValue})
		nodes, err := session.find(root, selector)
		if err != nil {
			return errorResponse(err.Error()), nil
		}
		results := []map[string]string{}
		for _, node := range nodes {
			results = append(results, map[string]string{"ELEMENT": node.id})
		}
		return jsonResponse(results), nil
	}

	if len(rest) == 1 && rest[0] == "elements" && request.Method == "POST" {
		return find("")
	}
	if len(rest) == 1 && rest[0] == "element" && request.Method == "POST" {
		if using != "css selector" {
			return errorResponse("unsupported selector type"), nil
		}
		session.finds = append(session.finds, findRecord{Root: "", Selector: selectorValue})
		nodes, err := session.find("", selectorValue)
		if err != nil {
			return errorResponse(err.Error()), nil
		}
		if len(nodes) == 0 {
			return errorResponse("no such element"), nil
		}
		return jsonResponse(map[string]string{"ELEMENT": nodes[0].id}), nil
	}

	if len(rest) >= 2 && rest[0] == "element" {
		elementID := rest[1]
		action := ""
		if len(rest) >= 3 {
			action = rest[2]
		}
		node, ok := session.byID[elementID]
		if !ok {
			return errorResponse("no such element"), nil
		}

		record := func(kind, value string) {
			session.actions = append(session.actions, actionRecord{
				Session: sessionID,
				Kind:    kind,
				Element: elementID,
				Value:   value,
			})
		}

		switch {
		case action == "elements" && request.Method == "POST":
			if node.stale {
				return errorResponse("stale element reference"), nil
			}
			return find(elementID)
		case action == "element" && request.Method == "POST":
			if node.stale {
				return errorResponse("stale element reference"), nil
			}
			singularSelector := selectorValue
			if using == "xpath" {
				singularSelector = strings.TrimPrefix(singularSelector, ".//")
			} else if using != "css selector" {
				return errorResponse("unsupported selector type"), nil
			}
			session.finds = append(session.finds, findRecord{Root: elementID, Selector: selectorValue})
			nodes, err := session.find(elementID, singularSelector)
			if err != nil {
				return errorResponse(err.Error()), nil
			}
			if len(nodes) == 0 {
				return errorResponse("no such element"), nil
			}
			return jsonResponse(map[string]string{"ELEMENT": nodes[0].id}), nil
		case action == "clear" && request.Method == "POST":
			if node.stale {
				return errorResponse("stale element reference"), nil
			}
			record("clear", "")
			return jsonResponse(nil), nil
		case action == "value" && request.Method == "POST":
			if node.stale {
				return errorResponse("stale element reference"), nil
			}
			letters, _ := body["value"].([]interface{})
			var text strings.Builder
			for _, letter := range letters {
				text.WriteString(fmt.Sprint(letter))
			}
			record("value", text.String())
			return jsonResponse(nil), nil
		case action == "click" && request.Method == "POST":
			if node.stale {
				return errorResponse("stale element reference"), nil
			}
			record("click", "")
			return jsonResponse(nil), nil
		case action == "text" && request.Method == "GET":
			if node.stale {
				return errorResponse("stale element reference"), nil
			}
			return jsonResponse(node.text), nil
		}
	}

	return errorResponse(fmt.Sprintf("unexpected command %s %s", request.Method, request.URL.Path)), nil
}

func (f *fixture) page(t *testing.T, sessionID string) *agouti.Page {
	t.Helper()
	f.mu.Lock()
	if _, ok := f.sessions[sessionID]; !ok {
		f.mu.Unlock()
		t.Fatalf("unknown fixture session %q", sessionID)
	}
	f.mu.Unlock()
	client := &http.Client{Transport: f}
	return agouti.JoinPage("http://fixture/session/"+sessionID, agouti.HTTPClient(client))
}

func (f *fixture) finds(sessionID string) []findRecord {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]findRecord(nil), f.sessions[sessionID].finds...)
}

func (f *fixture) actions(sessionID string) []actionRecord {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]actionRecord(nil), f.sessions[sessionID].actions...)
}

func (f *fixture) reset(sessionID string) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.sessions[sessionID].finds = nil
	f.sessions[sessionID].actions = nil
}

func (f *fixture) setClasses(sessionID, elementID, classes string) {
	f.mu.Lock()
	defer f.mu.Unlock()
	node := f.sessions[sessionID].byID[elementID]
	node.classes = strings.Fields(classes)
}

func (f *fixture) markStale(sessionID, elementID string) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.sessions[sessionID].byID[elementID].stale = true
}

// buildDOM constructs an order-sensitive document. IDs are prefixed so that
// two sessions never share element handles.
func buildDOM(session *fxSession, prefix string) {
	id := func(name string) string { return prefix + name }

	session.add(id("opts"), "div", "", "", "")
	session.add(id("iA"), "input", "primary fallback", id("opts"), "")
	session.add(id("iB"), "input", "primary", id("opts"), "")
	session.add(id("iC"), "input", "fallback", id("opts"), "")
	session.add(id("iD"), "input", "primary", id("opts"), "")

	session.add(id("A"), "section", "group", "", "")
	session.add(id("A1"), "tr", "", id("A"), "")
	session.add(id("A1td"), "td", "", id("A1"), "")
	session.add(id("A1b"), "button", "pending", id("A1td"), "one")
	session.add(id("A1input"), "input", "primary", id("A1"), "")
	session.add(id("A2"), "tr", "", id("A"), "")
	session.add(id("A2td"), "td", "", id("A2"), "")
	session.add(id("A2b"), "button", "pending", id("A2td"), "two")
	session.add(id("A2input"), "input", "fallback", id("A2"), "")
	session.add(id("A3"), "tr", "", id("A"), "")
	session.add(id("A3td"), "td", "", id("A3"), "")
	session.add(id("A3b"), "button", "pending", id("A3td"), "three")
	session.add(id("A3err"), "div", "error", id("A3"), "")

	session.add(id("B"), "section", "group", "", "")
	session.add(id("B1"), "tr", "", id("B"), "")
	session.add(id("B1td"), "td", "", id("B1"), "")
	session.add(id("B1b"), "button", "pending", id("B1td"), "four")
	session.add(id("B1input"), "input", "primary", id("B1"), "")
	session.add(id("B2"), "tr", "", id("B"), "")
	session.add(id("B2x"), "span", "", id("B2"), "")
	session.add(id("B3"), "tr", "", id("B"), "")
	session.add(id("B3td"), "td", "", id("B3"), "")
	session.add(id("B3b"), "button", "pending", id("B3td"), "five")
	session.add(id("B3input"), "input", "fallback", id("B3"), "")

	session.add(id("C"), "section", "", "", "")
	session.add(id("C1"), "tr", "", id("C"), "")
	session.add(id("C1td"), "td", "", id("C1"), "")
	session.add(id("C1b"), "button", "pending", id("C1td"), "six")
	session.add(id("C1err"), "div", "error", id("C1"), "")
	session.add(id("C2"), "tr", "", id("C"), "")
	session.add(id("C2x"), "span", "", id("C2"), "")

	session.sharedID = id("A1b")
}

func newBuiltFixture(t *testing.T) *fixture {
	t.Helper()
	f := newFixture()
	buildDOM(f.addSession("s1"), "")
	return f
}

func elementIDs(elements []*api.Element) []string {
	ids := make([]string, 0, len(elements))
	for _, element := range elements {
		ids = append(ids, element.ID)
	}
	return ids
}

func equalStrings(a, b []string) bool {
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

func findStrings(records []findRecord) []string {
	out := make([]string, 0, len(records))
	for _, record := range records {
		root := record.Root
		if root == "" {
			root = "<document>"
		}
		out = append(out, root+"/"+record.Selector)
	}
	return out
}

func actionStrings(records []actionRecord) []string {
	out := make([]string, 0, len(records))
	for _, record := range records {
		out = append(out, fmt.Sprintf("%s %s %s %q", record.Session, record.Kind, record.Element, record.Value))
	}
	return out
}

// -----------------------------------------------------------------------------
// TestRelationAlgebra
// -----------------------------------------------------------------------------

func TestRelationAlgebra(t *testing.T) {
	type algebraCase struct {
		name    string
		run     func(page *agouti.Page) ([]*api.Element, error)
		want    []string
		wantErr string
	}

	cases := []algebraCase{
		{
			name: "rows filtered in two sections",
			run: func(page *agouti.Page) ([]*api.Element, error) {
				return page.All("section.group").Query(
					agouti.QueryCSS("tr").
						Has(agouti.QueryCSS("button.pending")).
						HasNot(agouti.QueryCSS(".error")),
				).Elements()
			},
			want: []string{"A1", "A2", "B1", "B3"},
		},
		{
			name: "partially empty section contributes nothing",
			run: func(page *agouti.Page) ([]*api.Element, error) {
				return page.All("section").Query(
					agouti.QueryCSS("tr").
						Has(agouti.QueryCSS("button.pending")).
						HasNot(agouti.QueryCSS(".error")),
				).Elements()
			},
			want: []string{"A1", "A2", "B1", "B3"},
		},
		{
			name: "filter then per-parent at",
			run: func(page *agouti.Page) ([]*api.Element, error) {
				return page.All("section.group").Query(
					agouti.QueryCSS("tr").
						Has(agouti.QueryCSS("button.pending")).
						HasNot(agouti.QueryCSS(".error")).
						At(1),
				).Elements()
			},
			want: []string{"A2", "B3"},
		},
		{
			name: "per-parent at never borrows from another section",
			run: func(page *agouti.Page) ([]*api.Element, error) {
				return page.All("section").Query(
					agouti.QueryCSS("tr").
						Has(agouti.QueryCSS("button.pending")).
						HasNot(agouti.QueryCSS(".error")).
						At(1),
				).Elements()
			},
			wantErr: "out of range",
		},
		{
			name: "or keeps left order then unseen right",
			run: func(page *agouti.Page) ([]*api.Element, error) {
				return page.Query(
					agouti.QueryCSS("#opts input.primary").
						Or(agouti.QueryCSS("#opts input.fallback")),
				).Elements()
			},
			want: []string{"iA", "iB", "iD", "iC"},
		},
		{
			name: "or at applies after merging both sides",
			run: func(page *agouti.Page) ([]*api.Element, error) {
				return page.Query(
					agouti.QueryCSS("#opts input.primary").
						Or(agouti.QueryCSS("#opts input.fallback")).
						At(0),
				).Elements()
			},
			want: []string{"iA"},
		},
		{
			name: "or de-duplicates by remote id in the same context",
			run: func(page *agouti.Page) ([]*api.Element, error) {
				return page.Query(
					agouti.QueryCSS("shared").Or(agouti.QueryCSS("shared")),
				).Elements()
			},
			want: []string{"A1b"},
		},
		{
			name: "then concatenates without cross-parent de-duplication",
			run: func(page *agouti.Page) ([]*api.Element, error) {
				return page.Query(
					agouti.QueryCSS("section.group").
						Or(agouti.QueryCSS("tr")).
						Then(agouti.QueryCSS("td")),
				).Elements()
			},
			want: []string{
				"A1td", "A2td", "A3td", "B1td", "B3td",
				"A1td", "A2td", "A3td", "B1td", "B3td", "C1td",
			},
		},
		{
			name: "then preserves left element order",
			run: func(page *agouti.Page) ([]*api.Element, error) {
				return page.Query(
					agouti.QueryCSS("section.group").Then(agouti.QueryCSS("tr")),
				).Elements()
			},
			want: []string{"A1", "A2", "A3", "B1", "B2", "B3"},
		},
		{
			name: "nested relational has with then and or",
			run: func(page *agouti.Page) ([]*api.Element, error) {
				return page.All("section").Query(
					agouti.QueryCSS("tr").
						Has(agouti.QueryCSS("td").Then(agouti.QueryCSS("button.pending"))).
						Has(agouti.QueryCSS("input.primary").Or(agouti.QueryCSS("input.fallback"))),
				).Elements()
			},
			want: []string{"A1", "A2", "B1", "B3"},
		},
		{
			name: "nested has of filtered rows",
			run: func(page *agouti.Page) ([]*api.Element, error) {
				return page.Query(
					agouti.QueryCSS("section").Has(
						agouti.QueryCSS("tr").
							Has(agouti.QueryCSS("button.pending")).
							HasNot(agouti.QueryCSS(".error")),
					),
				).Elements()
			},
			want: []string{"A", "B"},
		},
		{
			name: "at out of range on a complete expression",
			run: func(page *agouti.Page) ([]*api.Element, error) {
				return page.Query(agouti.QueryCSS("#opts input.primary").At(9)).Elements()
			},
			wantErr: "out of range",
		},
		{
			name: "negative at is invalid",
			run: func(page *agouti.Page) ([]*api.Element, error) {
				return page.Query(agouti.QueryCSS("tr").At(-1)).Elements()
			},
			wantErr: "negative",
		},
		{
			name: "zero query is invalid",
			run: func(page *agouti.Page) ([]*api.Element, error) {
				return page.Query(agouti.Query{}).Elements()
			},
			wantErr: "zero-value query",
		},
		{
			name: "empty css is invalid",
			run: func(page *agouti.Page) ([]*api.Element, error) {
				return page.Query(agouti.QueryCSS("")).Elements()
			},
			wantErr: "must not be empty",
		},
		{
			name: "invalid nested sub-expression is rejected before evaluation",
			run: func(page *agouti.Page) ([]*api.Element, error) {
				return page.Query(
					agouti.QueryCSS("tr").Has(
						agouti.QueryCSS("button").Or(agouti.QueryCSS("")),
					),
				).Elements()
			},
			wantErr: "must not be empty",
		},
		{
			name: "remote invalid selector is reported",
			run: func(page *agouti.Page) ([]*api.Element, error) {
				return page.Query(agouti.QueryCSS("invalid[")).Elements()
			},
			wantErr: "invalid selector",
		},
		{
			name: "hasnot does not swallow a remote invalid selector",
			run: func(page *agouti.Page) ([]*api.Element, error) {
				return page.All("section").Query(
					agouti.QueryCSS("tr").HasNot(agouti.QueryCSS("invalid[")),
				).Elements()
			},
			wantErr: "invalid selector",
		},
		{
			name: "hasnot does not swallow a stale element error",
			run: func(page *agouti.Page) ([]*api.Element, error) {
				return page.Query(
					agouti.QueryCSS("tr").HasNot(agouti.QueryCSS("button.pending")),
				).Elements()
			},
			wantErr: "stale element reference",
		},
	}

	for _, testCase := range cases {
		testCase := testCase
		t.Run(testCase.name, func(t *testing.T) {
			f := newBuiltFixture(t)
			page := f.page(t, "s1")
			if testCase.name == "hasnot does not swallow a stale element error" {
				f.markStale("s1", "A1")
			}
			f.reset("s1")

			elements, err := testCase.run(page)
			if testCase.wantErr != "" {
				if err == nil {
					t.Fatalf("expected error containing %q, got elements %v", testCase.wantErr, elementIDs(elements))
				}
				if !strings.Contains(err.Error(), testCase.wantErr) {
					t.Fatalf("expected error containing %q, got %q", testCase.wantErr, err)
				}
				if testCase.name == "negative at is invalid" ||
					testCase.name == "zero query is invalid" ||
					testCase.name == "empty css is invalid" ||
					testCase.name == "invalid nested sub-expression is rejected before evaluation" {
					if finds := f.finds("s1"); len(finds) != 0 {
						t.Fatalf("invalid expression sent remote queries before validation: %v", findStrings(finds))
					}
				}
				return
			}
			if err != nil {
				t.Fatalf("unexpected error: %v", err)
			}
			if got := elementIDs(elements); !equalStrings(got, testCase.want) {
				t.Fatalf("wrong relation result: got %v want %v", got, testCase.want)
			}
		})
	}
}

// -----------------------------------------------------------------------------
// TestRelationActions
// -----------------------------------------------------------------------------

func TestRelationActions(t *testing.T) {
	t.Run("fill resolves the complete expression before any action", func(t *testing.T) {
		f := newBuiltFixture(t)
		page := f.page(t, "s1")
		selection := page.All("section").Query(agouti.QueryCSS("tr").At(2))
		f.reset("s1")

		err := selection.Fill("x")
		if err == nil || !strings.Contains(err.Error(), "out of range") {
			t.Fatalf("expected resolution error, got %v", err)
		}
		if actions := f.actions("s1"); len(actions) != 0 {
			t.Fatalf("no element may receive an action when a parent branch fails: %v", actionStrings(actions))
		}
	})

	t.Run("fill resolves once and reuses the snapshot", func(t *testing.T) {
		f := newBuiltFixture(t)
		page := f.page(t, "s1")
		selection := page.All("section.group").Query(agouti.QueryCSS("input.primary"))
		f.reset("s1")

		if err := selection.Fill("ok"); err != nil {
			t.Fatal(err)
		}

		wantFinds := []string{
			"<document>/section.group",
			"A/input.primary",
			"B/input.primary",
		}
		if got := findStrings(f.finds("s1")); !equalStrings(got, wantFinds) {
			t.Fatalf("wrong resolution requests: got %v want %v", got, wantFinds)
		}

		wantActions := []string{
			`s1 clear A1input ""`,
			`s1 value A1input "ok"`,
			`s1 clear B1input ""`,
			`s1 value B1input "ok"`,
		}
		if got := actionStrings(f.actions("s1")); !equalStrings(got, wantActions) {
			t.Fatalf("wrong action sequence: got %v want %v", got, wantActions)
		}
	})

	t.Run("terminal operation re-queries replaced server nodes", func(t *testing.T) {
		f := newBuiltFixture(t)
		page := f.page(t, "s1")
		selection := page.All("section.group").Query(agouti.QueryCSS("input.primary"))

		firstHandles, err := selection.Elements()
		if err != nil {
			t.Fatal(err)
		}
		if got := elementIDs(firstHandles); !equalStrings(got, []string{"A1input", "B1input"}) {
			t.Fatalf("wrong initial handles: %v", got)
		}

		f.setClasses("s1", "A1input", "")
		f.setClasses("s1", "A2input", "primary")
		f.setClasses("s1", "B1input", "")
		f.setClasses("s1", "B3input", "primary")

		secondHandles, err := selection.Elements()
		if err != nil {
			t.Fatal(err)
		}
		if got := elementIDs(secondHandles); !equalStrings(got, []string{"A2input", "B3input"}) {
			t.Fatalf("selection did not re-resolve replaced nodes: %v", got)
		}
		if got := elementIDs(firstHandles); !equalStrings(got, []string{"A1input", "B1input"}) {
			t.Fatalf("previously returned handles were mutated: %v", got)
		}
		if firstHandles[0].Session != secondHandles[0].Session {
			t.Fatalf("handles from the same page must share the session")
		}

		f.reset("s1")
		if err := selection.Fill("new"); err != nil {
			t.Fatal(err)
		}
		wantActions := []string{
			`s1 clear A2input ""`,
			`s1 value A2input "new"`,
			`s1 clear B3input ""`,
			`s1 value B3input "new"`,
		}
		if got := actionStrings(f.actions("s1")); !equalStrings(got, wantActions) {
			t.Fatalf("wrong replacement action sequence: got %v want %v", got, wantActions)
		}
	})

	t.Run("click and text use query results", func(t *testing.T) {
		f := newBuiltFixture(t)
		page := f.page(t, "s1")

		if text, err := page.Query(agouti.QueryCSS("button.pending").At(0)).Text(); err != nil || text != "one" {
			t.Fatalf("wrong text result %q, %v", text, err)
		}

		f.reset("s1")
		selection := page.All("section.group").Query(agouti.QueryCSS("button.pending").At(0))
		if err := selection.Click(); err != nil {
			t.Fatal(err)
		}
		wantActions := []string{
			`s1 click A1b ""`,
			`s1 click B1b ""`,
		}
		if got := actionStrings(f.actions("s1")); !equalStrings(got, wantActions) {
			t.Fatalf("wrong click actions: got %v want %v", got, wantActions)
		}
	})

	t.Run("two interleaved sessions do not cross the wire", func(t *testing.T) {
		f := newFixture()
		buildDOM(f.addSession("s1"), "")
		buildDOM(f.addSession("s2"), "Z")
		firstPage := f.page(t, "s1")
		secondPage := f.page(t, "s2")

		query := agouti.QueryCSS("input.primary")
		firstSelection := firstPage.All("section.group").Query(query)
		secondSelection := secondPage.All("section.group").Query(query)

		firstCount, err := firstSelection.Count()
		if err != nil {
			t.Fatal(err)
		}
		secondCount, err := secondSelection.Count()
		if err != nil {
			t.Fatal(err)
		}
		firstAgain, err := firstSelection.Count()
		if err != nil {
			t.Fatal(err)
		}
		if firstCount != 2 || secondCount != 2 || firstAgain != 2 {
			t.Fatalf("unexpected interleaved counts: %d %d %d", firstCount, secondCount, firstAgain)
		}

		f.reset("s1")
		f.reset("s2")
		if err := firstSelection.Fill("first"); err != nil {
			t.Fatal(err)
		}
		if err := secondSelection.Fill("second"); err != nil {
			t.Fatal(err)
		}

		firstActions := f.actions("s1")
		secondActions := f.actions("s2")
		for _, action := range firstActions {
			if action.Session != "s1" || strings.HasPrefix(action.Element, "Z") {
				t.Fatalf("first session leaked into second session: %v", actionStrings(firstActions))
			}
		}
		for _, action := range secondActions {
			if action.Session != "s2" || !strings.HasPrefix(action.Element, "Z") {
				t.Fatalf("second session action has wrong identity: %v", actionStrings(secondActions))
			}
		}
		wantFirst := []string{
			`s1 clear A1input ""`,
			`s1 value A1input "first"`,
			`s1 clear B1input ""`,
			`s1 value B1input "first"`,
		}
		if got := actionStrings(firstActions); !equalStrings(got, wantFirst) {
			t.Fatalf("wrong first-session actions: %v", got)
		}
		wantSecond := []string{
			`s2 clear ZA1input ""`,
			`s2 value ZA1input "second"`,
			`s2 clear ZB1input ""`,
			`s2 value ZB1input "second"`,
		}
		if got := actionStrings(secondActions); !equalStrings(got, wantSecond) {
			t.Fatalf("wrong second-session actions: %v", got)
		}
	})

	t.Run("query construction and reuse never touches the server", func(t *testing.T) {
		f := newBuiltFixture(t)
		page := f.page(t, "s1")
		f.reset("s1")

		base := agouti.QueryCSS("#opts input.primary")
		original := base.String()
		combined := base.Or(agouti.QueryCSS("#opts input.fallback"))
		filtered := base.HasNot(agouti.QueryCSS(".fallback"))
		nested := agouti.QueryCSS("section").Has(base)
		_ = combined.String()
		_ = filtered.String()
		_ = nested.String()

		if finds := f.finds("s1"); len(finds) != 0 {
			t.Fatalf("query construction contacted the server: %v", findStrings(finds))
		}
		if base.String() != original {
			t.Fatalf("query construction mutated an existing expression: %q != %q", base.String(), original)
		}

		baseCount, err := page.Query(base).Count()
		if err != nil {
			t.Fatal(err)
		}
		combinedCount, err := page.Query(combined).Count()
		if err != nil {
			t.Fatal(err)
		}
		filteredCount, err := page.Query(filtered).Count()
		if err != nil {
			t.Fatal(err)
		}
		if baseCount != 3 || combinedCount != 4 || filteredCount != 3 {
			t.Fatalf("unexpected counts: base=%d combined=%d filtered=%d", baseCount, combinedCount, filteredCount)
		}
	})
}

// -----------------------------------------------------------------------------
// TestRelationCompatibility
// -----------------------------------------------------------------------------

func TestRelationCompatibility(t *testing.T) {
	t.Run("query can be followed by the legacy all chain", func(t *testing.T) {
		f := newBuiltFixture(t)
		page := f.page(t, "s1")
		f.reset("s1")

		all := page.Query(agouti.QueryCSS("section.group")).All("tr")
		elements, err := all.Elements()
		if err != nil {
			t.Fatal(err)
		}
		if got := elementIDs(elements); !equalStrings(got, []string{"A1", "A2", "A3", "B1", "B2", "B3"}) {
			t.Fatalf("wrong query-then-all result: %v", got)
		}
		f.reset("s1")
		if count, err := all.Count(); err != nil || count != 6 {
			t.Fatalf("wrong query-then-all count %d, %v", count, err)
		}
		want := []string{
			"<document>/section.group",
			"A/tr",
			"B/tr",
		}
		if got := findStrings(f.finds("s1")); !equalStrings(got, want) {
			t.Fatalf("all was not rooted at the query result: got %v want %v", got, want)
		}
		for _, record := range f.finds("s1") {
			if record.Selector == "section.group tr" {
				t.Fatalf("query and following all were incorrectly merged")
			}
		}
	})

	t.Run("selection at after query applies per parent context", func(t *testing.T) {
		f := newBuiltFixture(t)
		page := f.page(t, "s1")
		f.reset("s1")

		elements, err := page.Query(agouti.QueryCSS("section.group").Then(agouti.QueryCSS("tr"))).At(1).Elements()
		if err != nil {
			t.Fatal(err)
		}
		if got := elementIDs(elements); !equalStrings(got, []string{"A2"}) {
			t.Fatalf("wrong selection at result: %v", got)
		}
		want := []string{
			"<document>/section.group",
			"A/tr",
			"B/tr",
		}
		if got := findStrings(f.finds("s1")); !equalStrings(got, want) {
			t.Fatalf("selection at re-queried incorrectly: got %v want %v", got, want)
		}
	})

	t.Run("legacy css chain still merges into one request", func(t *testing.T) {
		f := newBuiltFixture(t)
		page := f.page(t, "s1")
		f.reset("s1")

		count, err := page.All("section").All("tr").Count()
		if err != nil {
			t.Fatal(err)
		}
		if count != 8 {
			t.Fatalf("wrong legacy count: %d", count)
		}
		want := []string{"<document>/section tr"}
		if got := findStrings(f.finds("s1")); !equalStrings(got, want) {
			t.Fatalf("legacy CSS selection was not merged: got %v want %v", got, want)
		}
	})

	t.Run("query is a barrier and later all is rooted at query results", func(t *testing.T) {
		f := newBuiltFixture(t)
		page := f.page(t, "s1")
		f.reset("s1")

		count, err := page.All("section").Query(agouti.QueryCSS("tr")).All("td").Count()
		if err != nil {
			t.Fatal(err)
		}
		if count != 6 {
			t.Fatalf("wrong count: %d", count)
		}
		want := []string{
			"<document>/section",
			"A/tr",
			"B/tr",
			"C/tr",
			"A1/td",
			"A2/td",
			"A3/td",
			"B1/td",
			"B2/td",
			"B3/td",
			"C1/td",
			"C2/td",
		}
		got := findStrings(f.finds("s1"))
		if !equalStrings(got, want) {
			t.Fatalf("query barrier was crossed: got %v want %v", got, want)
		}
		for _, record := range f.finds("s1") {
			if strings.Contains(record.Selector, "section tr") || strings.Contains(record.Selector, "tr td") {
				t.Fatalf("selector was merged across a query barrier: %v", findStrings(f.finds("s1")))
			}
		}
	})

	t.Run("has is a barrier and is evaluated at each row", func(t *testing.T) {
		f := newBuiltFixture(t)
		page := f.page(t, "s1")
		f.reset("s1")

		count, err := page.All("section.group").Query(
			agouti.QueryCSS("tr").Has(agouti.QueryCSS("button.pending")),
		).Count()
		if err != nil {
			t.Fatal(err)
		}
		if count != 5 {
			t.Fatalf("wrong filtered count: %d", count)
		}
		want := []string{
			"<document>/section.group",
			"A/tr",
			"A1/button.pending",
			"A2/button.pending",
			"A3/button.pending",
			"B/tr",
			"B1/button.pending",
			"B2/button.pending",
			"B3/button.pending",
		}
		if got := findStrings(f.finds("s1")); !equalStrings(got, want) {
			t.Fatalf("has barrier was crossed: got %v want %v", got, want)
		}
		for _, record := range f.finds("s1") {
			if record.Selector == "tr button.pending" {
				t.Fatalf("has condition was merged into the left selector")
			}
			if record.Selector == "button.pending" && record.Root == "" {
				t.Fatalf("has condition was evaluated from the page root instead of the row")
			}
		}
	})

	t.Run("or evaluates both sides in the identical input context", func(t *testing.T) {
		f := newBuiltFixture(t)
		page := f.page(t, "s1")
		f.reset("s1")

		count, err := page.Query(
			agouti.QueryCSS("#opts input.primary").
				Or(agouti.QueryCSS("#opts input.fallback")),
		).Count()
		if err != nil {
			t.Fatal(err)
		}
		if count != 4 {
			t.Fatalf("wrong or count: %d", count)
		}
		want := []string{
			"<document>/#opts input.primary",
			"<document>/#opts input.fallback",
		}
		if got := findStrings(f.finds("s1")); !equalStrings(got, want) {
			t.Fatalf("or sides did not share the input context: got %v want %v", got, want)
		}
	})

	t.Run("at is a barrier and stays attached to the complete expression", func(t *testing.T) {
		f := newBuiltFixture(t)
		page := f.page(t, "s1")
		f.reset("s1")

		count, err := page.All("section.group").Query(
			agouti.QueryCSS("tr").At(1),
		).All("td").Count()
		if err != nil {
			t.Fatal(err)
		}
		if count != 1 {
			t.Fatalf("wrong count: %d", count)
		}
		want := []string{
			"<document>/section.group",
			"A/tr",
			"B/tr",
			"A2/td",
			"B2/td",
		}
		if got := findStrings(f.finds("s1")); !equalStrings(got, want) {
			t.Fatalf("at barrier was crossed: got %v want %v", got, want)
		}
	})

	t.Run("all after a filtered query is not attached to the filter", func(t *testing.T) {
		f := newBuiltFixture(t)
		page := f.page(t, "s1")
		f.reset("s1")

		count, err := page.All("section").Query(
			agouti.QueryCSS("tr").Has(agouti.QueryCSS("button.pending")),
		).All("td").Count()
		if err != nil {
			t.Fatal(err)
		}
		if count != 6 {
			t.Fatalf("wrong count: %d", count)
		}
		rowIDs := map[string]bool{"A1": true, "A2": true, "A3": true, "B1": true, "B3": true, "C1": true}
		for _, record := range f.finds("s1") {
			if record.Selector == "td" && !rowIDs[record.Root] {
				t.Fatalf("td query was not rooted at a filtered row: %v", findStrings(f.finds("s1")))
			}
			if strings.Contains(record.Selector, "tr td") {
				t.Fatalf("all after query was merged into the filter: %v", findStrings(f.finds("s1")))
			}
		}
	})

	t.Run("legacy per-parent at is unchanged", func(t *testing.T) {
		f := newBuiltFixture(t)
		page := f.page(t, "s1")
		f.reset("s1")

		elements, err := page.All("section.group").AllByXPath(".//tr").At(1).Elements()
		if err != nil {
			t.Fatal(err)
		}
		if got := elementIDs(elements); !equalStrings(got, []string{"A2", "B2"}) {
			t.Fatalf("wrong legacy per-parent at: %v", got)
		}
		want := []string{
			"<document>/section.group",
			"A/.//tr",
			"B/.//tr",
		}
		if got := findStrings(f.finds("s1")); !equalStrings(got, want) {
			t.Fatalf("wrong legacy requests: got %v want %v", got, want)
		}
	})

	t.Run("quantity constraints still apply to query selections", func(t *testing.T) {
		f := newBuiltFixture(t)
		page := f.page(t, "s1")

		empty := page.Query(agouti.QueryCSS("span.nomatch"))
		if count, err := empty.Count(); err != nil || count != 0 {
			t.Fatalf("wrong empty count %d, %v", count, err)
		}
		if _, err := empty.Text(); err == nil || !strings.Contains(err.Error(), "no elements found") {
			t.Fatalf("wrong zero-element text error: %v", err)
		}
		f.reset("s1")
		if err := empty.Fill("x"); err == nil || !strings.Contains(err.Error(), "no elements found") {
			t.Fatalf("wrong zero-element fill error: %v", err)
		}
		if actions := f.actions("s1"); len(actions) != 0 {
			t.Fatalf("zero-element fill executed actions: %v", actionStrings(actions))
		}

		multiple := page.All("section").Query(agouti.QueryCSS("input.primary"))
		if _, err := multiple.Text(); err == nil || !strings.Contains(err.Error(), "does not support multiple elements (2)") {
			t.Fatalf("wrong multiple-element text error: %v", err)
		}

		one := page.Query(agouti.QueryCSS("button.pending").At(1))
		if text, err := one.Text(); err != nil || text != "two" {
			t.Fatalf("wrong exactly-one text %q, %v", text, err)
		}

		if _, err := page.Query(agouti.QueryCSS("input.primary").At(10)).Text(); err == nil || !strings.Contains(err.Error(), "out of range") {
			t.Fatalf("wrong out-of-range error: %v", err)
		}
	})
}
