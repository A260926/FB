package rpcc_test

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"net"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/mafredri/cdp/example/sessioncollect"
	"github.com/mafredri/cdp/protocol/runtime"
	"github.com/mafredri/cdp/rpcc"
)

// pipeCDP is a fake CDP endpoint backed by net.Pipe. The client side is
// returned as a *rpcc.Conn, the test controls outer acknowledgements and
// encapsulated messages from the server side.
type pipeCDP struct {
	t     *testing.T
	encMu sync.Mutex
	enc   *json.Encoder
	dec   *json.Decoder
	reqs  chan rpcc.Request
}

func newPipeCDP(t *testing.T) (*pipeCDP, *rpcc.Conn) {
	t.Helper()

	a, b := net.Pipe()
	s := &pipeCDP{
		t:    t,
		enc:  json.NewEncoder(a),
		dec:  json.NewDecoder(a),
		reqs: make(chan rpcc.Request, 32),
	}

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	t.Cleanup(cancel)

	conn, err := rpcc.DialContext(ctx, "pipe://session-tunnel",
		rpcc.WithDialer(func(context.Context, string) (io.ReadWriteCloser, error) {
			return b, nil
		}))
	if err != nil {
		t.Fatalf("DialContext: %v", err)
	}
	t.Cleanup(func() {
		conn.Close()
		a.Close()
		b.Close()
	})

	go s.serve()
	return s, conn
}

func (s *pipeCDP) serve() {
	for {
		var req rpcc.Request
		if err := s.dec.Decode(&req); err != nil {
			return
		}
		select {
		case s.reqs <- req:
		default:
			// The test no longer consumes requests; stop serving.
			return
		}
	}
}

// nextReq waits for the next outer request the client sent.
func (s *pipeCDP) nextReq() rpcc.Request {
	select {
	case req := <-s.reqs:
		return req
	case <-time.After(2 * time.Second):
		s.t.Fatal("timeout waiting for outer request")
		return rpcc.Request{}
	}
}

// write serializes one JSON message towards the client. Errors are
// returned rather than reported through t because the write can race with
// the test's connection cleanup.
func (s *pipeCDP) write(v interface{}) error {
	s.encMu.Lock()
	defer s.encMu.Unlock()
	return s.enc.Encode(v)
}

// respond sends the outer acknowledgement for an outer request ID.
func (s *pipeCDP) respond(id uint64, rerr *rpcc.ResponseError) {
	resp := rpcc.Response{ID: id}
	if rerr != nil {
		resp.Error = rerr
	} else {
		resp.Result = json.RawMessage(`{}`)
	}
	s.write(resp)
}

// sendEvent sends an outer-level notification.
func (s *pipeCDP) sendEvent(method string, params interface{}) {
	raw, err := json.Marshal(params)
	if err != nil {
		s.t.Fatalf("marshal event params: %v", err)
	}
	s.write(rpcc.Response{Method: method, Args: raw})
}

// sendInner encapsulates one inner session message and delivers it.
func (s *pipeCDP) sendInner(sessionID string, inner interface{}) {
	raw, err := json.Marshal(inner)
	if err != nil {
		s.t.Fatalf("marshal inner message: %v", err)
	}
	s.sendEvent("Target.receivedMessageFromTarget", map[string]string{
		"sessionId": sessionID,
		"message":   string(raw),
	})
}

// sendInnerRaw delivers a raw (possibly invalid) encapsulated message.
func (s *pipeCDP) sendInnerRaw(sessionID, message string) {
	s.sendEvent("Target.receivedMessageFromTarget", map[string]string{
		"sessionId": sessionID,
		"message":   message,
	})
}

// sendInnerNoMessage sends the encapsulation event without a message.
func (s *pipeCDP) sendInnerNoMessage(sessionID string) {
	s.sendEvent("Target.receivedMessageFromTarget", map[string]string{
		"sessionId": sessionID,
	})
}

func (s *pipeCDP) sendDetached(sessionID string) {
	s.sendEvent("Target.detachedFromTarget", map[string]string{
		"sessionId": sessionID,
	})
}

// outerRequest is the decoded Target.sendMessageToTarget envelope.
type outerRequest struct {
	Message   string `json:"message"`
	SessionID string `json:"sessionId"`
}

func decodeOuter(t *testing.T, req rpcc.Request) (outerRequest, rpcc.Request) {
	t.Helper()
	if req.Method != "Target.sendMessageToTarget" {
		t.Fatalf("outer method: got %q, want Target.sendMessageToTarget", req.Method)
	}
	rawArgs, err := json.Marshal(req.Args)
	if err != nil {
		t.Fatalf("re-encode outer params: %v", err)
	}
	var outer outerRequest
	if err := json.Unmarshal(rawArgs, &outer); err != nil {
		t.Fatalf("decode outer params: %v", err)
	}
	var inner rpcc.Request
	if err := json.Unmarshal([]byte(outer.Message), &inner); err != nil {
		t.Fatalf("decode inner request: %v", err)
	}
	return outer, inner
}

// evalReply mirrors the nested shape of a Runtime.evaluate result so the
// tunnel's result decoding can be checked without the generated client.
type evalReply struct {
	Result struct {
		Value json.RawMessage `json:"value"`
	} `json:"result"`
}

func (r *evalReply) value(t *testing.T, v interface{}) {
	t.Helper()
	if err := json.Unmarshal(r.Result.Value, v); err != nil {
		t.Fatalf("decode inner value: %v", err)
	}
}

func innerResult(id uint64, value json.RawMessage) rpcc.Response {
	result, _ := json.Marshal(map[string]interface{}{
		"result": map[string]interface{}{
			"type":  "object",
			"value": value,
		},
	})
	return rpcc.Response{ID: id, Result: result}
}

func assertBlocked(t *testing.T, ch <-chan error, what string) {
	t.Helper()
	select {
	case err := <-ch:
		t.Fatalf("%s resolved early with %v, expected it to block", what, err)
	case <-time.After(30 * time.Millisecond):
	}
}

// TestTunnelDualReply covers the two-level reply: outer acceptance and
// the inner result must both be present, and an outer error wins over an
// inner result that arrived first.
func TestTunnelDualReply(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	t.Run("InnerFirstWaitsForOuterAcceptance", func(t *testing.T) {
		srv, parent := newPipeCDP(t)

		child, err := parent.NewSession("sess-dual-1")
		if err != nil {
			t.Fatalf("NewSession: %v", err)
		}

		done := make(chan error, 1)
		var reply evalReply
		go func() {
			done <- rpcc.Invoke(ctx, "Runtime.evaluate",
				map[string]string{"expression": "1"}, &reply, child)
		}()

		req := srv.nextReq()
		outer, inner := decodeOuter(t, req)
		if outer.SessionID != "sess-dual-1" {
			t.Errorf("outer sessionId: got %q", outer.SessionID)
		}
		if inner.ID != 1 || inner.Method != "Runtime.evaluate" {
			t.Errorf("inner request: got id=%d method=%q, want id=1 Runtime.evaluate", inner.ID, inner.Method)
		}

		// Inner result arrives first and must be parked.
		srv.sendInner("sess-dual-1", innerResult(1, json.RawMessage(`{"ok":true}`)))
		assertBlocked(t, done, "Invoke while outer confirmation is missing")

		// Outer acceptance unblocks the call and the parked inner
		// result is returned.
		srv.respond(req.ID, nil)
		select {
		case err := <-done:
			if err != nil {
				t.Fatalf("Invoke: %v", err)
			}
		case <-time.After(2 * time.Second):
			t.Fatal("timeout waiting for Invoke")
		}
		var ok struct {
			OK bool `json:"ok"`
		}
		reply.value(t, &ok)
		if !ok.OK {
			t.Errorf("inner reply was not delivered: %s", reply.Result.Value)
		}
	})

	t.Run("OuterRejectionOverridesParkedInnerSuccess", func(t *testing.T) {
		srv, parent := newPipeCDP(t)

		child, err := parent.NewSession("sess-dual-2")
		if err != nil {
			t.Fatalf("NewSession: %v", err)
		}

		done := make(chan error, 1)
		go func() {
			done <- rpcc.Invoke(ctx, "Runtime.evaluate", nil, nil, child)
		}()

		req := srv.nextReq()

		// The target already answered successfully inside the session.
		srv.sendInner("sess-dual-2", innerResult(1, json.RawMessage(`{}`)))
		assertBlocked(t, done, "Invoke while outer confirmation is missing")

		// The outer Target.sendMessageToTarget is rejected; it wins.
		outerErr := &rpcc.ResponseError{Code: -32000, Message: "session gone"}
		srv.respond(req.ID, outerErr)

		select {
		case err := <-done:
			var got *rpcc.ResponseError
			if !errors.As(err, &got) {
				t.Fatalf("want *rpcc.ResponseError, got %T: %v", err, err)
			}
			if got.Code != outerErr.Code || got.Message != outerErr.Message {
				t.Errorf("outer error: got %v, want %v", got, outerErr)
			}
		case <-time.After(2 * time.Second):
			t.Fatal("timeout waiting for Invoke")
		}
	})

	t.Run("InnerErrorReturnedAfterOuterAcceptance", func(t *testing.T) {
		srv, parent := newPipeCDP(t)

		child, err := parent.NewSession("sess-dual-3")
		if err != nil {
			t.Fatalf("NewSession: %v", err)
		}

		done := make(chan error, 1)
		go func() {
			done <- rpcc.Invoke(ctx, "Runtime.evaluate", nil, nil, child)
		}()

		req := srv.nextReq()

		// Outer acceptance first, then the target reports an inner
		// ResponseError that must survive errors.As.
		srv.respond(req.ID, nil)
		assertBlocked(t, done, "Invoke while inner result is missing")

		innerErr := &rpcc.ResponseError{Code: -32001, Message: "evaluate failed"}
		srv.sendInner("sess-dual-3", rpcc.Response{ID: 1, Error: innerErr})

		select {
		case err := <-done:
			var got *rpcc.ResponseError
			if !errors.As(err, &got) {
				t.Fatalf("want *rpcc.ResponseError, got %T: %v", err, err)
			}
			if got.Code != innerErr.Code {
				t.Errorf("inner error: got %v, want %v", got, innerErr)
			}
		case <-time.After(2 * time.Second):
			t.Fatal("timeout waiting for Invoke")
		}
	})
}

// TestTunnelIsolation verifies session bookkeeping: independent sequence
// spaces, concurrent pending commands, unaffected parent traffic and the
// blast radius of close, bad messages and detach.
func TestTunnelIsolation(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer cancel()

	t.Run("TwoCommandsPendingWithoutOuterConfirmation", func(t *testing.T) {
		srv, parent := newPipeCDP(t)

		child, err := parent.NewSession("sess-pending")
		if err != nil {
			t.Fatalf("NewSession: %v", err)
		}

		type callResult struct {
			N int `json:"n"`
		}
		errs := make(chan error, 2)
		var r1, r2 evalReply

		// Start the calls one at a time and wait for each request to
		// reach the wire, so inner ids are deterministically 1 then 2
		// while both commands remain in flight.
		go func() { errs <- rpcc.Invoke(ctx, "Runtime.evaluate", nil, &r1, child) }()
		req1 := srv.nextReq()
		_, in1 := decodeOuter(t, req1)
		go func() { errs <- rpcc.Invoke(ctx, "Runtime.evaluate", nil, &r2, child) }()
		req2 := srv.nextReq()
		_, in2 := decodeOuter(t, req2)

		outerByInner := map[uint64]rpcc.Request{in1.ID: req1, in2.ID: req2}
		if len(outerByInner) != 2 || outerByInner[1].ID == 0 || outerByInner[2].ID == 0 {
			t.Fatalf("inner ids on the wire: %d and %d, want 1 and 2", in1.ID, in2.ID)
		}
		assertBlocked(t, errs, "first Invoke")
		assertBlocked(t, errs, "second Invoke")

		// Settle out of order: the inner result for command 2 arrives
		// first and the outer acceptance for command 1 arrives first.
		srv.sendInner("sess-pending", innerResult(2, json.RawMessage(`{"n":20}`)))
		srv.respond(outerByInner[1].ID, nil)
		srv.respond(outerByInner[2].ID, nil)
		srv.sendInner("sess-pending", innerResult(1, json.RawMessage(`{"n":10}`)))

		for i := 0; i < 2; i++ {
			select {
			case err := <-errs:
				if err != nil {
					t.Fatalf("Invoke %d: %v", i, err)
				}
			case <-time.After(2 * time.Second):
				t.Fatal("timeout waiting for Invoke")
			}
		}
		// Positional check proves replies are not routed across calls.
		var v1, v2 callResult
		r1.value(t, &v1)
		r2.value(t, &v2)
		if v1.N != 10 || v2.N != 20 {
			t.Errorf("responses crossed: r1=%d r2=%d, want 10 and 20", v1.N, v2.N)
		}
	})

	t.Run("SessionsReuseInnerRequestIDs", func(t *testing.T) {
		srv, parent := newPipeCDP(t)

		childA, err := parent.NewSession("sess-a")
		if err != nil {
			t.Fatalf("NewSession A: %v", err)
		}
		childB, err := parent.NewSession("sess-b")
		if err != nil {
			t.Fatalf("NewSession B: %v", err)
		}

		type page struct {
			Page string `json:"page"`
		}
		var ra, rb evalReply
		doneA := make(chan error, 1)
		doneB := make(chan error, 1)
		go func() { doneA <- rpcc.Invoke(ctx, "Runtime.evaluate", nil, &ra, childA) }()
		go func() { doneB <- rpcc.Invoke(ctx, "Runtime.evaluate", nil, &rb, childB) }()

		// Concurrent sends can arrive in either order; collect both
		// envelopes and key them by session id.
		envelopes := make(map[string]struct {
			outer outerRequest
			req   rpcc.Request
		}, 2)
		for i := 0; i < 2; i++ {
			req := srv.nextReq()
			outer, inner := decodeOuter(t, req)
			envelopes[outer.SessionID] = struct {
				outer outerRequest
				req   rpcc.Request
			}{outer, req}
			// Both sessions number their first command 1.
			if inner.ID != 1 {
				t.Fatalf("inner id for %q: got %d, want 1", outer.SessionID, inner.ID)
			}
		}
		envelopeA, envelopeB := envelopes["sess-a"], envelopes["sess-b"]
		if envelopeA.req.ID == envelopeB.req.ID {
			t.Fatalf("outer ids collided: %d", envelopeA.req.ID)
		}

		srv.respond(envelopeA.req.ID, nil)
		srv.respond(envelopeB.req.ID, nil)
		// Reply in reverse order, same inner id, routed by session.
		srv.sendInner("sess-b", innerResult(1, json.RawMessage(`{"page":"b"}`)))
		srv.sendInner("sess-a", innerResult(1, json.RawMessage(`{"page":"a"}`)))

		for _, ch := range []<-chan error{doneA, doneB} {
			select {
			case err := <-ch:
				if err != nil {
					t.Fatalf("Invoke: %v", err)
				}
			case <-time.After(2 * time.Second):
				t.Fatal("timeout waiting for Invoke")
			}
		}
		var va, vb page
		ra.value(t, &va)
		rb.value(t, &vb)
		if va.Page != "a" || vb.Page != "b" {
			t.Errorf("session routing crossed: va=%q vb=%q", va.Page, vb.Page)
		}
	})

	t.Run("ParentCommandsAndEventSubscriptionsKeepWorking", func(t *testing.T) {
		srv, parent := newPipeCDP(t)

		// A pre-existing parent subscription must still receive the
		// full Target.receivedMessageFromTarget event.
		parentStream, err := rpcc.NewStream(ctx, "Target.receivedMessageFromTarget", parent)
		if err != nil {
			t.Fatalf("parent NewStream: %v", err)
		}
		defer parentStream.Close()

		child, err := parent.NewSession("sess-parent")
		if err != nil {
			t.Fatalf("NewSession: %v", err)
		}
		childStream, err := rpcc.NewStream(ctx, "Runtime.consoleAPICalled", child)
		if err != nil {
			t.Fatalf("child NewStream: %v", err)
		}
		defer childStream.Close()

		srv.sendInner("sess-parent", rpcc.Response{
			Method: "Runtime.consoleAPICalled",
			Args:   json.RawMessage(`{"type":"log"}`),
		})

		var parentEvent struct {
			SessionID string `json:"sessionId"`
			Message   string `json:"message"`
		}
		if err := parentStream.RecvMsg(&parentEvent); err != nil {
			t.Fatalf("parent stream RecvMsg: %v", err)
		}
		if parentEvent.SessionID != "sess-parent" || parentEvent.Message == "" {
			t.Errorf("parent stream got %+v, want full encapsulated event", parentEvent)
		}

		var childEvent struct {
			Type string `json:"type"`
		}
		if err := childStream.RecvMsg(&childEvent); err != nil {
			t.Fatalf("child stream RecvMsg: %v", err)
		}
		if childEvent.Type != "log" {
			t.Errorf("child stream got %q, want log", childEvent.Type)
		}

		// Ordinary parent commands still work and share the outer
		// request sequence with the tunneled ones.
		parentErr := make(chan error, 1)
		go func() {
			parentErr <- rpcc.Invoke(ctx, "test.ParentPing", nil, nil, parent)
		}()
		req := srv.nextReq()
		if req.Method != "test.ParentPing" {
			t.Fatalf("parent command method: got %q, want test.ParentPing", req.Method)
		}
		srv.respond(req.ID, nil)
		if err := <-parentErr; err != nil {
			t.Errorf("parent Invoke: %v", err)
		}
	})

	t.Run("SessionIDIsReservedAndValidated", func(t *testing.T) {
		_, parent := newPipeCDP(t)

		if _, err := parent.NewSession(""); err == nil {
			t.Error("empty sessionID was accepted")
		}

		child, err := parent.NewSession("sess-once")
		if err != nil {
			t.Fatalf("NewSession: %v", err)
		}
		if _, err := parent.NewSession("sess-once"); !errors.Is(err, rpcc.ErrSessionExists) {
			t.Errorf("duplicate session: got %v, want ErrSessionExists", err)
		}

		// Closed IDs are not reused.
		if err := child.Close(); err != nil {
			t.Fatalf("child Close: %v", err)
		}
		if _, err := parent.NewSession("sess-once"); !errors.Is(err, rpcc.ErrSessionExists) {
			t.Errorf("reused closed session id: got %v, want ErrSessionExists", err)
		}
	})

	t.Run("ClosedParentAndNestedSessionRejected", func(t *testing.T) {
		_, parent := newPipeCDP(t)

		child, err := parent.NewSession("sess-nested")
		if err != nil {
			t.Fatalf("NewSession: %v", err)
		}

		// A child Conn cannot create grandchild sessions.
		if _, err := child.NewSession("sess-grandchild"); err == nil {
			t.Error("nested NewSession was accepted")
		}

		// NewSession on a closed parent returns the close error.
		if err := parent.Close(); err != nil {
			t.Fatalf("parent Close: %v", err)
		}
		if _, err := parent.NewSession("sess-after-close"); !errors.Is(err, rpcc.ErrConnClosing) {
			t.Errorf("NewSession after parent close: got %v, want ErrConnClosing", err)
		}
	})

	t.Run("ChildCloseOnlyAffectsThatSession", func(t *testing.T) {
		srv, parent := newPipeCDP(t)

		victim, err := parent.NewSession("sess-victim")
		if err != nil {
			t.Fatalf("NewSession victim: %v", err)
		}
		sibling, err := parent.NewSession("sess-sibling")
		if err != nil {
			t.Fatalf("NewSession sibling: %v", err)
		}

		inFlight := make(chan error, 1)
		go func() {
			inFlight <- rpcc.Invoke(ctx, "Runtime.evaluate", nil, nil, victim)
		}()
		srv.nextReq() // Ensure the command was sent before closing.

		stream, err := rpcc.NewStream(ctx, "Runtime.consoleAPICalled", victim)
		if err != nil {
			t.Fatalf("victim NewStream: %v", err)
		}

		if err := victim.Close(); err != nil {
			t.Fatalf("victim Close: %v", err)
		}

		select {
		case err := <-inFlight:
			if !errors.Is(err, rpcc.ErrConnClosing) {
				t.Errorf("in-flight command: got %v, want ErrConnClosing", err)
			}
		case <-time.After(2 * time.Second):
			t.Fatal("in-flight command was not ended by Close")
		}
		if err := stream.RecvMsg(nil); !errors.Is(err, rpcc.ErrConnClosing) {
			t.Errorf("victim stream: got %v, want ErrConnClosing", err)
		}

		// Parent and sibling are untouched. The sibling completes a
		// normal command and still receives notifications.
		done := make(chan error, 1)
		go func() {
			done <- rpcc.Invoke(ctx, "Runtime.evaluate", nil, nil, sibling)
		}()
		req := srv.nextReq()
		srv.respond(req.ID, nil)
		srv.sendInner("sess-sibling", innerResult(1, json.RawMessage(`{}`)))
		select {
		case err := <-done:
			if err != nil {
				t.Fatalf("sibling Invoke after victim close: %v", err)
			}
		case <-time.After(2 * time.Second):
			t.Fatal("timeout waiting for sibling Invoke")
		}
	})

	t.Run("BadMessagesAndDetachAreScoped", func(t *testing.T) {
		srv, parent := newPipeCDP(t)

		bad, err := parent.NewSession("sess-bad")
		if err != nil {
			t.Fatalf("NewSession bad: %v", err)
		}
		good, err := parent.NewSession("sess-good")
		if err != nil {
			t.Fatalf("NewSession good: %v", err)
		}

		// Invalid single JSON message closes only the bad session and
		// ends its in-flight command with a protocol error.
		badCmd := make(chan error, 1)
		go func() {
			badCmd <- rpcc.Invoke(ctx, "Runtime.evaluate", nil, nil, bad)
		}()
		srv.nextReq()
		srv.sendInnerRaw("sess-bad", "{not json")
		waitClosed(t, bad)
		if err := bad.Context().Err(); err == nil {
			t.Error("bad session context was not cancelled")
		}
		select {
		case err := <-badCmd:
			if err == nil || !strings.Contains(err.Error(), "protocol error") {
				t.Errorf("bad-session command: got %v, want a protocol error", err)
			}
		case <-time.After(2 * time.Second):
			t.Fatal("bad-session command was not ended")
		}

		// Missing message field likewise closes that session only.
		missing, err := parent.NewSession("sess-missing")
		if err != nil {
			t.Fatalf("NewSession missing: %v", err)
		}
		missingCmd := make(chan error, 1)
		go func() {
			missingCmd <- rpcc.Invoke(ctx, "Runtime.evaluate", nil, nil, missing)
		}()
		srv.nextReq()
		srv.sendInnerNoMessage("sess-missing")
		waitClosed(t, missing)
		select {
		case err := <-missingCmd:
			if err == nil || !strings.Contains(err.Error(), "protocol error") {
				t.Errorf("missing-message command: got %v, want a protocol error", err)
			}
		case <-time.After(2 * time.Second):
			t.Fatal("missing-message command was not ended")
		}

		// Unknown sessions are ignored, they must not close anything.
		srv.sendInnerRaw("sess-unknown", `{"id":1,"result":{}}`)
		if err := good.Context().Err(); err != nil {
			t.Errorf("good session affected by unknown session event: %v", err)
		}

		// Two concatenated JSON values are not a single response and
		// only close the session they belong to.
		multi, err := parent.NewSession("sess-multi")
		if err != nil {
			t.Fatalf("NewSession multi: %v", err)
		}
		srv.sendInnerRaw("sess-multi", `{"id":1,"result":{}} {"id":2,"result":{}}`)
		waitClosed(t, multi)
		if err := good.Context().Err(); err != nil {
			t.Errorf("good session affected by multi-value event: %v", err)
		}

		// Detach ends the targeted session with ErrSessionDetached.
		detached, err := parent.NewSession("sess-detached")
		if err != nil {
			t.Fatalf("NewSession detached: %v", err)
		}
		late := make(chan error, 1)
		go func() {
			late <- rpcc.Invoke(ctx, "Runtime.evaluate", nil, nil, detached)
		}()
		srv.nextReq()
		srv.sendDetached("sess-detached")
		select {
		case err := <-late:
			if !errors.Is(err, rpcc.ErrSessionDetached) {
				t.Errorf("detached Invoke: got %v, want ErrSessionDetached", err)
			}
		case <-time.After(2 * time.Second):
			t.Fatal("detached Invoke was not ended")
		}

		// Late inner/outer replies must not settle the call again.
		srv.sendInner("sess-detached", innerResult(1, json.RawMessage(`{}`)))
		select {
		case err := <-late:
			t.Fatalf("late inner reply settled the call again: %v", err)
		case <-time.After(30 * time.Millisecond):
		}

		// The good session still serves a full command.
		done := make(chan error, 1)
		go func() { done <- rpcc.Invoke(ctx, "Runtime.evaluate", nil, nil, good) }()
		req := srv.nextReq()
		srv.respond(req.ID, nil)
		srv.sendInner("sess-good", innerResult(1, json.RawMessage(`{}`)))
		select {
		case err := <-done:
			if err != nil {
				t.Fatalf("good session Invoke: %v", err)
			}
		case <-time.After(2 * time.Second):
			t.Fatal("timeout waiting for good session Invoke")
		}

		// Parent itself is still open.
		if err := parent.Context().Err(); err != nil {
			t.Errorf("parent context affected by session failures: %v", err)
		}
	})

	t.Run("ParentCloseEndsEverySession", func(t *testing.T) {
		srv, parent := newPipeCDP(t)

		child, err := parent.NewSession("sess-parent-close")
		if err != nil {
			t.Fatalf("NewSession: %v", err)
		}
		done := make(chan error, 1)
		go func() {
			done <- rpcc.Invoke(ctx, "Runtime.evaluate", nil, nil, child)
		}()
		srv.nextReq()

		if err := parent.Close(); err != nil {
			t.Fatalf("parent Close: %v", err)
		}
		waitClosed(t, child)

		select {
		case err := <-done:
			if err == nil {
				t.Fatal("in-flight session command succeeded after parent close")
			}
		case <-time.After(2 * time.Second):
			t.Fatal("session command was not ended by parent close")
		}
	})
}

// waitClosed blocks until the session context is cancelled.
func waitClosed(t *testing.T, c *rpcc.Conn) {
	t.Helper()
	select {
	case <-c.Context().Done():
	case <-time.After(2 * time.Second):
		t.Fatal("session was not closed in time")
	}
}

// TestTunnelGenerated drives the tunnel through generated clients and the
// real cross-target collection example.
func TestTunnelGenerated(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	t.Run("RuntimeEvaluateAndInnerNotification", func(t *testing.T) {
		srv, parent := newPipeCDP(t)

		child, err := parent.NewSession("sess-runtime")
		if err != nil {
			t.Fatalf("NewSession: %v", err)
		}

		client := runtime.NewClient(child)
		stream, err := rpcc.NewStream(ctx, "Runtime.consoleAPICalled", child)
		if err != nil {
			t.Fatalf("NewStream: %v", err)
		}
		defer stream.Close()

		evalDone := make(chan error, 1)
		var reply *runtime.EvaluateReply
		go func() {
			var err error
			reply, err = client.Evaluate(ctx, runtime.NewEvaluateArgs("1+1").SetReturnByValue(true))
			evalDone <- err
		}()

		req := srv.nextReq()
		outer, inner := decodeOuter(t, req)
		if outer.SessionID != "sess-runtime" || inner.ID != 1 {
			t.Fatalf("unexpected envelope: %+v inner: %+v", outer, inner)
		}

		// Inner notification and inner result, then outer acceptance.
		srv.sendInner("sess-runtime", rpcc.Response{
			Method: "Runtime.consoleAPICalled",
			Args:   json.RawMessage(`{"type":"log","args":[]}`),
		})
		srv.sendInner("sess-runtime", innerResult(1, json.RawMessage(`42`)))
		srv.respond(req.ID, nil)

		select {
		case err := <-evalDone:
			if err != nil {
				t.Fatalf("Evaluate: %v", err)
			}
		case <-time.After(2 * time.Second):
			t.Fatal("timeout waiting for Evaluate")
		}
		if string(reply.Result.Value) != "42" {
			t.Errorf("evaluate value: got %s, want 42", reply.Result.Value)
		}

		var event runtime.ConsoleAPICalledReply
		if err := stream.RecvMsg(&event); err != nil {
			t.Fatalf("RecvMsg: %v", err)
		}
		if event.Type != "log" {
			t.Errorf("event type: got %q, want log", event.Type)
		}
	})

	t.Run("CrossTargetCollectionOrchestration", func(t *testing.T) {
		srv, parent := newPipeCDP(t)

		const (
			sessionA = "sess-page-a"
			sessionB = "sess-page-b"
		)
		pages := []sessioncollect.PageConfig{
			{SessionID: sessionA, Source: "page-a"},
			{SessionID: sessionB, Source: "page-b"},
		}

		pageBProducts := []sessioncollect.Product{
			{ID: "p1", Name: "Mug", Price: 100},
		}

		// Scripted page-side CDP: both sessions reuse inner id 1. For
		// page A the inner success arrives first and the outer request
		// is then rejected; page B succeeds and pushes an update.
		var wire []rpcc.Request
		var wireMu sync.Mutex
		serverDone := make(chan struct{})
		go func() {
			defer close(serverDone)
			for handled := 0; handled < 3; handled++ {
				select {
				case req := <-srv.reqs:
					if req.Method != "Target.sendMessageToTarget" {
						// Ordinary parent command, acknowledge it and
						// keep it out of the tunneled wire log.
						srv.respond(req.ID, nil)
						continue
					}

					wireMu.Lock()
					wire = append(wire, req)
					wireMu.Unlock()

					var outer outerRequest
					rawArgs, err := json.Marshal(req.Args)
					if err != nil {
						t.Errorf("re-encode outer params: %v", err)
						continue
					}
					if err := json.Unmarshal(rawArgs, &outer); err != nil {
						t.Errorf("outer params: %v", err)
						continue
					}
					switch outer.SessionID {
					case sessionA:
						// Inner answer first, parked behind the outer
						// request which is rejected afterwards.
						srv.sendInner(sessionA, innerResult(1, json.RawMessage(`[{"id":"px","name":"stale","price":1}]`)))
						srv.respond(req.ID, &rpcc.ResponseError{
							Code: -32000, Message: "outer rejected",
						})
					case sessionB:
						srv.respond(req.ID, nil)
						products, _ := json.Marshal(pageBProducts)
						srv.sendInner(sessionB, innerResult(1, products))
						// Update after the stream had time to
						// subscribe: one duplicate (p1) and one new
						// product (p2).
						time.Sleep(60 * time.Millisecond)
						event, _ := json.Marshal(rpcc.Response{
							Method: "Runtime.consoleAPICalled",
							Args: json.RawMessage(`{"type":"log","args":[
								{"type":"object","value":{"id":"p1","name":"Mug","price":100}},
								{"type":"object","value":{"id":"p2","name":"Bowl","price":250}}
							]}`),
						})
						srv.sendInnerRaw(sessionB, string(event))
					default:
						t.Errorf("unexpected session %q", outer.SessionID)
					}
				case <-ctx.Done():
					return
				}
			}
		}()

		var out bytes.Buffer
		failures, err := sessioncollect.Collect(ctx, parent, pages, sessioncollect.Options{
			Expression:    "collectProducts()",
			UpdateTimeout: 400 * time.Millisecond,
		}, &out)
		if err != nil {
			t.Fatalf("Collect: %v", err)
		}

		// Error list: only page A failed, and the original
		// ResponseError remains reachable through errors.As.
		if len(failures) != 1 {
			t.Fatalf("failures: got %d entries %+v, want 1", len(failures), failures)
		}
		f := failures[0]
		if f.SessionID != sessionA {
			t.Errorf("failure session: got %q, want %q", f.SessionID, sessionA)
		}
		var respErr *rpcc.ResponseError
		if !errors.As(f.Err, &respErr) {
			t.Errorf("failure error: got %v, want *rpcc.ResponseError", f.Err)
		} else if respErr.Message != "outer rejected" {
			t.Errorf("failure message: got %q", respErr.Message)
		}

		// Structured JSON Lines output: only page B data, de-duplicated
		// by session ID and product ID.
		var records []sessioncollect.Record
		dec := json.NewDecoder(bytes.NewReader(out.Bytes()))
		for dec.More() {
			var r sessioncollect.Record
			if err := dec.Decode(&r); err != nil {
				t.Fatalf("decode JSON Lines: %v", err)
			}
			records = append(records, r)
		}
		if len(records) != 2 {
			t.Fatalf("records: got %d %+v, want 2", len(records), records)
		}
		seen := map[string]bool{}
		for _, r := range records {
			if r.SessionID != sessionB || r.Source != "page-b" {
				t.Errorf("record from failed/unknown page leaked: %+v", r)
			}
			key := r.SessionID + "/" + r.Product.ID
			if seen[key] {
				t.Errorf("duplicate record published: %s", key)
			}
			seen[key] = true
		}
		if !seen[sessionB+"/p1"] || !seen[sessionB+"/p2"] {
			t.Errorf("missing products, got keys: %v", seen)
		}

		// Ordinary parent collection keeps running on the same Conn.
		// The scripted server owns the request channel and answers it
		// like any non-tunneled parent command.
		parentDone := make(chan error, 1)
		go func() {
			parentDone <- rpcc.Invoke(ctx, "test.ParentCollection", nil, nil, parent)
		}()

		// The scripted server handled both tunneled commands and the
		// parent command.
		select {
		case <-serverDone:
		case <-time.After(2 * time.Second):
			t.Fatal("scripted CDP server did not finish in time")
		}
		select {
		case err := <-parentDone:
			if err != nil {
				t.Fatalf("parent Invoke: %v", err)
			}
		case <-time.After(2 * time.Second):
			t.Fatal("timeout waiting for parent Invoke")
		}

		// Check the actual tunneled wire requests: two commands with
		// distinct outer ids and reused inner id 1.
		wireMu.Lock()
		got := wire
		wireMu.Unlock()
		if len(got) != 2 {
			t.Fatalf("tunneled wire requests: got %d, want 2", len(got))
		}
		innerIDs := map[uint64]int{}
		outerIDs := map[uint64]bool{}
		sessions := map[string]bool{}
		for _, req := range got {
			if req.Method != "Target.sendMessageToTarget" {
				t.Fatalf("wire method: got %q", req.Method)
			}
			outer, inner := decodeOuter(t, req)
			innerIDs[inner.ID]++
			outerIDs[req.ID] = true
			sessions[outer.SessionID] = true
		}
		if innerIDs[1] != 2 {
			t.Errorf("inner id reuse: got %v, want two commands with id 1", innerIDs)
		}
		if len(outerIDs) != 2 {
			t.Errorf("outer ids were not unique: %v", outerIDs)
		}
		if !sessions[sessionA] || !sessions[sessionB] {
			t.Errorf("missing sessions on the wire: %v", sessions)
		}
	})
}
