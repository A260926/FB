package selenium_test

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
	"sync"
	"testing"
	"time"

	selenium "github.com/tebeka/selenium"
)

const (
	shadowElemKey = "element-6066-11e4-a52e-4f735466cecf"
	shadowRootKey = "shadow-6066-11e4-a52e-4f735466cecf"
	shadowSession = "sess"
)

// shadowCall is one HTTP request observed by the in-memory transport.
type shadowCall struct {
	method string
	path   string
	body   string
	// ctxErr is the state of the request context at the moment the request was
	// handled. Every request must carry a live, non-cancelled context.
	ctxErr error
}

type scriptedReply struct {
	status int
	body   string
}

// shadowMock is an http.RoundTripper that answers WebDriver requests from an
// in-process script, recording every request instead of opening a socket. No
// browser or external process is involved.
type shadowMock struct {
	mu     sync.Mutex
	calls  []shadowCall
	counts map[string]int
	w3c    bool
	script func(path, body string, nth int) scriptedReply
}

func (m *shadowMock) RoundTrip(r *http.Request) (*http.Response, error) {
	body := ""
	if r.Body != nil {
		buf, err := io.ReadAll(r.Body)
		if err != nil {
			return nil, err
		}
		body = string(buf)
	}

	m.mu.Lock()
	nth := m.counts[r.URL.Path]
	m.counts[r.URL.Path] = nth + 1
	m.calls = append(m.calls, shadowCall{
		method: r.Method,
		path:   r.URL.Path,
		body:   body,
		ctxErr: r.Context().Err(),
	})
	m.mu.Unlock()

	var reply scriptedReply
	if r.URL.Path == "/session" {
		if m.w3c {
			reply = scriptedReply{http.StatusOK,
				`{"value":{"sessionId":"` + shadowSession + `","capabilities":{"browserName":"chrome","browserVersion":"120.0.0"}}}`}
		} else {
			reply = scriptedReply{http.StatusOK,
				`{"sessionId":"` + shadowSession + `","status":0,"value":{"browserName":"chrome","version":"120.0.0"}}`}
		}
	} else {
		reply = m.script(r.URL.Path, body, nth)
	}

	return &http.Response{
		StatusCode: reply.status,
		Status:     fmt.Sprintf("%d OK", reply.status),
		Header:     http.Header{"Content-Type": []string{"application/json"}},
		Body:       io.NopCloser(strings.NewReader(reply.body)),
		Request:    r,
	}, nil
}

func (m *shadowMock) snapshot() []shadowCall {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]shadowCall, len(m.calls))
	copy(out, m.calls)
	return out
}

func (m *shadowMock) paths() []string {
	calls := m.snapshot()
	out := make([]string, len(calls))
	for i, c := range calls {
		out[i] = c.method + " " + c.path
	}
	return out
}

func (m *shadowMock) count(path string) int {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.counts[path]
}

// assertLiveContexts verifies every request was dispatched with a context that
// had not yet been cancelled when the request was handled.
func (m *shadowMock) assertLiveContexts(t *testing.T) {
	t.Helper()
	for _, c := range m.snapshot() {
		if c.ctxErr != nil {
			t.Fatalf("request %s %s carried a cancelled context: %v", c.method, c.path, c.ctxErr)
		}
	}
}

func newShadowDriver(t *testing.T, w3c bool, script func(path, body string, nth int) scriptedReply) (selenium.WebDriver, *shadowMock) {
	t.Helper()
	mock := &shadowMock{w3c: w3c, counts: map[string]int{}, script: script}
	old := selenium.HTTPClient
	selenium.HTTPClient = &http.Client{Transport: mock}
	t.Cleanup(func() { selenium.HTTPClient = old })

	wd, err := selenium.NewRemote(selenium.Capabilities{"browserName": "chrome"}, "http://webdriver.invalid")
	if err != nil {
		t.Fatalf("NewSession: %v", err)
	}
	return wd, mock
}

func shadowOK(body string) scriptedReply { return scriptedReply{http.StatusOK, body} }

func shadowW3CError(status int, errName, message string) scriptedReply {
	return scriptedReply{status, fmt.Sprintf(`{"value":{"error":%q,"message":%q}}`, errName, message)}
}

func shadowElementRef(id string) string {
	return fmt.Sprintf(`{"value":{%q:%q}}`, shadowElemKey, id)
}

func shadowRootRef(id string) string {
	return fmt.Sprintf(`{"value":{%q:%q}}`, shadowRootKey, id)
}

// shadowElementID reads the W3C element reference out of a WebElement, using
// its public JSON representation, so tests can assert the identity of the
// element the client ultimately operates on.
func shadowElementID(t *testing.T, elem selenium.WebElement) string {
	t.Helper()
	data, err := json.Marshal(elem)
	if err != nil {
		t.Fatalf("marshal element: %v", err)
	}
	var refs map[string]string
	if err := json.Unmarshal(data, &refs); err != nil {
		t.Fatalf("unmarshal element: %v", err)
	}
	id := refs[shadowElemKey]
	if id == "" {
		t.Fatalf("element has no W3C reference: %s", data)
	}
	return id
}

func shadowFindBody(t *testing.T, call shadowCall) (using, value string) {
	t.Helper()
	var req struct {
		Using string `json:"using"`
		Value string `json:"value"`
	}
	if err := json.Unmarshal([]byte(call.body), &req); err != nil {
		t.Fatalf("find request body %q is not JSON: %v", call.body, err)
	}
	return req.Using, req.Value
}

// TestShadowTwoLevelRootsFill drives the real client through two nested shadow
// roots, fills the contact field in the inner one, and checks the exact W3C
// endpoints and the identity of the filled element.
func TestShadowTwoLevelRootsFill(t *testing.T) {
	const (
		outerHost = "outer-host"
		outerRoot = "outer-root"
		innerHost = "inner-host"
		innerRoot = "inner-root"
		input     = "contact-input"
	)
	script := func(path, body string, nth int) scriptedReply {
		switch path {
		case "/session/" + shadowSession + "/element":
			return shadowOK(shadowElementRef(outerHost))
		case "/session/" + shadowSession + "/element/" + outerHost + "/shadow":
			return shadowOK(shadowRootRef(outerRoot))
		case "/session/" + shadowSession + "/shadow/" + outerRoot + "/element":
			return shadowOK(shadowElementRef(innerHost))
		case "/session/" + shadowSession + "/element/" + innerHost + "/shadow":
			return shadowOK(shadowRootRef(innerRoot))
		case "/session/" + shadowSession + "/shadow/" + innerRoot + "/element":
			return shadowOK(shadowElementRef(input))
		case "/session/" + shadowSession + "/shadow/" + innerRoot + "/elements":
			return shadowOK(fmt.Sprintf(`{"value":[{%q:%q},{%q:%q}]}`,
				shadowElemKey, input, shadowElemKey, input+"-2"))
		case "/session/" + shadowSession + "/element/" + input + "/value":
			if !strings.Contains(body, "Ada Lovelace") {
				t.Errorf("SendKeys body = %q, want it to contain the typed text", body)
			}
			return shadowOK(`{"value":null}`)
		case "/session/" + shadowSession + "/element/" + input + "/attribute/data-id":
			return shadowOK(`{"value":"contact-attr"}`)
		default:
			t.Errorf("unexpected request to %q", path)
			return scriptedReply{http.StatusInternalServerError, `{"value":{"error":"unknown error","message":"unexpected path"}}`}
		}
	}
	wd, mock := newShadowDriver(t, true, script)

	host, err := wd.FindElement(selenium.ByCSSSelector, "contact-card")
	if err != nil {
		t.Fatalf("find outer host: %v", err)
	}
	root1, err := selenium.GetShadowRoot(host)
	if err != nil {
		t.Fatalf("get outer root: %v", err)
	}
	inner, err := root1.FindElement(selenium.ByCSSSelector, "inner-card")
	if err != nil {
		t.Fatalf("find inner host: %v", err)
	}
	root2, err := selenium.GetShadowRoot(inner)
	if err != nil {
		t.Fatalf("get inner root: %v", err)
	}

	// A root search uses the shadow endpoint and returns an ordinary
	// WebElement that supports the full element API.
	field, err := root2.FindElement(selenium.ByID, "contact")
	if err != nil {
		t.Fatalf("find field in inner root: %v", err)
	}
	if id := shadowElementID(t, field); id != input {
		t.Fatalf("field id = %q, want %q", id, input)
	}
	using, value := shadowFindBody(t, mock.snapshot()[len(mock.snapshot())-1])
	if using != selenium.ByCSSSelector || value != "#contact" {
		t.Fatalf("inner find = (%q, %q), want (css selector, #contact)", using, value)
	}

	listed, err := root2.FindElements(selenium.ByName, "field")
	if err != nil {
		t.Fatalf("list fields: %v", err)
	}
	if len(listed) != 2 {
		t.Fatalf("got %d listed elements, want 2", len(listed))
	}
	if id := shadowElementID(t, listed[0]); id != input {
		t.Fatalf("listed[0] id = %q, want %q", id, input)
	}

	// The returned element remains a normal remote WebElement: typing and
	// attribute reads hit the standard element endpoints with its handle.
	if err := field.SendKeys("Ada Lovelace"); err != nil {
		t.Fatalf("SendKeys: %v", err)
	}
	attr, err := field.GetAttribute("data-id")
	if err != nil {
		t.Fatalf("GetAttribute: %v", err)
	}
	if attr != "contact-attr" {
		t.Fatalf("attribute = %q, want contact-attr", attr)
	}

	wantPaths := []string{
		"POST /session",
		"POST /session/sess/element",
		"POST /session/sess/element/outer-host/shadow",
		"POST /session/sess/shadow/outer-root/element",
		"POST /session/sess/element/inner-host/shadow",
		"POST /session/sess/shadow/inner-root/element",
		"POST /session/sess/shadow/inner-root/elements",
		"POST /session/sess/element/contact-input/value",
		"GET /session/sess/element/contact-input/attribute/data-id",
	}
	if got := mock.paths(); fmt.Sprint(got) != fmt.Sprint(wantPaths) {
		t.Fatalf("request sequence:\n got %v\nwant %v", got, wantPaths)
	}
	mock.assertLiveContexts(t)
}

// TestShadowComponentRefreshRestartsFromFirstStep makes the second-level root
// go detached mid-traversal and verifies ResolveShadowPath discards every
// intermediate handle and re-walks the path from the first, page-level step.
func TestShadowComponentRefreshRestartsFromFirstStep(t *testing.T) {
	steps := []selenium.ShadowStep{
		{By: selenium.ByCSSSelector, Value: "contact-card", EnterShadow: true},
		{By: selenium.ByCSSSelector, Value: "inner-card", EnterShadow: true},
		{By: selenium.ByID, Value: "contact"},
	}
	// attempt counts traversals: the page-level element endpoint is the first
	// request of each traversal, so its count identifies which component
	// generation every other handle must belong to.
	attempt := 0
	script := func(path, body string, nth int) scriptedReply {
		suffix := "a"
		if path == "/session/"+shadowSession+"/element" {
			if attempt >= 1 {
				suffix = "b"
			}
			attempt++
		} else if attempt >= 2 {
			suffix = "b"
		}
		switch {
		case path == "/session/"+shadowSession+"/element":
			return shadowOK(shadowElementRef("outer-host-" + suffix))
		case strings.HasSuffix(path, "/shadow") && strings.Contains(path, "outer-host-"):
			return shadowOK(shadowRootRef("outer-root-" + suffix))
		case strings.HasPrefix(path, "/session/"+shadowSession+"/shadow/outer-root-") &&
			strings.HasSuffix(path, "/element"):
			return shadowOK(shadowElementRef("inner-host-" + suffix))
		case strings.HasSuffix(path, "/shadow") && strings.Contains(path, "inner-host-"):
			return shadowOK(shadowRootRef("inner-root-" + suffix))
		case strings.HasPrefix(path, "/session/"+shadowSession+"/shadow/inner-root-") &&
			strings.HasSuffix(path, "/element"):
			if suffix == "a" {
				// The component was rebuilt: the saved inner root is detached.
				return shadowW3CError(http.StatusNotFound, "detached shadow root", "component rebuilt")
			}
			return shadowOK(shadowElementRef("contact-input-" + suffix))
		case path == "/session/"+shadowSession+"/element/contact-input-b/value":
			return shadowOK(`{"value":null}`)
		default:
			t.Errorf("unexpected request to %q", path)
			return shadowW3CError(http.StatusInternalServerError, "unknown error", "unexpected path")
		}
	}
	wd, mock := newShadowDriver(t, true, script)

	elem, err := selenium.ResolveShadowPath(context.Background(), wd, steps, time.Millisecond)
	if err != nil {
		t.Fatalf("ResolveShadowPath: %v", err)
	}
	if id := shadowElementID(t, elem); id != "contact-input-b" {
		t.Fatalf("resolved id = %q, want contact-input-b from the rebuilt component", id)
	}
	if err := elem.SendKeys("Ada"); err != nil {
		t.Fatalf("SendKeys on resolved element: %v", err)
	}

	pageFinds := mock.count("/session/" + shadowSession + "/element")
	if pageFinds != 2 {
		t.Fatalf("page-level finds = %d, want 2 traversals restarting from step one", pageFinds)
	}
	calls := mock.snapshot()
	secondTraversal := 0
	for i, c := range calls {
		if c.path == "/session/"+shadowSession+"/element" {
			secondTraversal = i
		}
	}
	for _, c := range calls[secondTraversal:] {
		if strings.Contains(c.path, "-a") {
			t.Fatalf("retry reused a stale handle from the failed traversal: %s", c.path)
		}
	}
	if mock.count("/session/"+shadowSession+"/element/contact-input-b/value") != 1 {
		t.Fatalf("fill was not performed exactly once against the resolved element: %v", mock.paths())
	}
	mock.assertLiveContexts(t)
}

// TestShadowReferenceDecoding feeds malformed handles to both the root and
// element decoders, including null references, wrong types, mixed keys and a
// bad entry inside a list. None may produce a usable handle, and a list
// failure must never return partial results.
func TestShadowReferenceDecoding(t *testing.T) {
	// badPayloads maps a subtest to the malformed server response, keyed by
	// which endpoint is exercised by that subtest.
	rootBad := map[string]string{
		"shadow root value is null":                   `{"value":null}`,
		"shadow root object is empty":                 `{"value":{}}`,
		"shadow root and element keys together":       fmt.Sprintf(`{"value":{%q:"e",%q:"r"}}`, shadowElemKey, shadowRootKey),
		"element key where a shadow root is required": shadowElementRef("not-a-root"),
		"shadow root value has the wrong JSON type":   `{"value":42}`,
	}
	elementBad := []struct {
		name    string
		payload string
		list    bool
	}{
		{"element value is null", `{"value":null}`, false},
		{"element value has the wrong JSON type", `{"value":42}`, false},
		{"root key where an element is required", shadowRootRef("not-an-element"), false},
		{"element and shadow keys together",
			fmt.Sprintf(`{"value":{%q:"elem",%q:"root"}}`, shadowElemKey, shadowRootKey), false},
		{"element list containing a bad reference",
			fmt.Sprintf(`{"value":[{%q:"good"},{%q:"bad"}]}`, shadowElemKey, shadowRootKey), true},
		{"element list containing mixed element and shadow keys",
			fmt.Sprintf(`{"value":[{%q:"elem",%q:"root"}]}`, shadowElemKey, shadowRootKey), true},
	}
	for name, payload := range rootBad {
		name, payload := name, payload
		t.Run(name, func(t *testing.T) {
			script := func(path, body string, nth int) scriptedReply {
				switch path {
				case "/session/" + shadowSession + "/element":
					return shadowOK(shadowElementRef("host"))
				case "/session/" + shadowSession + "/element/host/shadow":
					// The first call below obtains a good root; every later
					// shadow fetch returns the malformed payload under test.
					if nth == 0 {
						return shadowOK(shadowRootRef("root"))
					}
					return shadowOK(payload)
				default:
					return shadowW3CError(http.StatusInternalServerError, "unknown error", "unexpected "+path)
				}
			}
			wd, mock := newShadowDriver(t, true, script)
			host, err := wd.FindElement(selenium.ByCSSSelector, "host")
			if err != nil {
				t.Fatal(err)
			}
			if _, err := selenium.GetShadowRoot(host); err != nil {
				t.Fatalf("baseline root fetch: %v", err)
			}
			if _, err := selenium.GetShadowRoot(host); err == nil {
				t.Fatalf("malformed payload %q decoded into a usable shadow root handle", payload)
			}
			mock.assertLiveContexts(t)
		})
	}
	for _, tc := range elementBad {
		tc := tc
		t.Run(tc.name, func(t *testing.T) {
			payload := tc.payload
			script := func(path, body string, nth int) scriptedReply {
				switch {
				case path == "/session/"+shadowSession+"/element":
					return shadowOK(shadowElementRef("host"))
				case path == "/session/"+shadowSession+"/element/host/shadow":
					return shadowOK(shadowRootRef("root"))
				case strings.HasPrefix(path, "/session/"+shadowSession+"/shadow/root/element"):
					return shadowOK(payload)
				default:
					return shadowW3CError(http.StatusInternalServerError, "unknown error", "unexpected "+path)
				}
			}
			wd, mock := newShadowDriver(t, true, script)
			host, err := wd.FindElement(selenium.ByCSSSelector, "host")
			if err != nil {
				t.Fatal(err)
			}
			root, err := selenium.GetShadowRoot(host)
			if err != nil {
				t.Fatal(err)
			}
			if tc.list {
				elems, err := root.FindElements(selenium.ByCSSSelector, "input")
				if err == nil {
					t.Fatalf("malformed list accepted, elems=%v", elems)
				}
				if elems != nil {
					t.Fatalf("partial results handed back: %v", elems)
				}
			} else if _, err := root.FindElement(selenium.ByCSSSelector, "input"); err == nil {
				t.Fatalf("malformed payload %q decoded into a usable element handle", payload)
			}
			mock.assertLiveContexts(t)
		})
	}

	// An empty list is a legitimate response and must decode to an empty,
	// error-free result rather than being treated as a missing reference.
	t.Run("empty element list", func(t *testing.T) {
		script := func(path, body string, nth int) scriptedReply {
			switch {
			case path == "/session/"+shadowSession+"/element":
				return shadowOK(shadowElementRef("host"))
			case path == "/session/"+shadowSession+"/element/host/shadow":
				return shadowOK(shadowRootRef("root"))
			case path == "/session/"+shadowSession+"/shadow/root/elements":
				return shadowOK(`{"value":[]}`)
			}
			return shadowW3CError(http.StatusInternalServerError, "unknown error", "unexpected "+path)
		}
		wd, mock := newShadowDriver(t, true, script)
		host, err := wd.FindElement(selenium.ByCSSSelector, "host")
		if err != nil {
			t.Fatal(err)
		}
		root, err := selenium.GetShadowRoot(host)
		if err != nil {
			t.Fatal(err)
		}
		elems, err := root.FindElements(selenium.ByCSSSelector, "input")
		if err != nil {
			t.Fatalf("empty list: %v", err)
		}
		if len(elems) != 0 {
			t.Fatalf("got %d elements, want 0", len(elems))
		}
		mock.assertLiveContexts(t)
	})
}

// TestShadowRetryableErrors covers every protocol error that must trigger a
// full re-resolution from the first step.
func TestShadowRetryableErrors(t *testing.T) {
	retryable := []string{
		"no such element",
		"no such shadow root",
		"detached shadow root",
		"stale element reference",
	}
	for _, errName := range retryable {
		t.Run(errName, func(t *testing.T) {
			steps := []selenium.ShadowStep{
				{By: selenium.ByCSSSelector, Value: "host", EnterShadow: true},
				{By: selenium.ByCSSSelector, Value: "field"},
			}
			script := func(path, body string, nth int) scriptedReply {
				switch path {
				case "/session/" + shadowSession + "/element":
					if nth == 0 {
						return shadowOK(shadowElementRef("host-1"))
					}
					return shadowOK(shadowElementRef("host-2"))
				case "/session/" + shadowSession + "/element/host-1/shadow":
					return shadowOK(shadowRootRef("root-1"))
				case "/session/" + shadowSession + "/shadow/root-1/element":
					return shadowW3CError(http.StatusNotFound, errName, "first traversal fails")
				case "/session/" + shadowSession + "/element/host-2/shadow":
					return shadowOK(shadowRootRef("root-2"))
				case "/session/" + shadowSession + "/shadow/root-2/element":
					return shadowOK(shadowElementRef("field-2"))
				default:
					return shadowW3CError(http.StatusInternalServerError, "unknown error", "unexpected "+path)
				}
			}
			wd, mock := newShadowDriver(t, true, script)

			elem, err := selenium.ResolveShadowPath(context.Background(), wd, steps, time.Millisecond)
			if err != nil {
				t.Fatalf("ResolveShadowPath: %v", err)
			}
			if id := shadowElementID(t, elem); id != "field-2" {
				t.Fatalf("resolved id = %q, want field-2", id)
			}
			if got := mock.count("/session/" + shadowSession + "/element"); got != 2 {
				t.Fatalf("page-level finds = %d, want a fresh traversal after %q", got, errName)
			}
			mock.assertLiveContexts(t)
		})
	}
}

// TestShadowCancellationStopsRetrying verifies that a cancelled context never
// starts another traversal, while the in-flight attempt is allowed to finish.
func TestShadowCancellationStopsRetrying(t *testing.T) {
	script := func(path, body string, nth int) scriptedReply {
		switch path {
		case "/session/" + shadowSession + "/element":
			// Retryable failure: without cancellation the client would loop.
			return shadowW3CError(http.StatusNotFound, "no such element", "not yet rendered")
		default:
			return shadowW3CError(http.StatusInternalServerError, "unknown error", "unexpected "+path)
		}
	}
	wd, mock := newShadowDriver(t, true, script)

	ctx, cancel := context.WithCancel(context.Background())
	cancel() // Already cancelled before the first attempt completes its retry check.

	steps := []selenium.ShadowStep{{By: selenium.ByCSSSelector, Value: "host"}}
	_, err := selenium.ResolveShadowPath(ctx, wd, steps, time.Millisecond)
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("err = %v, want context.Canceled", err)
	}
	// One traversal ran (session + one page find); cancellation forbids a
	// second round even though the failure was retryable.
	if got := mock.count("/session/" + shadowSession + "/element"); got != 1 {
		t.Fatalf("page-level finds = %d, want exactly 1 before cancellation", got)
	}
	mock.assertLiveContexts(t)
}

// TestShadowNonRetryableError verifies that protocol errors outside the
// stale-reference set are returned immediately with the Selenium Error type
// and fields intact, instead of being swallowed by the wait loop.
func TestShadowNonRetryableError(t *testing.T) {
	steps := []selenium.ShadowStep{
		{By: selenium.ByCSSSelector, Value: "host", EnterShadow: true},
		{By: selenium.ByCSSSelector, Value: "field"},
	}
	script := func(path, body string, nth int) scriptedReply {
		switch path {
		case "/session/" + shadowSession + "/element":
			return shadowOK(shadowElementRef("host-1"))
		case "/session/" + shadowSession + "/element/host-1/shadow":
			return shadowOK(shadowRootRef("root-1"))
		case "/session/" + shadowSession + "/shadow/root-1/element":
			return shadowW3CError(http.StatusBadRequest, "invalid selector", "malformed selector")
		default:
			return shadowW3CError(http.StatusInternalServerError, "unknown error", "unexpected "+path)
		}
	}
	wd, mock := newShadowDriver(t, true, script)

	_, err := selenium.ResolveShadowPath(context.Background(), wd, steps, time.Millisecond)
	var se *selenium.Error
	if !errors.As(err, &se) {
		t.Fatalf("err = %T %v, want *selenium.Error", err, err)
	}
	if se.Err != "invalid selector" || se.Message != "malformed selector" || se.HTTPCode != http.StatusBadRequest {
		t.Fatalf("error fields lost: %+v", se)
	}
	// The non-retryable failure must not trigger a second traversal.
	if got := mock.count("/session/" + shadowSession + "/element"); got != 1 {
		t.Fatalf("page-level finds = %d, want 1, retries must not happen", got)
	}
	mock.assertLiveContexts(t)
}

// TestShadowPathValidation checks the up-front validation rules: no request is
// issued for an invalid path.
func TestShadowPathValidation(t *testing.T) {
	wd, mock := newShadowDriver(t, true, func(path, body string, nth int) scriptedReply {
		t.Errorf("no request expected for invalid path, got %s", path)
		return shadowW3CError(http.StatusInternalServerError, "unknown error", "unexpected")
	})
	cases := []struct {
		name  string
		steps []selenium.ShadowStep
		wait  time.Duration
	}{
		{"no steps", nil, time.Millisecond},
		{"final step enters shadow", []selenium.ShadowStep{
			{By: selenium.ByCSSSelector, Value: "host", EnterShadow: true},
		}, time.Millisecond},
		{"zero interval", []selenium.ShadowStep{
			{By: selenium.ByCSSSelector, Value: "host"},
		}, 0},
		{"negative interval", []selenium.ShadowStep{
			{By: selenium.ByCSSSelector, Value: "host"},
		}, -time.Second},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if _, err := selenium.ResolveShadowPath(context.Background(), wd, tc.steps, tc.wait); err == nil {
				t.Fatalf("expected validation error for %s", tc.name)
			}
		})
	}
	// Only the session creation request exists; validation ran before any
	// path request.
	if len(mock.snapshot()) != 1 {
		t.Fatalf("requests = %v, want only the session request", mock.paths())
	}
	mock.assertLiveContexts(t)
}

// TestShadowLegacySessionUnsupported verifies that a JSON Wire Protocol
// session rejects shadow root access outright, without issuing shadow or
// script requests as a fallback.
func TestShadowLegacySessionUnsupported(t *testing.T) {
	script := func(path, body string, nth int) scriptedReply {
		switch path {
		case "/session/" + shadowSession + "/element":
			return shadowOK(shadowElementRef("legacy-host"))
		default:
			return shadowW3CError(http.StatusInternalServerError, "unknown error", "unexpected "+path)
		}
	}
	wd, mock := newShadowDriver(t, false, script)

	host, err := wd.FindElement(selenium.ByCSSSelector, "host")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := selenium.GetShadowRoot(host); err == nil ||
		!strings.Contains(err.Error(), "W3C") {
		t.Fatalf("legacy GetShadowRoot err = %v, want an unsupported W3C error", err)
	}
	for _, c := range mock.snapshot() {
		if strings.Contains(c.path, "/shadow") || strings.Contains(c.path, "/execute") {
			t.Fatalf("legacy session issued a forbidden fallback request: %s", c.path)
		}
	}

	steps := []selenium.ShadowStep{
		{By: selenium.ByCSSSelector, Value: "host", EnterShadow: true},
		{By: selenium.ByCSSSelector, Value: "field"},
	}
	if _, err := selenium.ResolveShadowPath(context.Background(), wd, steps, time.Millisecond); err == nil ||
		!strings.Contains(err.Error(), "W3C") {
		t.Fatalf("legacy ResolveShadowPath err = %v, want an unsupported W3C error", err)
	}
	// ResolveShadowPath legitimately locates the step-one element before
	// GetShadowRoot rejects the legacy session; the rejection is still
	// non-retryable, so exactly one traversal ran and no shadow request was
	// issued.
	if got := mock.count("/session/" + shadowSession + "/element"); got != 2 {
		t.Fatalf("page-level finds = %d, want 2 (one standalone, one in the single traversal)", got)
	}
	for _, c := range mock.snapshot() {
		if strings.Contains(c.path, "/shadow") || strings.Contains(c.path, "/execute") {
			t.Fatalf("legacy session issued a forbidden fallback request: %s", c.path)
		}
	}
	mock.assertLiveContexts(t)
}

// TestShadowSpecialIDAndNameLocators verifies that ID and name locators are
// translated to CSS with every literal character escaped: quotes, backslashes,
// spaces, leading digits and non-ASCII text must keep their literal meaning.
func TestShadowSpecialIDAndNameLocators(t *testing.T) {
	const (
		specialID   = `1a"b\c dé`
		wantIDCSS   = `#\000031a\000022b\00005cc\000020d\0000e9`
		specialName = `n"m\o 名`
		wantNameCSS = `[name=n\000022m\00005co\000020\00540d]`
	)
	script := func(path, body string, nth int) scriptedReply {
		switch {
		case path == "/session/"+shadowSession+"/element":
			return shadowOK(shadowElementRef("host"))
		case path == "/session/"+shadowSession+"/element/host/shadow":
			return shadowOK(shadowRootRef("root"))
		case strings.HasPrefix(path, "/session/"+shadowSession+"/shadow/root/elements"):
			return shadowOK(fmt.Sprintf(`{"value":[{%q:"special-input"}]}`, shadowElemKey))
		case strings.HasPrefix(path, "/session/"+shadowSession+"/shadow/root/element"):
			return shadowOK(shadowElementRef("special-input"))
		case path == "/session/"+shadowSession+"/element/special-input/value":
			return shadowOK(`{"value":null}`)
		default:
			return shadowW3CError(http.StatusInternalServerError, "unknown error", "unexpected "+path)
		}
	}
	wd, mock := newShadowDriver(t, true, script)

	host, err := wd.FindElement(selenium.ByCSSSelector, "host")
	if err != nil {
		t.Fatal(err)
	}
	root, err := selenium.GetShadowRoot(host)
	if err != nil {
		t.Fatal(err)
	}

	before := len(mock.snapshot())
	elem, err := root.FindElement(selenium.ByID, specialID)
	if err != nil {
		t.Fatalf("FindElement by special ID: %v", err)
	}
	using, value := shadowFindBody(t, mock.snapshot()[before])
	if using != selenium.ByCSSSelector || value != wantIDCSS {
		t.Fatalf("ID locator sent (%q, %q), want (css selector, %q)", using, value, wantIDCSS)
	}

	before = len(mock.snapshot())
	listed, err := root.FindElements(selenium.ByName, specialName)
	if err != nil {
		t.Fatalf("FindElements by special name: %v", err)
	}
	using, value = shadowFindBody(t, mock.snapshot()[before])
	if using != selenium.ByCSSSelector || value != wantNameCSS {
		t.Fatalf("name locator sent (%q, %q), want (css selector, %q)", using, value, wantNameCSS)
	}
	if len(listed) != 1 || shadowElementID(t, listed[0]) != "special-input" {
		t.Fatalf("listed element identity wrong: %v", listed)
	}

	// A plain CSS selector is forwarded untouched, and an unsupported strategy
	// is rejected client-side without any request.
	before = len(mock.snapshot())
	if _, err := root.FindElement(selenium.ByCSSSelector, `input[naïve]`); err != nil {
		t.Fatalf("passthrough CSS: %v", err)
	}
	using, value = shadowFindBody(t, mock.snapshot()[before])
	if using != selenium.ByCSSSelector || value != `input[naïve]` {
		t.Fatalf("CSS locator altered to (%q, %q)", using, value)
	}
	before = len(mock.snapshot())
	if _, err := root.FindElement(selenium.ByXPATH, "//input"); err == nil {
		t.Fatal("xpath shadow search was accepted")
	}
	if len(mock.snapshot()) != before {
		t.Fatalf("unsupported strategy issued a request: %v", mock.paths())
	}

	// The element located with the escaped selectors is the same handle the
	// subsequent fill operation targets.
	if err := elem.SendKeys("Grace"); err != nil {
		t.Fatalf("SendKeys: %v", err)
	}
	if mock.count("/session/"+shadowSession+"/element/special-input/value") != 1 {
		t.Fatalf("fill did not target the located element: %v", mock.paths())
	}
	mock.assertLiveContexts(t)
}

// ExampleResolveShadowPath shows how to reach a contact field nested inside
// two web components and re-resolve the whole path automatically when either
// component is rebuilt. It uses only the W3C shadow endpoints, never injected
// JavaScript. The example is compile-only: it needs a live WebDriver server.
func ExampleResolveShadowPath() {
	caps := selenium.Capabilities{"browserName": "chrome"}
	wd, err := selenium.NewRemote(caps, "http://127.0.0.1:4444/wd/hub")
	if err != nil {
		panic(err)
	}
	defer wd.Quit()

	// Step 1 finds the outer host and enters its shadow root; step 2 finds
	// the nested host and enters its root; step 3 returns the contact input.
	steps := []selenium.ShadowStep{
		{By: selenium.ByCSSSelector, Value: "contact-card", EnterShadow: true},
		{By: selenium.ByCSSSelector, Value: "contact-form", EnterShadow: true},
		{By: selenium.ByID, Value: "email"},
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	field, err := selenium.ResolveShadowPath(ctx, wd, steps, 50*time.Millisecond)
	if err != nil {
		panic(err)
	}
	if err := field.SendKeys("ada@example.com"); err != nil {
		panic(err)
	}

	// The same handles can also be driven one endpoint at a time.
	host, err := wd.FindElement(selenium.ByCSSSelector, "contact-card")
	if err != nil {
		panic(err)
	}
	root, err := selenium.GetShadowRoot(host)
	if err != nil {
		panic(err)
	}
	nested, err := root.FindElement(selenium.ByCSSSelector, "contact-form")
	if err != nil {
		panic(err)
	}
	innerRoot, err := selenium.GetShadowRoot(nested)
	if err != nil {
		panic(err)
	}
	if _, err := innerRoot.FindElement(selenium.ByName, "email"); err != nil {
		panic(err)
	}
}
