package agouti_test

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"reflect"
	"strings"
	"sync"
	"testing"

	"github.com/sclevine/agouti"
	"github.com/sclevine/agouti/api"
)

// The tests in this file exercise the legacy JSON Wire Protocol and the W3C
// WebDriver protocol against the same in-memory RoundTripper. No driver or
// browser is required.

type dialectRequest struct {
	Method string
	Path   string
	Body   []byte
}

type dialectTransport struct {
	mu       sync.Mutex
	requests []dialectRequest
	respond  func(dialectRequest) (int, string)
}

func (t *dialectTransport) RoundTrip(request *http.Request) (*http.Response, error) {
	var body []byte
	if request.Body != nil {
		var err error
		body, err = io.ReadAll(request.Body)
		if err != nil {
			return nil, err
		}
	}
	recorded := dialectRequest{request.Method, request.URL.Path, body}
	t.mu.Lock()
	t.requests = append(t.requests, recorded)
	t.mu.Unlock()

	status, responseBody := t.respond(recorded)
	return &http.Response{
		StatusCode: status,
		Header:     http.Header{"Content-Type": []string{"application/json"}},
		Body:       io.NopCloser(strings.NewReader(responseBody)),
		Request:    request,
	}, nil
}

func (t *dialectTransport) log() []string {
	t.mu.Lock()
	defer t.mu.Unlock()
	log := make([]string, len(t.requests))
	for index, request := range t.requests {
		log[index] = request.Method + " " + request.Path
	}
	return log
}

func (t *dialectTransport) bodies(method, path string) []string {
	t.mu.Lock()
	defer t.mu.Unlock()
	var bodies []string
	for _, request := range t.requests {
		if request.Method == method && request.Path == path {
			bodies = append(bodies, string(request.Body))
		}
	}
	return bodies
}

func (t *dialectTransport) count(method, path string) int {
	return len(t.bodies(method, path))
}

func newDialectPage(t *testing.T, transport *dialectTransport, options ...agouti.Option) *agouti.Page {
	t.Helper()
	options = append(options, agouti.HTTPClient(&http.Client{Transport: transport}))
	page, err := agouti.NewPage("http://driver.invalid", options...)
	if err != nil {
		t.Fatalf("failed to create page: %v", err)
	}
	return page
}

func jsonSnapshot(t *testing.T, value interface{}) string {
	t.Helper()
	encoded, err := json.Marshal(value)
	if err != nil {
		t.Fatalf("failed to snapshot value: %v", err)
	}
	return string(encoded)
}

func nullValue(w3c bool) string {
	if w3c {
		return `{"value":null}`
	}
	return `{"status":0,"value":null}`
}

func valueResponse(w3c bool, value string) string {
	if w3c {
		return fmt.Sprintf(`{"value":%s}`, value)
	}
	return fmt.Sprintf(`{"status":0,"value":%s}`, value)
}

func elementBody(w3c bool, id string) string {
	if w3c {
		return fmt.Sprintf(`{"element-6066-11e4-a52e-4f735466cecf":%q}`, id)
	}
	return fmt.Sprintf(`{"ELEMENT":%q}`, id)
}

// formResponder serves the endpoints used by the form flow for both sessions.
func formResponder(request dialectRequest) (int, string) {
	if request.Path == "/session" {
		if bytes.Contains(request.Body, []byte("desiredCapabilities")) {
			return 200, `{"sessionId":"legacy-id","status":0,"value":{}}`
		}
		return 200, `{"value":{"sessionId":"w3c-id","capabilities":{"browserName":"chrome","acceptInsecureCerts":true,"goog:chromeOptions":{"args":["--headless"]}}}}`
	}

	w3c := strings.HasPrefix(request.Path, "/session/w3c-id")
	sessionPrefix := "/session/legacy-id"
	if w3c {
		sessionPrefix = "/session/w3c-id"
	}
	suffix := strings.TrimPrefix(strings.TrimPrefix(request.Path, sessionPrefix), "/")

	switch {
	case suffix == "execute" || suffix == "execute/sync":
		return 200, valueResponse(w3c, `42`)
	case suffix == "alert_text" || suffix == "alert/text":
		return 200, valueResponse(w3c, fmt.Sprintf("%q", map[bool]string{true: "w3c popup", false: "legacy popup"}[w3c]))
	case suffix == "accept_alert" || suffix == "alert/accept":
		return 200, nullValue(w3c)
	case suffix == "dismiss_alert" || suffix == "alert/dismiss":
		return 200, nullValue(w3c)
	case suffix == "window_handles" || suffix == "window/handles":
		return 200, valueResponse(w3c, `["w1","w2"]`)
	case suffix == "window_handle" || suffix == "window":
		if request.Method == "GET" {
			return 200, valueResponse(w3c, `"w1"`)
		}
		return 200, nullValue(w3c)
	case suffix == "timeouts" || suffix == "timeouts/implicit_wait" || suffix == "timeouts/async_script":
		return 200, nullValue(w3c)
	case suffix == "elements":
		return 200, valueResponse(w3c, "["+elementBody(w3c, "el")+"]")
	case suffix == "element/active":
		return 200, valueResponse(w3c, elementBody(w3c, "el"))
	case suffix == "element/el/equals/el":
		return 200, valueResponse(w3c, `true`)
	}
	return 404, `{"value":{"error":"unknown command","message":"unexpected endpoint ` + suffix + `"}}`
}

func TestDialectInterleavedFormOperations(t *testing.T) {
	transport := &dialectTransport{respond: formResponder}
	legacy := newDialectPage(t, transport)
	w3c := newDialectPage(t, transport, agouti.W3C(true), agouti.Browser("chrome"))

	runScript := func(page *agouti.Page) int {
		var result int
		if err := page.RunScript("return 42", nil, &result); err != nil {
			t.Fatalf("failed to run script: %v", err)
		}
		return result
	}

	if result := runScript(legacy); result != 42 {
		t.Fatalf("legacy script result = %d", result)
	}
	if result := runScript(w3c); result != 42 {
		t.Fatalf("w3c script result = %d", result)
	}

	if text, err := legacy.PopupText(); err != nil || text != "legacy popup" {
		t.Fatalf("legacy popup text = %q, %v", text, err)
	}
	if text, err := w3c.PopupText(); err != nil || text != "w3c popup" {
		t.Fatalf("w3c popup text = %q, %v", text, err)
	}
	if err := legacy.ConfirmPopup(); err != nil {
		t.Fatal(err)
	}
	if err := w3c.ConfirmPopup(); err != nil {
		t.Fatal(err)
	}
	if err := legacy.CancelPopup(); err != nil {
		t.Fatal(err)
	}
	if err := w3c.CancelPopup(); err != nil {
		t.Fatal(err)
	}

	if count, err := legacy.WindowCount(); err != nil || count != 2 {
		t.Fatalf("legacy window count = %d, %v", count, err)
	}
	if count, err := w3c.WindowCount(); err != nil || count != 2 {
		t.Fatalf("w3c window count = %d, %v", count, err)
	}
	if err := legacy.NextWindow(); err != nil {
		t.Fatal(err)
	}
	if err := w3c.NextWindow(); err != nil {
		t.Fatal(err)
	}

	if err := legacy.SetImplicitWait(125); err != nil {
		t.Fatal(err)
	}
	if err := w3c.SetImplicitWait(125); err != nil {
		t.Fatal(err)
	}
	if err := legacy.SetPageLoad(250); err != nil {
		t.Fatal(err)
	}
	if err := w3c.SetPageLoad(250); err != nil {
		t.Fatal(err)
	}
	if err := legacy.SetScriptTimeout(375); err != nil {
		t.Fatal(err)
	}
	if err := w3c.SetScriptTimeout(375); err != nil {
		t.Fatal(err)
	}

	if active, err := legacy.Find("#field").Active(); err != nil || !active {
		t.Fatalf("legacy active = %v, %v", active, err)
	}
	if active, err := w3c.Find("#field").Active(); err != nil || !active {
		t.Fatalf("w3c active = %v, %v", active, err)
	}

	expected := []string{
		"POST /session",
		"POST /session",
		// Scripts.
		"POST /session/legacy-id/execute",
		"POST /session/w3c-id/execute/sync",
		// Popup text.
		"GET /session/legacy-id/alert_text",
		"GET /session/w3c-id/alert/text",
		// Confirm and cancel.
		"POST /session/legacy-id/accept_alert",
		"POST /session/w3c-id/alert/accept",
		"POST /session/legacy-id/dismiss_alert",
		"POST /session/w3c-id/alert/dismiss",
		// Window count.
		"GET /session/legacy-id/window_handles",
		"GET /session/w3c-id/window/handles",
		// Next window (handles, current handle, set handle).
		"GET /session/legacy-id/window_handles",
		"GET /session/legacy-id/window_handle",
		"POST /session/legacy-id/window",
		"GET /session/w3c-id/window/handles",
		"GET /session/w3c-id/window",
		"POST /session/w3c-id/window",
		// Timeouts.
		"POST /session/legacy-id/timeouts/implicit_wait",
		"POST /session/w3c-id/timeouts",
		"POST /session/legacy-id/timeouts",
		"POST /session/w3c-id/timeouts",
		"POST /session/legacy-id/timeouts/async_script",
		"POST /session/w3c-id/timeouts",
		// Active element (find, active element, compare).
		"POST /session/legacy-id/elements",
		"POST /session/legacy-id/element/active",
		"GET /session/legacy-id/element/el/equals/el",
		"POST /session/w3c-id/elements",
		"GET /session/w3c-id/element/active",
		"GET /session/w3c-id/element/el/equals/el",
	}
	if actual := transport.log(); !reflect.DeepEqual(actual, expected) {
		t.Fatalf("unexpected request sequence:\n%s\nwant:\n%s", strings.Join(actual, "\n"), strings.Join(expected, "\n"))
	}

	assertJSONEqual(t, transport.bodies("POST", "/session/w3c-id/timeouts")[0], `{"implicit":125}`)
	assertJSONEqual(t, transport.bodies("POST", "/session/w3c-id/timeouts")[1], `{"pageLoad":250}`)
	assertJSONEqual(t, transport.bodies("POST", "/session/w3c-id/timeouts")[2], `{"script":375}`)
	assertJSONEqual(t, transport.bodies("POST", "/session/legacy-id/timeouts")[0], `{"ms":250,"type":"page load"}`)
	assertJSONEqual(t, transport.bodies("POST", "/session/w3c-id/window")[0], `{"handle":"w2"}`)
	assertJSONEqual(t, transport.bodies("POST", "/session/legacy-id/window")[0], `{"name":"w2"}`)
}

func assertJSONEqual(t *testing.T, actual, expected string) {
	t.Helper()
	var actualValue, expectedValue interface{}
	if err := json.Unmarshal([]byte(actual), &actualValue); err != nil {
		t.Fatalf("invalid JSON %q: %v", actual, err)
	}
	if err := json.Unmarshal([]byte(expected), &expectedValue); err != nil {
		t.Fatalf("invalid expected JSON %q: %v", expected, err)
	}
	if !reflect.DeepEqual(actualValue, expectedValue) {
		t.Fatalf("JSON mismatch: got %s, want %s", actual, expected)
	}
}

func TestDialectW3CHandshakeCapabilities(t *testing.T) {
	transport := &dialectTransport{respond: func(request dialectRequest) (int, string) {
		return 200, `{"value":{"sessionId":"w3c-id","capabilities":{"browserName":"chrome"}}}`
	}}
	capabilities := map[string]interface{}{
		"browserName":    "chrome",
		"acceptSslCerts": false,
		"chromeOptions": map[string]interface{}{
			"args":  []interface{}{"--headless", "--no-sandbox"},
			"prefs": map[string]interface{}{"download": map[string]interface{}{"prompt_for_download": true}},
		},
	}
	before := jsonSnapshot(t, capabilities)

	session, err := api.OpenW3CWithClient("http://driver.invalid", capabilities, &http.Client{Transport: transport})
	if err != nil {
		t.Fatalf("failed to open W3C session: %v", err)
	}
	if session == nil {
		t.Fatal("expected a session")
	}
	if session.Dialect() != api.W3C {
		t.Fatalf("session dialect = %v", session.Dialect())
	}
	if after := jsonSnapshot(t, capabilities); after != before {
		t.Fatalf("capabilities were mutated:\nbefore %s\nafter  %s", before, after)
	}

	requests := transport.log()
	if !reflect.DeepEqual(requests, []string{"POST /session"}) {
		t.Fatalf("expected exactly one handshake request, got %v", requests)
	}

	var body map[string]interface{}
	if err := json.Unmarshal([]byte(transport.bodies("POST", "/session")[0]), &body); err != nil {
		t.Fatal(err)
	}
	if _, present := body["desiredCapabilities"]; present {
		t.Fatal("W3C handshake must not include desiredCapabilities")
	}
	if _, present := body["sessionId"]; present {
		t.Fatal("unexpected top-level sessionId")
	}
	capabilitiesBody, ok := body["capabilities"].(map[string]interface{})
	if !ok {
		t.Fatalf("missing capabilities object: %v", body)
	}
	alwaysMatch, ok := capabilitiesBody["alwaysMatch"].(map[string]interface{})
	if !ok {
		t.Fatalf("missing alwaysMatch object: %v", capabilitiesBody)
	}
	firstMatch, ok := capabilitiesBody["firstMatch"].([]interface{})
	if !ok || len(firstMatch) != 1 {
		t.Fatalf("firstMatch = %v", capabilitiesBody["firstMatch"])
	}
	if entry, ok := firstMatch[0].(map[string]interface{}); !ok || len(entry) != 0 {
		t.Fatalf("firstMatch[0] = %v", firstMatch[0])
	}
	if value, present := alwaysMatch["acceptInsecureCerts"]; !present || value != false {
		t.Fatalf("acceptInsecureCerts = %v", alwaysMatch["acceptInsecureCerts"])
	}
	if _, present := alwaysMatch["acceptSslCerts"]; present {
		t.Fatal("legacy acceptSslCerts must be translated")
	}
	googOptions, ok := alwaysMatch["goog:chromeOptions"].(map[string]interface{})
	if !ok {
		t.Fatalf("missing goog:chromeOptions: %v", alwaysMatch)
	}
	if args, ok := googOptions["args"].([]interface{}); !ok || len(args) != 2 || args[0] != "--headless" {
		t.Fatalf("goog:chromeOptions args = %v", googOptions["args"])
	}
	if _, present := alwaysMatch["chromeOptions"]; present {
		t.Fatal("legacy chromeOptions must be translated")
	}
	if alwaysMatch["browserName"] != "chrome" {
		t.Fatalf("browserName = %v", alwaysMatch["browserName"])
	}
}

func TestDialectW3CStandardKeysTakePrecedence(t *testing.T) {
	transport := &dialectTransport{respond: func(request dialectRequest) (int, string) {
		return 200, `{"value":{"sessionId":"w3c-id","capabilities":{}}}`
	}}
	capabilities := map[string]interface{}{
		"acceptSslCerts":      false,
		"acceptInsecureCerts": true,
		"chromeOptions":       map[string]interface{}{"args": []interface{}{"--legacy"}},
		"goog:chromeOptions":  map[string]interface{}{"args": []interface{}{"--standard"}},
	}
	before := jsonSnapshot(t, capabilities)

	if _, err := api.OpenW3CWithClient("http://driver.invalid", capabilities, &http.Client{Transport: transport}); err != nil {
		t.Fatalf("failed to open W3C session: %v", err)
	}
	if after := jsonSnapshot(t, capabilities); after != before {
		t.Fatalf("capabilities were mutated:\nbefore %s\nafter  %s", before, after)
	}

	var body struct {
		Capabilities struct {
			AlwaysMatch map[string]interface{} `json:"alwaysMatch"`
		} `json:"capabilities"`
	}
	if err := json.Unmarshal([]byte(transport.bodies("POST", "/session")[0]), &body); err != nil {
		t.Fatal(err)
	}
	alwaysMatch := body.Capabilities.AlwaysMatch
	if alwaysMatch["acceptInsecureCerts"] != true {
		t.Fatalf("standard acceptInsecureCerts should win: %v", alwaysMatch["acceptInsecureCerts"])
	}
	if _, present := alwaysMatch["acceptSslCerts"]; present {
		t.Fatal("legacy acceptSslCerts should be removed")
	}
	googOptions, ok := alwaysMatch["goog:chromeOptions"].(map[string]interface{})
	if !ok {
		t.Fatalf("goog:chromeOptions = %v", alwaysMatch["goog:chromeOptions"])
	}
	args, _ := googOptions["args"].([]interface{})
	if len(args) != 1 || args[0] != "--standard" {
		t.Fatalf("standard goog:chromeOptions should win: %v", args)
	}
	if _, present := alwaysMatch["chromeOptions"]; present {
		t.Fatal("legacy chromeOptions should be removed")
	}
}

func TestDialectW3CHandshakeFailures(t *testing.T) {
	cases := []struct {
		name          string
		status        int
		body          string
		wantCode      string
		wantMessage   string
		wantInMessage string
	}{
		{
			name:        "protocol error",
			status:      500,
			body:        `{"value":{"error":"session not created","message":"no browser","stacktrace":"stack"}}`,
			wantCode:    "session not created",
			wantMessage: "no browser",
		},
		{
			name:          "invalid JSON",
			status:        500,
			body:          `not json`,
			wantInMessage: "HTTP 500",
		},
		{
			name:          "missing error fields",
			status:        500,
			body:          `{"value":{"error":"session not created"}}`,
			wantInMessage: "HTTP 500",
		},
		{
			name:          "missing session id",
			status:        200,
			body:          `{"value":{"capabilities":{}}}`,
			wantInMessage: "session",
		},
		{
			name:          "capabilities not an object",
			status:        200,
			body:          `{"value":{"sessionId":"w3c-id","capabilities":[]}}`,
			wantInMessage: "session",
		},
	}

	for _, testCase := range cases {
		t.Run(testCase.name, func(t *testing.T) {
			transport := &dialectTransport{respond: func(request dialectRequest) (int, string) {
				return testCase.status, testCase.body
			}}
			session, err := api.OpenW3CWithClient("http://driver.invalid", nil, &http.Client{Transport: transport})
			if err == nil {
				t.Fatal("expected an error")
			}
			if session != nil {
				t.Fatal("no session should be returned on failure")
			}
			var protocolError *api.ProtocolError
			if !errors.As(err, &protocolError) {
				t.Fatalf("expected api.ProtocolError, got %T: %v", err, err)
			}
			if protocolError.HTTPStatus != testCase.status {
				t.Fatalf("HTTPStatus = %d", protocolError.HTTPStatus)
			}
			if testCase.wantCode != "" && protocolError.Code != testCase.wantCode {
				t.Fatalf("Code = %q", protocolError.Code)
			}
			if testCase.wantMessage != "" && protocolError.Message != testCase.wantMessage {
				t.Fatalf("Message = %q", protocolError.Message)
			}
			if testCase.wantInMessage != "" && !strings.Contains(protocolError.Message, testCase.wantInMessage) {
				t.Fatalf("Message %q does not contain %q", protocolError.Message, testCase.wantInMessage)
			}
			if testCase.name == "protocol error" && protocolError.Stacktrace != "stack" {
				t.Fatalf("Stacktrace = %q", protocolError.Stacktrace)
			}
		})
	}
}

func TestDialectLegacyNestedSessionID(t *testing.T) {
	transport := &dialectTransport{respond: func(request dialectRequest) (int, string) {
		return 200, `{"value":{"sessionId":"nested-id"}}`
	}}
	session, err := api.OpenWithClient("http://driver.invalid", nil, &http.Client{Transport: transport})
	if err != nil {
		t.Fatalf("failed to open legacy session: %v", err)
	}
	if session.Dialect() != api.JSONWire {
		t.Fatalf("dialect = %v", session.Dialect())
	}
}

func TestDialectLegacyHandshakeStatusFailure(t *testing.T) {
	transport := &dialectTransport{respond: func(request dialectRequest) (int, string) {
		return 200, `{"status":13,"value":{"message":"{\"errorMessage\":\"session not created\"}"}}`
	}}
	session, err := api.OpenWithClient("http://driver.invalid", nil, &http.Client{Transport: transport})
	if err == nil || session != nil {
		t.Fatal("expected a failed, empty legacy session")
	}
	var protocolError *api.ProtocolError
	if !errors.As(err, &protocolError) {
		t.Fatalf("expected api.ProtocolError, got %T: %v", err, err)
	}
	if protocolError.Message != "session not created" {
		t.Fatalf("Message = %q", protocolError.Message)
	}
}

func TestDialectW3CEnvelopeMissingValue(t *testing.T) {
	transport := &dialectTransport{respond: func(request dialectRequest) (int, string) {
		switch request.Path {
		case "/session":
			return 200, `{"value":{"sessionId":"w3c-id","capabilities":{}}}`
		}
		return 200, `{"notvalue":{}}`
	}}
	session, err := api.OpenW3CWithClient("http://driver.invalid", nil, &http.Client{Transport: transport})
	if err != nil {
		t.Fatalf("failed to open session: %v", err)
	}
	var result interface{}
	err = session.Execute("return 1", nil, &result)
	if err == nil {
		t.Fatal("expected an envelope error")
	}
	var protocolError *api.ProtocolError
	if !errors.As(err, &protocolError) {
		t.Fatalf("expected api.ProtocolError in chain, got %T: %v", err, err)
	}
	if protocolError.HTTPStatus != 200 {
		t.Fatalf("HTTPStatus = %d", protocolError.HTTPStatus)
	}
	if !strings.Contains(protocolError.Message, "HTTP 200") {
		t.Fatalf("Message %q should describe the status", protocolError.Message)
	}
	if !strings.Contains(protocolError.Message, "notvalue") {
		t.Fatalf("Message %q should include a response summary", protocolError.Message)
	}
}

func TestDialectW3CSessionCapabilitiesShape(t *testing.T) {
	transport := &dialectTransport{respond: func(request dialectRequest) (int, string) {
		return 200, `{"value":{"sessionId":"w3c-id","capabilities":{"browserName":"chrome"}}}`
	}}
	client := &http.Client{Transport: transport}
	session, err := api.OpenW3CWithClient("http://driver.invalid", map[string]interface{}{"browserName": "chrome"}, client)
	if err != nil {
		t.Fatalf("failed to open session: %v", err)
	}
	if session.Dialect() != api.W3C {
		t.Fatalf("dialect = %v", session.Dialect())
	}
	if count := transport.count("POST", "/session"); count != 1 {
		t.Fatalf("expected exactly one handshake, got %d", count)
	}
}

func TestDialectW3CWindowHandlesRequestShape(t *testing.T) {
	transport := &dialectTransport{respond: formResponder}
	page := newDialectPage(t, transport, agouti.W3C(true))

	if _, err := page.WindowCount(); err != nil {
		t.Fatalf("window count failed: %v", err)
	}
	if err := page.NextWindow(); err != nil {
		t.Fatalf("next window failed: %v", err)
	}
	assertJSONEqual(t, transport.bodies("POST", "/session/w3c-id/window")[0], `{"handle":"w2"}`)

	legacy := newDialectPage(t, transport)
	if _, err := legacy.WindowCount(); err != nil {
		t.Fatalf("legacy window count failed: %v", err)
	}
	if err := legacy.NextWindow(); err != nil {
		t.Fatalf("legacy next window failed: %v", err)
	}
	assertJSONEqual(t, transport.bodies("POST", "/session/legacy-id/window")[0], `{"name":"w2"}`)
	if count := transport.count("GET", "/session/w3c-id/window"); count != 1 {
		t.Fatalf("GET window count = %d", count)
	}
}

func TestDialectW3CGetActiveElementUsesGET(t *testing.T) {
	transport := &dialectTransport{respond: formResponder}
	page := newDialectPage(t, transport, agouti.W3C(true))
	if active, err := page.Find("#field").Active(); err != nil || !active {
		t.Fatalf("active = %v, %v", active, err)
	}
	if count := transport.count("GET", "/session/w3c-id/element/active"); count != 1 {
		t.Fatalf("GET active element count = %d", count)
	}
	if count := transport.count("POST", "/session/w3c-id/element/active"); count != 0 {
		t.Fatalf("unexpected POST active element count = %d", count)
	}
}

func TestDialectUnknownCommandDoesNotSwitchDialect(t *testing.T) {
	transport := &dialectTransport{respond: func(request dialectRequest) (int, string) {
		switch request.Path {
		case "/session":
			return 200, `{"sessionId":"legacy-id","status":0}`
		case "/session/legacy-id/timeouts/async_script":
			return 200, `{"status":0,"value":null}`
		}
		return 404, `{"value":{"error":"unknown command","message":"unknown"}}`
	}}
	page := newDialectPage(t, transport)
	if err := page.SetScriptTimeout(300); err != nil {
		t.Fatalf("legacy timeout failed: %v", err)
	}
	// A legacy unknown-command response must be reported, not retried as W3C.
	var result interface{}
	if err := page.RunScript("return 1", nil, &result); err == nil {
		t.Fatal("expected the unknown command to be reported")
	}
	if count := transport.count("POST", "/session/legacy-id/execute/sync"); count != 0 {
		t.Fatalf("unexpected fallback request: %d", count)
	}
	if count := transport.count("POST", "/session/legacy-id/timeouts"); count != 0 {
		t.Fatalf("unexpected fallback timeouts request: %d", count)
	}
}

func TestDialectW3CUnknownCommandDoesNotSwitchDialect(t *testing.T) {
	transport := &dialectTransport{respond: func(request dialectRequest) (int, string) {
		switch request.Path {
		case "/session":
			return 200, `{"value":{"sessionId":"w3c-id","capabilities":{}}}`
		}
		return 404, `{"value":{"error":"unknown command","message":"unknown"}}`
	}}
	page := newDialectPage(t, transport, agouti.W3C(true))
	var result interface{}
	if err := page.RunScript("return 1", nil, &result); err == nil {
		t.Fatal("expected the unknown command to be reported")
	}
	if count := transport.count("POST", "/session/w3c-id/execute"); count != 0 {
		t.Fatalf("unexpected legacy fallback: %d", count)
	}
	if count := transport.count("POST", "/session/w3c-id/execute/sync"); count != 1 {
		t.Fatalf("execute/sync count = %d", count)
	}
}

func TestDialectLegacyTimeoutBodiesUnchanged(t *testing.T) {
	transport := &dialectTransport{respond: formResponder}
	page := newDialectPage(t, transport)

	if err := page.SetImplicitWait(125); err != nil {
		t.Fatal(err)
	}
	if err := page.SetPageLoad(250); err != nil {
		t.Fatal(err)
	}
	if err := page.SetScriptTimeout(375); err != nil {
		t.Fatal(err)
	}
	assertJSONEqual(t, transport.bodies("POST", "/session/legacy-id/timeouts/implicit_wait")[0], `{"ms":125}`)
	assertJSONEqual(t, transport.bodies("POST", "/session/legacy-id/timeouts")[0], `{"ms":250,"type":"page load"}`)
	assertJSONEqual(t, transport.bodies("POST", "/session/legacy-id/timeouts/async_script")[0], `{"ms":375}`)
}

func TestDialectW3CAlertSideEffectsNotRetried(t *testing.T) {
	transport := &dialectTransport{respond: func(request dialectRequest) (int, string) {
		switch {
		case request.Path == "/session":
			return 200, `{"value":{"sessionId":"w3c-id","capabilities":{}}}`
		case strings.HasSuffix(request.Path, "/alert/accept"):
			return 500, `{"value":{"error":"no such alert","message":"no alert"}}`
		}
		return 200, `{"value":null}`
	}}
	page := newDialectPage(t, transport, agouti.W3C(true))
	var result interface{}
	if err := page.ConfirmPopup(); err == nil {
		t.Fatal("expected confirm popup to fail")
	}
	var protocolError *api.ProtocolError
	err := page.ConfirmPopup()
	if !errors.As(err, &protocolError) || protocolError.Code != "no such alert" {
		t.Fatalf("expected protocol error in chain, got %v", err)
	}
	if count := transport.count("POST", "/session/w3c-id/alert/accept"); count != 2 {
		t.Fatalf("accept count = %d", count)
	}
	if count := transport.count("POST", "/session/w3c-id/dismiss_alert"); count != 0 {
		t.Fatalf("unexpected legacy dismiss: %d", count)
	}
	_ = result
}

func TestDialectPageErrorContextPreserved(t *testing.T) {
	transport := &dialectTransport{respond: func(request dialectRequest) (int, string) {
		switch request.Path {
		case "/session":
			return 200, `{"value":{"sessionId":"w3c-id","capabilities":{}}}`
		}
		return 404, `{"value":{"error":"no such window","message":"gone","stacktrace":"at frame"}}`
	}}
	page := newDialectPage(t, transport, agouti.W3C(true))
	_, err := page.PopupText()
	if err == nil {
		t.Fatal("expected popup text to fail")
	}
	if !strings.Contains(err.Error(), "failed to retrieve popup text") {
		t.Fatalf("missing page context: %v", err)
	}
	var protocolError *api.ProtocolError
	if !errors.As(err, &protocolError) {
		t.Fatalf("expected protocol error in chain: %v", err)
	}
	if protocolError.Code != "no such window" || protocolError.Message != "gone" ||
		protocolError.Stacktrace != "at frame" {
		t.Fatalf("unexpected protocol error: %+v", protocolError)
	}
}

func TestDialectProtocolErrorIsExportedAlias(t *testing.T) {
	transport := &dialectTransport{respond: func(request dialectRequest) (int, string) {
		switch request.Path {
		case "/session":
			return 200, `{"value":{"sessionId":"w3c-id","capabilities":{}}}`
		}
		return 500, `{"value":{"error":"unknown error","message":"oops","stacktrace":"s"}}`
	}}
	session, err := api.OpenW3CWithClient("http://driver.invalid", nil, &http.Client{Transport: transport})
	if err != nil {
		t.Fatalf("failed to open session: %v", err)
	}
	_, err = session.GetAlertText()
	var protocolError *api.ProtocolError
	if !errors.As(err, &protocolError) {
		t.Fatalf("errors.As failed: %T %v", err, err)
	}
	if protocolError.HTTPStatus != 500 {
		t.Fatalf("HTTPStatus = %d", protocolError.HTTPStatus)
	}
}

func TestDialectW3CCommandProtocolError(t *testing.T) {
	transport := &dialectTransport{respond: func(request dialectRequest) (int, string) {
		switch request.Path {
		case "/session":
			return 200, `{"value":{"sessionId":"w3c-id","capabilities":{}}}`
		case "/session/w3c-id/execute/sync":
			return 400, `{"value":{"error":"javascript error","message":"boom","stacktrace":"frame 1"}}`
		}
		return 404, `{"value":{"error":"unknown command","message":"unexpected"}}`
	}}

	page := newDialectPage(t, transport, agouti.W3C(true))
	var result interface{}
	err := page.RunScript("throw new Error('boom')", nil, &result)
	if err == nil {
		t.Fatal("expected a script error")
	}
	if !strings.Contains(err.Error(), "failed to run script") {
		t.Fatalf("missing context: %v", err)
	}
	var protocolError *api.ProtocolError
	if !errors.As(err, &protocolError) {
		t.Fatalf("expected api.ProtocolError in chain, got %T: %v", err, err)
	}
	if protocolError.HTTPStatus != 400 || protocolError.Code != "javascript error" ||
		protocolError.Message != "boom" || protocolError.Stacktrace != "frame 1" {
		t.Fatalf("unexpected protocol error: %+v", protocolError)
	}
	if count := transport.count("POST", "/session/w3c-id/execute/sync"); count != 1 {
		t.Fatalf("script was sent %d times; side effects must not be retried", count)
	}
}

func TestDialectLegacyStatusErrorOnHTTP200(t *testing.T) {
	transport := &dialectTransport{respond: func(request dialectRequest) (int, string) {
		switch request.Path {
		case "/session":
			return 200, `{"sessionId":"legacy-id","status":0}`
		}
		return 200, `{"status":7,"value":{"message":"{\"errorMessage\":\"element not visible\"}"}}`
	}}

	page := newDialectPage(t, transport)
	var result interface{}
	err := page.RunScript("return 1", nil, &result)
	if err == nil {
		t.Fatal("expected a legacy status error on HTTP 200")
	}
	var protocolError *api.ProtocolError
	if !errors.As(err, &protocolError) {
		t.Fatalf("expected api.ProtocolError in chain, got %T: %v", err, err)
	}
	if protocolError.HTTPStatus != 200 {
		t.Fatalf("HTTPStatus = %d", protocolError.HTTPStatus)
	}
	if protocolError.Message != "element not visible" {
		t.Fatalf("legacy message was not preserved: %q", protocolError.Message)
	}
	if !strings.Contains(err.Error(), "element not visible") {
		t.Fatalf("error chain lost the message: %v", err)
	}
}

func TestDialectSuccessfulScriptWithErrorKey(t *testing.T) {
	transport := &dialectTransport{respond: func(request dialectRequest) (int, string) {
		switch request.Path {
		case "/session":
			return 200, `{"value":{"sessionId":"w3c-id","capabilities":{}}}`
		}
		return 200, `{"value":{"status":"ok","error":null,"message":"not a protocol failure"}}`
	}}

	page := newDialectPage(t, transport, agouti.W3C(true))
	var result map[string]interface{}
	if err := page.RunScript("return {status: 'ok', error: null}", nil, &result); err != nil {
		t.Fatalf("a successful script object with an error key must succeed: %v", err)
	}
	if result["status"] != "ok" {
		t.Fatalf("unexpected result: %v", result)
	}

	legacyTransport := &dialectTransport{respond: func(request dialectRequest) (int, string) {
		switch request.Path {
		case "/session":
			return 200, `{"sessionId":"legacy-id","status":0}`
		}
		return 200, `{"status":0,"value":{"status":"ok","error":"not a protocol failure"}}`
	}}
	legacyPage := newDialectPage(t, legacyTransport)
	if err := legacyPage.RunScript("return {status: 'ok', error: 'x'}", nil, &result); err != nil {
		t.Fatalf("legacy successful script object must succeed: %v", err)
	}
}

func TestDialectJoinPageDialect(t *testing.T) {
	transport := &dialectTransport{respond: func(request dialectRequest) (int, string) {
		if strings.HasSuffix(request.Path, "/alert/text") {
			return 200, `{"value":"w3c text"}`
		}
		if strings.HasSuffix(request.Path, "/alert_text") {
			return 200, `{"status":0,"value":"legacy text"}`
		}
		return 200, `{"status":0,"value":null}`
	}}
	client := &http.Client{Transport: transport}

	legacyPage := agouti.JoinPage("http://driver.invalid/session/existing", agouti.HTTPClient(client), agouti.W3C(false))
	if text, err := legacyPage.PopupText(); err != nil || text != "legacy text" {
		t.Fatalf("legacy join text = %q, %v", text, err)
	}
	w3cPage := agouti.JoinPage("http://driver.invalid/session/existing", agouti.HTTPClient(client), agouti.W3C(true))
	if text, err := w3cPage.PopupText(); err != nil || text != "w3c text" {
		t.Fatalf("w3c join text = %q, %v", text, err)
	}

	expected := []string{
		"GET /session/existing/alert_text",
		"GET /session/existing/alert/text",
	}
	if actual := transport.log(); !reflect.DeepEqual(actual, expected) {
		t.Fatalf("unexpected join requests: %v", actual)
	}
}

func TestDialectCapabilitiesCopyNestedChromeOptions(t *testing.T) {
	transport := &dialectTransport{respond: func(request dialectRequest) (int, string) {
		return 200, `{"value":{"sessionId":"w3c-id","capabilities":{}}}`
	}}
	chromeOptions := map[string]interface{}{
		"args": []interface{}{"--headless"},
		"prefs": map[string]interface{}{
			"profile": map[string]interface{}{"default_content_setting_values": map[string]interface{}{"notifications": 2}},
		},
	}
	capabilities := map[string]interface{}{"chromeOptions": chromeOptions}
	before := jsonSnapshot(t, capabilities)

	if _, err := api.OpenW3CWithClient("http://driver.invalid", capabilities, &http.Client{Transport: transport}); err != nil {
		t.Fatalf("failed to open session: %v", err)
	}
	if after := jsonSnapshot(t, capabilities); after != before {
		t.Fatalf("nested chromeOptions were mutated:\nbefore %s\nafter  %s", before, after)
	}
	args := chromeOptions["args"].([]interface{})
	args[0] = "--mutated"
	if after := jsonSnapshot(t, capabilities); after == before {
		t.Fatal("test setup did not mutate the source map")
	}
	if strings.Count(transport.bodies("POST", "/session")[0], "--headless") != 1 {
		t.Fatal("request body should be a copy of the original options")
	}
}

func TestDialectHandshakeDoesNotMutateOnError(t *testing.T) {
	transport := &dialectTransport{respond: func(request dialectRequest) (int, string) {
		return 500, `{"value":{"error":"session not created","message":"nope"}}`
	}}
	capabilities := map[string]interface{}{
		"acceptSslCerts": true,
		"chromeOptions":  map[string]interface{}{"args": []interface{}{"--headless"}},
	}
	before := jsonSnapshot(t, capabilities)
	session, err := api.OpenW3CWithClient("http://driver.invalid", capabilities, &http.Client{Transport: transport})
	if err == nil || session != nil {
		t.Fatal("expected a failed, non-nil-free handshake")
	}
	if after := jsonSnapshot(t, capabilities); after != before {
		t.Fatalf("capabilities mutated on failure: %s != %s", after, before)
	}
}

// sendOnlyBus intentionally implements nothing but Send. Session.Bus must not
// require extra dialect-related methods.
type sendOnlyBus struct {
	method   string
	endpoint string
}

func (b *sendOnlyBus) Send(method, endpoint string, body, result interface{}) error {
	b.method, b.endpoint = method, endpoint
	return nil
}

var _ api.Bus = &sendOnlyBus{}

func TestDialectBusInterfaceUnchanged(t *testing.T) {
	bus := &sendOnlyBus{}
	session := &api.Session{Bus: bus}
	if session.Dialect() != api.JSONWire {
		t.Fatalf("a Send-only Bus should default to the legacy dialect, got %v", session.Dialect())
	}
	if err := session.SetImplicitWait(10); err != nil {
		t.Fatal(err)
	}
	if bus.method != "POST" || bus.endpoint != "timeouts/implicit_wait" {
		t.Fatalf("legacy Send-only bus got %s %s", bus.method, bus.endpoint)
	}
}

func TestDialectNewPageDefaultAndExplicitLegacy(t *testing.T) {
	transport := &dialectTransport{respond: formResponder}
	for _, options := range [][]agouti.Option{nil, {agouti.W3C(false)}} {
		transport.requests = nil
		if _, err := agouti.NewPage("http://driver.invalid",
			append(options, agouti.HTTPClient(&http.Client{Transport: transport}))...); err != nil {
			t.Fatalf("failed to create page: %v", err)
		}
		body := transport.bodies("POST", "/session")[0]
		if !strings.Contains(body, "desiredCapabilities") {
			t.Fatalf("expected a legacy handshake, got %s", body)
		}
		if strings.Contains(body, "alwaysMatch") {
			t.Fatalf("legacy handshake must not contain alwaysMatch: %s", body)
		}
	}
}
