package task_support_test

import (
	"context"
	"encoding/json"
	"errors"
	"sync"
	"testing"
	"time"

	"github.com/go-rod/rod"
	"github.com/go-rod/rod/lib/proto"
	"github.com/go-rod/rod/task_support/cdpstub"
)

const testTimeout = 3 * time.Second

var errBoom = errors.New("boom")

// harness wires a stub cdp client to a real rod Browser.
type harness struct {
	t *testing.T
	b *rod.Browser
	c *cdpstub.Client
}

func newHarness(t *testing.T) *harness {
	t.Helper()

	c := cdpstub.New()
	b := rod.New().Context(context.Background()).Client(c)
	if err := b.Connect(); err != nil {
		t.Fatalf("connect: %v", err)
	}

	t.Cleanup(func() { _ = b.Close() })

	return &harness{t: t, b: b, c: c}
}

func pausedEvent(session, id string, rt proto.NetworkResourceType, url string) *proto.FetchRequestPaused {
	return &proto.FetchRequestPaused{
		RequestID:    proto.FetchRequestID(id),
		ResourceType: rt,
		Request: &proto.NetworkRequest{
			URL:     url,
			Method:  "GET",
			Headers: proto.NetworkHeaders{},
		},
	}
}

func authEvent(session, id string, rt proto.NetworkResourceType, url string) *proto.FetchAuthRequired {
	return &proto.FetchAuthRequired{
		RequestID:    proto.FetchRequestID(id),
		ResourceType: rt,
		Request: &proto.NetworkRequest{
			URL:     url,
			Method:  "GET",
			Headers: proto.NetworkHeaders{},
		},
		AuthChallenge: &proto.FetchAuthChallenge{Origin: url, Scheme: "basic", Realm: "zone"},
	}
}

func (h *harness) emitPaused(session, id string, rt proto.NetworkResourceType, url string) {
	h.t.Helper()
	if err := h.c.Emit(session, "Fetch.requestPaused", pausedEvent(session, id, rt, url)); err != nil {
		h.t.Fatalf("emit paused: %v", err)
	}
}

func (h *harness) emitAuth(session, id string, rt proto.NetworkResourceType, url string) {
	h.t.Helper()
	if err := h.c.Emit(session, "Fetch.authRequired", authEvent(session, id, rt, url)); err != nil {
		h.t.Fatalf("emit auth: %v", err)
	}
}

// lastEnable returns the params of the last confirmed Fetch.enable sent to
// the session; rejected attempts are ignored.
func (h *harness) lastEnable(session string) proto.FetchEnable {
	h.t.Helper()

	var raw json.RawMessage
	for _, r := range h.c.Records() {
		if r.Session == session && r.Method == "Fetch.enable" && r.Err == nil {
			raw = r.Params
		}
	}

	if raw == nil {
		h.t.Fatal("no confirmed Fetch.enable recorded")
	}

	var en proto.FetchEnable
	if err := json.Unmarshal(raw, &en); err != nil {
		h.t.Fatalf("unmarshal enable: %v", err)
	}

	return en
}

func (h *harness) waitUntil(msg string, cond func() bool) {
	h.t.Helper()

	deadline := time.Now().Add(testTimeout)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(5 * time.Millisecond)
	}

	h.t.Fatal(msg)
}

// countCommands counts recorded commands of the method on the session whose
// params carry the given request id.
func (h *harness) countCommands(session, method, id string) int {
	h.t.Helper()

	n := 0
	for _, r := range h.c.Records() {
		if r.Session != session || r.Method != method {
			continue
		}

		var p struct {
			RequestID string `json:"requestId"`
		}
		_ = json.Unmarshal(r.Params, &p)
		if p.RequestID == id {
			n++
		}
	}

	return n
}

func (h *harness) countMethod(session, method string) int {
	h.t.Helper()

	n := 0
	for _, r := range h.c.Records() {
		if r.Session == session && r.Method == method {
			n++
		}
	}

	return n
}

// findCommand returns the first recorded command matching session/method/id.
func (h *harness) findCommand(session, method, id string) (cdpstub.Record, bool) {
	h.t.Helper()

	for _, r := range h.c.Records() {
		if r.Session != session || r.Method != method {
			continue
		}

		var p struct {
			RequestID string `json:"requestId"`
		}
		_ = json.Unmarshal(r.Params, &p)
		if p.RequestID == id {
			return r, true
		}
	}

	return cdpstub.Record{}, false
}

// authAnswer decodes the continueWithAuth command of the request id into a
// fresh struct so absent fields never inherit a previous decode.
func (h *harness) authAnswer(session, id string) proto.FetchAuthChallengeResponse {
	h.t.Helper()

	rec, ok := h.findCommand(session, "Fetch.continueWithAuth", id)
	if !ok {
		h.t.Fatalf("continueWithAuth for %q not recorded", id)
	}

	var cmd proto.FetchContinueWithAuth
	if err := json.Unmarshal(rec.Params, &cmd); err != nil {
		h.t.Fatalf("decode continueWithAuth: %v", err)
	}

	return *cmd.AuthChallengeResponse
}

// assertPatterns checks the full pattern list of a Fetch.enable config.
func assertPatterns(t *testing.T, en proto.FetchEnable, wantAuth bool, want [][2]string) {
	t.Helper()

	if en.HandleAuthRequests != wantAuth {
		t.Fatalf("handleAuthRequests = %v, want %v (params: %+v)", en.HandleAuthRequests, wantAuth, en)
	}

	if len(en.Patterns) != len(want) {
		t.Fatalf("patterns len = %d, want %d: %+v", len(en.Patterns), len(want), en.Patterns)
	}

	for i, p := range want {
		if en.Patterns[i].URLPattern != p[0] || string(en.Patterns[i].ResourceType) != p[1] {
			t.Fatalf("pattern %d = (%s,%s), want (%s,%s)",
				i, en.Patterns[i].URLPattern, en.Patterns[i].ResourceType, p[0], p[1])
		}
	}
}

// routeCtl controls what a normal handler does for the next paused event.
type routeCtl struct {
	mu       sync.Mutex
	action   string // "skip", "continue", "fail", "fulfill"
	log      chan string
	skipSeen chan bool
	errLog   chan error
}

func newRouteCtl() *routeCtl {
	return &routeCtl{
		action:   "continue",
		log:      make(chan string, 64),
		skipSeen: make(chan bool, 64),
		errLog:   make(chan error, 64),
	}
}

func (c *routeCtl) set(action string) {
	c.mu.Lock()
	c.action = action
	c.mu.Unlock()
}

func (c *routeCtl) get() string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.action
}

func (c *routeCtl) handler(name string) func(*rod.Hijack) {
	return func(h *rod.Hijack) {
		h.OnError = func(err error) { c.errLog <- err }
		c.log <- name
		c.skipSeen <- h.Skip

		switch c.get() {
		case "skip":
			h.Skip = true
		case "continue":
			h.ContinueRequest(&proto.FetchContinueRequest{})
		case "fail":
			h.Response.Fail(proto.NetworkErrorReasonAborted)
		case "fulfill":
			h.Response.SetBody("mocked")
		}
	}
}

func (c *routeCtl) calls() []string {
	out := []string{}
	for {
		select {
		case name := <-c.log:
			out = append(out, name)
		default:
			return out
		}
	}
}

func (c *routeCtl) waitCalls(t *testing.T, n int) []string {
	t.Helper()

	out := []string{}
	deadline := time.Now().Add(testTimeout)
	for len(out) < n {
		select {
		case name := <-c.log:
			out = append(out, name)
		case <-time.After(time.Until(deadline)):
			t.Fatalf("wait %d handler calls, got %d (%v)", n, len(out), out)
		}
	}

	return out
}

// TestRouterAuthConfig covers the merged Fetch.enable configuration produced by
// Add, Remove and SetAuthHandler in both registration orders.
func TestRouterAuthConfig(t *testing.T) {
	t.Run("routes first then auth", func(t *testing.T) {
		h := newHarness(t)
		r := h.b.HijackRequests()
		defer func() { _ = r.Stop() }()

		ctl := newRouteCtl()

		if err := r.Add("*", proto.NetworkResourceTypeDocument, ctl.handler("doc")); err != nil {
			t.Fatal(err)
		}
		assertPatterns(t, h.lastEnable(""), false, [][2]string{{"*", "Document"}})

		// Same url pattern, different resource types: both must be kept.
		if err := r.Add("https://a.test/*", proto.NetworkResourceTypeScript, ctl.handler("script")); err != nil {
			t.Fatal(err)
		}
		if err := r.Add("https://a.test/*", proto.NetworkResourceTypeXHR, ctl.handler("xhr")); err != nil {
			t.Fatal(err)
		}
		assertPatterns(t, h.lastEnable(""), false, [][2]string{
			{"*", "Document"},
			{"https://a.test/*", "Script"},
			{"https://a.test/*", "XHR"},
		})

		// Enabling auth must not drop any pattern.
		if err := r.SetAuthHandler(func(*proto.FetchAuthRequired) *proto.FetchAuthChallengeResponse {
			return nil
		}, nil); err != nil {
			t.Fatal(err)
		}
		assertPatterns(t, h.lastEnable(""), true, [][2]string{
			{"*", "Document"},
			{"https://a.test/*", "Script"},
			{"https://a.test/*", "XHR"},
		})

		// Remove keeps the remaining resource types intact, auth stays enabled.
		if err := r.Remove("*"); err != nil {
			t.Fatal(err)
		}
		assertPatterns(t, h.lastEnable(""), true, [][2]string{
			{"https://a.test/*", "Script"},
			{"https://a.test/*", "XHR"},
		})

		// Removing auth keeps the patterns.
		if err := r.SetAuthHandler(nil, nil); err != nil {
			t.Fatal(err)
		}
		assertPatterns(t, h.lastEnable(""), false, [][2]string{
			{"https://a.test/*", "Script"},
			{"https://a.test/*", "XHR"},
		})

		// Empty resource type means any type: it is omitted on the wire.
		if err := r.Add("https://b.test/*", "", ctl.handler("any")); err != nil {
			t.Fatal(err)
		}
		en := h.lastEnable("")
		assertPatterns(t, en, false, [][2]string{
			{"https://a.test/*", "Script"},
			{"https://a.test/*", "XHR"},
			{"https://b.test/*", ""},
		})
		if en.Patterns[2].ResourceType != "" {
			t.Fatalf("empty resource type serialized as %q", en.Patterns[2].ResourceType)
		}

		// A rejected Fetch.enable returns the original error and leaves the
		// last confirmed config untouched.
		h.c.Fail("Fetch.enable", errBoom)
		err := r.Add("https://lost.test/*", proto.NetworkResourceTypeImage, ctl.handler("lost"))
		if !errors.Is(err, errBoom) {
			t.Fatalf("enable error = %v, want %v", err, errBoom)
		}
		h.c.Pass("Fetch.enable")

		// Next operation continues from the last successful state: the rejected
		// pattern is absent and removal still works.
		if err := r.Remove("https://b.test/*"); err != nil {
			t.Fatal(err)
		}
		assertPatterns(t, h.lastEnable(""), false, [][2]string{
			{"https://a.test/*", "Script"},
			{"https://a.test/*", "XHR"},
		})
	})

	t.Run("auth first then routes", func(t *testing.T) {
		h := newHarness(t)
		r := h.b.HijackRequests()
		defer func() { _ = r.Stop() }()

		ctl := newRouteCtl()

		if err := r.SetAuthHandler(func(*proto.FetchAuthRequired) *proto.FetchAuthChallengeResponse {
			return &proto.FetchAuthChallengeResponse{
				Response: proto.FetchAuthChallengeResponseResponseProvideCredentials,
				Username: "u",
				Password: "p",
			}
		}, nil); err != nil {
			t.Fatal(err)
		}
		assertPatterns(t, h.lastEnable(""), true, [][2]string{})

		if err := r.Add("*", proto.NetworkResourceTypeDocument, ctl.handler("doc")); err != nil {
			t.Fatal(err)
		}
		assertPatterns(t, h.lastEnable(""), true, [][2]string{{"*", "Document"}})

		if err := r.Remove("*"); err != nil {
			t.Fatal(err)
		}
		assertPatterns(t, h.lastEnable(""), true, [][2]string{})

		// Rejected change rolls back: auth flag stays on.
		h.c.Fail("Fetch.enable", errBoom)
		if err := r.SetAuthHandler(nil, nil); !errors.Is(err, errBoom) {
			t.Fatalf("enable error = %v, want %v", err, errBoom)
		}
		h.c.Pass("Fetch.enable")
		assertPatterns(t, h.lastEnable(""), true, [][2]string{})
	})
}

// TestRouterAuthPaused covers requestPaused dispatch: url + resource-type
// matching, Add order, Skip reset, all-skip and no-match fallbacks, terminal
// actions and terminal failures.
func TestRouterAuthPaused(t *testing.T) {
	h := newHarness(t)
	r := h.b.HijackRequests()
	defer func() { _ = r.Stop() }()

	ctl := newRouteCtl()

	const url = "https://a.test/x"

	// Two handlers for the same url/type, then a document handler and a script
	// handler for the same url.
	if err := r.Add("https://a.test/*", proto.NetworkResourceTypeXHR, ctl.handler("A")); err != nil {
		t.Fatal(err)
	}
	if err := r.Add("https://a.test/*", proto.NetworkResourceTypeXHR, ctl.handler("B")); err != nil {
		t.Fatal(err)
	}
	if err := r.Add("*", proto.NetworkResourceTypeDocument, ctl.handler("D")); err != nil {
		t.Fatal(err)
	}
	if err := r.Add("https://a.test/*", proto.NetworkResourceTypeScript, ctl.handler("S")); err != nil {
		t.Fatal(err)
	}

	go r.Run()

	// Resource type really comes from the event: a Script event only matches S,
	// never the XHR patterns of the same url.
	ctl.set("fulfill")
	h.emitPaused("", "r-script", proto.NetworkResourceTypeScript, url)
	h.waitUntil("script route did not dispatch", func() bool {
		return h.countCommands("", "Fetch.fulfillRequest", "r-script") == 1
	})
	if calls := ctl.calls(); len(calls) != 1 || calls[0] != "S" {
		t.Fatalf("script event calls = %v, want [S]", calls)
	}

	// No matching type: exactly one untouched continueRequest, no handler call.
	h.emitPaused("", "r-image", proto.NetworkResourceTypeImage, url)
	h.waitUntil("unmatched event not continued", func() bool {
		return h.countCommands("", "Fetch.continueRequest", "r-image") == 1
	})
	if calls := ctl.calls(); len(calls) != 0 {
		t.Fatalf("unmatched event invoked handlers %v", calls)
	}
	if r, ok := h.findCommand("", "Fetch.continueRequest", "r-image"); ok {
		var p proto.FetchContinueRequest
		_ = json.Unmarshal(r.Params, &p)
		if p.URL != "" || p.Method != "" || len(p.Headers) != 0 || len(p.PostData) != 0 {
			t.Fatalf("fallback continue rewrote the request: %s", r.Params)
		}
	}

	// Skip: A skips, B must see Skip reset to false and then skip as well, so
	// the untouched fallback continue is sent once.
	ctl.set("skip")
	h.emitPaused("", "r-skip", proto.NetworkResourceTypeXHR, url)
	calls := ctl.waitCalls(t, 2)
	if calls[0] != "A" || calls[1] != "B" {
		t.Fatalf("skip calls = %v, want [A B]", calls)
	}
	seen := []bool{<-ctl.skipSeen, <-ctl.skipSeen}
	if seen[0] || seen[1] {
		t.Fatalf("Skip must be cleared before each call, got %v", seen)
	}
	h.waitUntil("all-skip event not continued", func() bool {
		return h.countCommands("", "Fetch.continueRequest", "r-skip") == 1
	})
	if h.countCommands("", "Fetch.fulfillRequest", "r-skip") != 0 ||
		h.countCommands("", "Fetch.failRequest", "r-skip") != 0 {
		t.Fatal("all-skip event sent a terminal command other than continueRequest")
	}

	// First Fulfill terminates dispatch: B and later handlers are not called.
	ctl.set("fulfill")
	h.emitPaused("", "r-fulfill", proto.NetworkResourceTypeXHR, url)
	h.waitUntil("fulfill not sent", func() bool {
		return h.countCommands("", "Fetch.fulfillRequest", "r-fulfill") == 1
	})
	h.waitUntil("handler channel settled", func() bool { return len(ctl.log) == 1 })
	if calls := ctl.calls(); len(calls) != 1 || calls[0] != "A" {
		t.Fatalf("fulfill calls = %v, want [A]", calls)
	}
	if h.countCommands("", "Fetch.continueRequest", "r-fulfill") != 0 {
		t.Fatal("fulfill was followed by a continueRequest")
	}

	// Fail terminates dispatch as well.
	ctl.set("fail")
	h.emitPaused("", "r-fail", proto.NetworkResourceTypeXHR, url)
	h.waitUntil("fail not sent", func() bool {
		return h.countCommands("", "Fetch.failRequest", "r-fail") == 1
	})
	h.waitUntil("handler channel settled", func() bool { return len(ctl.log) == 1 })
	if calls := ctl.calls(); len(calls) != 1 || calls[0] != "A" {
		t.Fatalf("fail calls = %v, want [A]", calls)
	}

	// A rejected terminal command reports once through Hijack.OnError and no
	// alternative terminal command is attempted.
	ctl.set("fulfill")
	h.c.Fail("Fetch.fulfillRequest", errBoom)
	h.emitPaused("", "r-err", proto.NetworkResourceTypeXHR, url)
	select {
	case err := <-ctl.errLog:
		if !errors.Is(err, errBoom) {
			t.Fatalf("OnError = %v, want %v", err, errBoom)
		}
	case <-time.After(testTimeout):
		t.Fatal("OnError not invoked")
	}
	if len(ctl.errLog) != 0 {
		t.Fatal("OnError invoked more than once")
	}
	if h.countCommands("", "Fetch.continueRequest", "r-err") != 0 ||
		h.countCommands("", "Fetch.failRequest", "r-err") != 0 {
		t.Fatal("failed fulfill was followed by another terminal command")
	}
	h.c.Pass("Fetch.fulfillRequest")

	// The router stays usable after a terminal failure.
	h.emitPaused("", "r-ok", proto.NetworkResourceTypeXHR, url)
	h.waitUntil("router did not recover after terminal failure", func() bool {
		return h.countCommands("", "Fetch.fulfillRequest", "r-ok") == 1
	})
}

// TestRouterAuthChallenge covers authRequired dispatch: exclusive auth
// handling, credential and cancel answers, repeated challenges and send
// failures.
func TestRouterAuthChallenge(t *testing.T) {
	h := newHarness(t)
	r := h.b.HijackRequests()
	defer func() { _ = r.Stop() }()

	ctl := newRouteCtl()
	if err := r.Add("*", proto.NetworkResourceTypeDocument, ctl.handler("D")); err != nil {
		t.Fatal(err)
	}

	var (
		authMu       sync.Mutex
		authCalls    int
		nextResponse *proto.FetchAuthChallengeResponse
		authErr      = make(chan error, 8)
	)
	setAuth := func(resp *proto.FetchAuthChallengeResponse) {
		authMu.Lock()
		nextResponse = resp
		authMu.Unlock()
	}
	if err := r.SetAuthHandler(func(e *proto.FetchAuthRequired) *proto.FetchAuthChallengeResponse {
		authMu.Lock()
		authCalls++
		resp := nextResponse
		authMu.Unlock()

		return resp
	}, func(err error) { authErr <- err }); err != nil {
		t.Fatal(err)
	}

	go r.Run()

	const url = "https://a.test/secret"

	// ProvideCredentials: response, username and password are preserved.
	provided := &proto.FetchAuthChallengeResponse{
		Response: proto.FetchAuthChallengeResponseResponseProvideCredentials,
		Username: "user",
		Password: "secret",
	}
	setAuth(provided)
	h.emitAuth("", "a-1", proto.NetworkResourceTypeDocument, url)
	h.waitUntil("credential answer not sent", func() bool {
		return h.countCommands("", "Fetch.continueWithAuth", "a-1") == 1
	})
	if calls := ctl.calls(); len(calls) != 0 {
		t.Fatalf("auth event invoked normal handlers %v", calls)
	}
	if h.countMethod("", "Fetch.continueRequest") != 0 {
		t.Fatal("auth event sent a Fetch.continueRequest")
	}
	answer := h.authAnswer("", "a-1")
	if answer.Response != proto.FetchAuthChallengeResponseResponseProvideCredentials ||
		answer.Username != "user" || answer.Password != "secret" {
		t.Fatalf("auth answer not preserved: %+v", answer)
	}
	// The caller's object must not be written back.
	if provided.Response != proto.FetchAuthChallengeResponseResponseProvideCredentials ||
		provided.Username != "user" || provided.Password != "secret" {
		t.Fatal("returned challenge response was mutated")
	}

	// nil answer becomes CancelAuth, without credentials.
	setAuth(nil)
	h.emitAuth("", "a-2", proto.NetworkResourceTypeDocument, url)
	h.waitUntil("cancel answer not sent", func() bool {
		return h.countCommands("", "Fetch.continueWithAuth", "a-2") == 1
	})
	answer = h.authAnswer("", "a-2")
	if answer.Response != proto.FetchAuthChallengeResponseResponseCancelAuth ||
		answer.Username != "" || answer.Password != "" {
		t.Fatalf("nil answer did not become CancelAuth: %+v", answer)
	}

	// Repeated challenges with the same request id: every challenge is handed
	// to the handler independently and answered independently.
	setAuth(provided)
	h.emitAuth("", "dup", proto.NetworkResourceTypeDocument, url)
	setAuth(nil)
	h.emitAuth("", "dup", proto.NetworkResourceTypeDocument, url)
	h.waitUntil("second duplicate challenge not answered", func() bool {
		return h.countCommands("", "Fetch.continueWithAuth", "dup") == 2
	})
	authMu.Lock()
	if authCalls != 4 { // a-1, a-2, dup, dup
		t.Fatalf("auth handler calls = %d, want 4", authCalls)
	}
	authMu.Unlock()

	// Send failure: onError gets the original error once, no retry, no default
	// credentials and no continueRequest fallback.
	setAuth(provided)
	h.c.Fail("Fetch.continueWithAuth", errBoom)
	h.emitAuth("", "a-err", proto.NetworkResourceTypeDocument, url)
	select {
	case err := <-authErr:
		if !errors.Is(err, errBoom) {
			t.Fatalf("auth onError = %v, want %v", err, errBoom)
		}
	case <-time.After(testTimeout):
		t.Fatal("auth onError not invoked")
	}
	if len(authErr) != 0 {
		t.Fatal("auth onError invoked more than once")
	}
	if h.countCommands("", "Fetch.continueWithAuth", "a-err") != 1 {
		t.Fatal("failed auth answer was retried")
	}
	answer = h.authAnswer("", "a-err")
	if answer.Response == proto.FetchAuthChallengeResponseResponseDefault {
		t.Fatal("failed auth answer fell back to Default credentials")
	}
	if h.countCommands("", "Fetch.continueRequest", "a-err") != 0 {
		t.Fatal("failed auth event was continued as a normal request")
	}
	h.c.Pass("Fetch.continueWithAuth")

	// A blocked answer must not stop other challenges from dispatching: while
	// the first command is held behind the gate, the second challenge must
	// still reach the handler and queue its own answer.
	releaseGate := h.c.Gate("Fetch.continueWithAuth")
	h.emitAuth("", "block-1", proto.NetworkResourceTypeDocument, url)
	h.emitAuth("", "block-2", proto.NetworkResourceTypeDocument, url)
	h.waitUntil("second challenge did not dispatch while the first blocked", func() bool {
		authMu.Lock()
		n := authCalls
		authMu.Unlock()
		return n == 7 // a-1, a-2, dup, dup, a-err, block-1, block-2
	})
	if n := h.countCommands("", "Fetch.continueWithAuth", "block-1") +
		h.countCommands("", "Fetch.continueWithAuth", "block-2"); n != 0 {
		t.Fatalf("gated commands recorded before release: %d", n)
	}

	releaseGate()
	h.waitUntil("blocked challenges not answered after release", func() bool {
		return h.countCommands("", "Fetch.continueWithAuth", "block-1") == 1 &&
			h.countCommands("", "Fetch.continueWithAuth", "block-2") == 1
	})
}

// TestRouterAuthSessions covers session ownership: page routers only see their
// own session, the browser router fans out across sessions, and every command
// returns to the session that produced the event.
func TestRouterAuthSessions(t *testing.T) {
	h := newHarness(t)

	// Two page routers on different sessions plus a browser-level router.
	pageA := h.b.PageFromSession("page-A")
	pageB := h.b.PageFromSession("page-B")

	ctlA := newRouteCtl()
	ctlB := newRouteCtl()

	routerA := pageA.HijackRequests()
	routerB := pageB.HijackRequests()
	defer func() { _ = routerA.Stop() }()
	defer func() { _ = routerB.Stop() }()

	if err := routerA.Add("*", "", ctlA.handler("A")); err != nil {
		t.Fatal(err)
	}
	if err := routerB.Add("*", "", ctlB.handler("B")); err != nil {
		t.Fatal(err)
	}

	authA := make(chan string, 8)
	if err := routerA.SetAuthHandler(func(e *proto.FetchAuthRequired) *proto.FetchAuthChallengeResponse {
		authA <- string(e.RequestID)
		return &proto.FetchAuthChallengeResponse{
			Response: proto.FetchAuthChallengeResponseResponseProvideCredentials,
			Username: "a",
			Password: "a",
		}
	}, nil); err != nil {
		t.Fatal(err)
	}

	go routerA.Run()
	go routerB.Run()

	// Page router only receives its own session's events.
	h.emitPaused("page-A", "p1", proto.NetworkResourceTypeDocument, "https://a.test/")
	h.waitUntil("page-A paused event not handled", func() bool {
		return h.countCommands("page-A", "Fetch.continueRequest", "p1") == 1
	})
	h.emitPaused("page-B", "p2", proto.NetworkResourceTypeDocument, "https://b.test/")
	h.waitUntil("page-B paused event not handled", func() bool {
		return h.countCommands("page-B", "Fetch.continueRequest", "p2") == 1
	})
	if calls := ctlA.calls(); len(calls) != 1 || calls[0] != "A" {
		t.Fatalf("routerA calls = %v, want [A]", calls)
	}
	if calls := ctlB.calls(); len(calls) != 1 || calls[0] != "B" {
		t.Fatalf("routerB calls = %v, want [B]", calls)
	}
	// Commands never leak to the wrong session.
	if h.countCommands("page-B", "Fetch.continueRequest", "p1") != 0 ||
		h.countCommands("page-A", "Fetch.continueRequest", "p2") != 0 {
		t.Fatal("command sent to a session other than the event origin")
	}

	// Auth on page-A is answered on page-A only.
	h.emitAuth("page-A", "auth1", proto.NetworkResourceTypeDocument, "https://a.test/")
	select {
	case id := <-authA:
		if id != "auth1" {
			t.Fatalf("auth handler got id %q", id)
		}
	case <-time.After(testTimeout):
		t.Fatal("page-A auth handler not invoked")
	}
	h.waitUntil("page-A auth answer not sent", func() bool {
		return h.countCommands("page-A", "Fetch.continueWithAuth", "auth1") == 1
	})
	if h.countCommands("page-B", "Fetch.continueWithAuth", "auth1") != 0 {
		t.Fatal("auth answer leaked to page-B")
	}

	// Browser router fans out to every session, including the empty session.
	root := h.b.HijackRequests()
	defer func() { _ = root.Stop() }()
	rootCtl := newRouteCtl()
	if err := root.Add("*", "", rootCtl.handler("root")); err != nil {
		t.Fatal(err)
	}
	rootAuth := make(chan string, 8)
	if err := root.SetAuthHandler(func(e *proto.FetchAuthRequired) *proto.FetchAuthChallengeResponse {
		rootAuth <- string(e.RequestID)
		return nil // CancelAuth
	}, nil); err != nil {
		t.Fatal(err)
	}
	go root.Run()

	// The same request id in two sessions must be handled independently, and
	// each command returns to its originating session.
	h.emitPaused("s1", "same-id", proto.NetworkResourceTypeXHR, "https://x.test/")
	h.emitPaused("s2", "same-id", proto.NetworkResourceTypeXHR, "https://x.test/")
	h.waitUntil("s1 paused not handled by root", func() bool {
		return h.countCommands("s1", "Fetch.continueRequest", "same-id") == 1
	})
	h.waitUntil("s2 paused not handled by root", func() bool {
		return h.countCommands("s2", "Fetch.continueRequest", "same-id") == 1
	})

	// Empty session events go back to the empty session.
	h.emitAuth("", "root-auth", proto.NetworkResourceTypeDocument, "https://x.test/")
	h.waitUntil("empty-session auth not answered", func() bool {
		return h.countCommands("", "Fetch.continueWithAuth", "root-auth") == 1
	})
	h.emitAuth("s2", "s2-auth", proto.NetworkResourceTypeDocument, "https://x.test/")
	h.waitUntil("s2 auth not answered on s2", func() bool {
		return h.countCommands("s2", "Fetch.continueWithAuth", "s2-auth") == 1
	})
	if h.countCommands("", "Fetch.continueWithAuth", "s2-auth") != 0 {
		t.Fatal("s2 auth answer was sent to the browser session")
	}

	// Fetch.enable itself lands on each router's own session.
	var hasA, hasB, hasRoot bool
	for _, r := range h.c.Records() {
		if r.Method != "Fetch.enable" {
			continue
		}
		switch r.Session {
		case "page-A":
			hasA = true
		case "page-B":
			hasB = true
		case "":
			hasRoot = true
		}
	}
	if !hasA || !hasB || !hasRoot {
		t.Fatalf("Fetch.enable sessions missing: A=%v B=%v root=%v", hasA, hasB, hasRoot)
	}
}

// TestRouterAuthFailure covers recovery after rejected configuration and
// terminal commands, plus concurrent dispatch behind an answer gate.
func TestRouterAuthFailure(t *testing.T) {
	h := newHarness(t)
	r := h.b.HijackRequests()
	defer func() { _ = r.Stop() }()

	ctl := newRouteCtl()

	// Configuration rejected before anything is confirmed.
	h.c.Fail("Fetch.enable", errBoom)
	if err := r.Add("https://lost.test/*", "", ctl.handler("lost")); !errors.Is(err, errBoom) {
		t.Fatalf("Add error = %v, want %v", err, errBoom)
	}
	if err := r.SetAuthHandler(func(*proto.FetchAuthRequired) *proto.FetchAuthChallengeResponse {
		return nil
	}, nil); !errors.Is(err, errBoom) {
		t.Fatalf("SetAuthHandler error = %v, want %v", err, errBoom)
	}
	h.c.Pass("Fetch.enable")

	// Confirmed configuration after recovery: only the new pattern is active.
	if err := r.Add("https://keep.test/*", proto.NetworkResourceTypeXHR, ctl.handler("keep")); err != nil {
		t.Fatal(err)
	}
	assertPatterns(t, h.lastEnable(""), false, [][2]string{{"https://keep.test/*", "XHR"}})

	go r.Run()

	// Rejected pattern never became active: a paused event of that url falls
	// through to the untouched continueRequest.
	h.emitPaused("", "lost-1", proto.NetworkResourceTypeDocument, "https://lost.test/x")
	h.waitUntil("rejected pattern still routed events", func() bool {
		return h.countCommands("", "Fetch.continueRequest", "lost-1") == 1
	})
	if calls := ctl.calls(); len(calls) != 0 {
		t.Fatalf("rejected handler invoked: %v", calls)
	}

	// Terminal command rejected: OnError once, then later events still dispatch.
	ctl.set("continue")
	h.c.Fail("Fetch.continueRequest", errBoom)
	h.emitPaused("", "keep-err", proto.NetworkResourceTypeXHR, "https://keep.test/x")
	select {
	case err := <-ctl.errLog:
		if !errors.Is(err, errBoom) {
			t.Fatalf("OnError = %v, want %v", err, errBoom)
		}
	case <-time.After(testTimeout):
		t.Fatal("terminal OnError not invoked")
	}
	if len(ctl.errLog) != 0 {
		t.Fatal("terminal OnError invoked more than once")
	}
	h.c.Pass("Fetch.continueRequest")
	h.emitPaused("", "keep-ok", proto.NetworkResourceTypeXHR, "https://keep.test/x")
	h.waitUntil("router did not recover after continue failure", func() bool {
		return h.countCommands("", "Fetch.continueRequest", "keep-ok") == 1
	})

	// Answer gate: a blocked terminal command must not stall other events.
	ctl.set("fulfill")
	release := h.c.Gate("Fetch.fulfillRequest")
	h.emitPaused("", "gate-1", proto.NetworkResourceTypeXHR, "https://keep.test/x")
	h.emitPaused("", "gate-2", proto.NetworkResourceTypeXHR, "https://keep.test/x")
	h.waitUntil("gated events did not both reach handlers", func() bool {
		return len(ctl.log) >= 2
	})
	if h.countCommands("", "Fetch.fulfillRequest", "gate-1") != 0 ||
		h.countCommands("", "Fetch.fulfillRequest", "gate-2") != 0 {
		t.Fatal("gated fulfill recorded before release")
	}
	release()
	h.waitUntil("gate-1 not answered", func() bool {
		return h.countCommands("", "Fetch.fulfillRequest", "gate-1") == 1
	})
	h.waitUntil("gate-2 not answered", func() bool {
		return h.countCommands("", "Fetch.fulfillRequest", "gate-2") == 1
	})
}
