package chromedp

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/chromedp/cdproto/cdp"
	"github.com/chromedp/cdproto/network"
	"github.com/chromedp/cdproto/page"
)

// pixelPNG is a 1x1 transparent PNG.
var pixelPNG = []byte{
	0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
	0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
	0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
	0x89, 0x00, 0x00, 0x00, 0x0a, 0x49, 0x44, 0x41,
	0x54, 0x78, 0x9c, 0x63, 0x00, 0x01, 0x00, 0x00,
	0x05, 0x00, 0x01, 0x0d, 0x0a, 0x2d, 0xb4, 0x00,
	0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
	0x42, 0x60, 0x82,
}

// frameResponseServer is a local test server hosting a top-level page and
// two same-origin iframes. The top-level page and both iframe documents use
// the same field and button IDs, so that tests can prove that actions are
// scoped to a single frame.
type frameResponseServer struct {
	s *httptest.Server

	// posted receives the parsed form of every /submit request.
	posted chan url.Values
	// imgStarted receives a value when the browser requests the result
	// page's image, which the server then holds until releaseImg is
	// called.
	imgStarted chan struct{}

	releaseImg func()
}

func newFrameResponseServer(t *testing.T) *frameResponseServer {
	t.Helper()

	fs := &frameResponseServer{
		posted:     make(chan url.Values, 1),
		imgStarted: make(chan struct{}, 1),
	}
	imgRelease := make(chan struct{})
	var imgOnce sync.Once
	fs.releaseImg = func() { imgOnce.Do(func() { close(imgRelease) }) }

	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		fmt.Fprint(w, `<html><body>
<form action="/submit" method="post">
<input id="field" name="field" value="top-original">
<input type="hidden" name="who" value="top">
<button id="submit" type="submit">go</button>
</form>
<iframe id="frame-a" src="/frame?who=a"></iframe>
<iframe id="frame-b" src="/frame?who=b"></iframe>
</body></html>`)
	})
	mux.HandleFunc("/frame", func(w http.ResponseWriter, r *http.Request) {
		who := r.URL.Query().Get("who")
		w.Header().Set("Content-Type", "text/html")
		fmt.Fprintf(w, `<html><body>
<div id="state">frame %s original</div>
<form action="/submit" method="post">
<input id="field" name="field" value="">
<input type="hidden" name="who" value="%s">
<button id="submit" type="submit">go</button>
</form>
</body></html>`, who, who)
	})
	mux.HandleFunc("/submit", func(w http.ResponseWriter, r *http.Request) {
		if err := r.ParseForm(); err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		fs.posted <- r.Form
		q := url.Values{}
		q.Set("who", r.Form.Get("who"))
		q.Set("field", r.Form.Get("field"))
		http.Redirect(w, r, "/result?"+q.Encode(), http.StatusSeeOther)
	})
	mux.HandleFunc("/result", func(w http.ResponseWriter, r *http.Request) {
		who := r.URL.Query().Get("who")
		field := r.URL.Query().Get("field")
		w.Header().Set("Content-Type", "text/html")
		w.Header().Set("X-Result-Who", who)
		fmt.Fprintf(w, `<html><body>
<div id="result">result:%s:%s</div>
<img src="/img?who=%s">
</body></html>`, who, field, who)
	})
	mux.HandleFunc("/img", func(w http.ResponseWriter, r *http.Request) {
		select {
		case fs.imgStarted <- struct{}{}:
		default:
		}
		<-imgRelease
		w.Header().Set("Content-Type", "image/png")
		_, _ = w.Write(pixelPNG)
	})

	fs.s = httptest.NewServer(mux)
	t.Cleanup(fs.s.Close)
	// Release any held request before the server is closed; cleanup
	// functions run in LIFO order, so this runs before fs.s.Close.
	t.Cleanup(fs.releaseImg)
	return fs
}

// frameWaitResult carries the outcome of a RunResponseForFrame call running
// in a separate goroutine.
type frameWaitResult struct {
	resp *network.Response
	err  error
}

func waitForFrameSignal(t *testing.T, ch <-chan struct{}, what string) {
	t.Helper()
	select {
	case <-ch:
	case <-time.After(10 * time.Second):
		t.Fatalf("timed out waiting for %s", what)
	}
}

// setupFrameResponseTest starts the test server and navigates a new tab to
// the top-level page containing the two iframes.
func setupFrameResponseTest(t *testing.T) (context.Context, *frameResponseServer) {
	t.Helper()

	fs := newFrameResponseServer(t)

	ctx, cancel := testAllocate(t, "")
	t.Cleanup(cancel)
	ctx, cancel = context.WithTimeout(ctx, 30*time.Second)
	t.Cleanup(cancel)

	if err := Run(ctx, Navigate(fs.s.URL+"/")); err != nil {
		t.Fatal(err)
	}
	return ctx, fs
}

// frameTestIframeIDs returns the frame IDs of the two iframes in the
// top-level page.
func frameTestIframeIDs(t *testing.T, ctx context.Context) (frameA, frameB cdp.FrameID) {
	t.Helper()

	var iframes []*cdp.Node
	if err := Run(ctx, Nodes("iframe", &iframes, ByQuery)); err != nil {
		t.Fatal(err)
	}
	if len(iframes) != 2 {
		t.Fatalf("expected 2 iframes, got %d", len(iframes))
	}
	if iframes[0].FrameID == "" || iframes[1].FrameID == "" {
		t.Fatalf("iframe nodes missing frame IDs: %q, %q", iframes[0].FrameID, iframes[1].FrameID)
	}
	return iframes[0].FrameID, iframes[1].FrameID
}

// checkFrameCtxUsable verifies that the original context still works after a
// failed or cancelled wait.
func checkFrameCtxUsable(t *testing.T, ctx context.Context, wantURL string) {
	t.Helper()

	var u string
	if err := Run(ctx, Location(&u)); err != nil {
		t.Fatalf("context is no longer usable: %v", err)
	}
	if u != wantURL {
		t.Fatalf("unexpected top-level location %q, want %q", u, wantURL)
	}
}

func TestFrameResponseForm(t *testing.T) {
	t.Parallel()

	ctx, fs := setupFrameResponseTest(t)

	var iframes []*cdp.Node
	if err := Run(ctx, Nodes("iframe", &iframes, ByQuery)); err != nil {
		t.Fatal(err)
	}
	if len(iframes) != 2 {
		t.Fatalf("expected 2 iframes, got %d", len(iframes))
	}
	frameB := iframes[1]
	frameID := frameB.FrameID
	if frameID == "" {
		t.Fatal("iframe node has no frame ID")
	}

	// Fill in the form inside the second iframe and submit it. The form
	// POSTs to /submit, which redirects to the result page.
	resCh := make(chan frameWaitResult, 1)
	go func() {
		resp, err := RunResponseForFrame(ctx, frameID,
			SendKeys("#field", "hello-b", ByQuery, FromNode(frameB)),
			Click("#submit", ByQuery, FromNode(frameB)),
		)
		resCh <- frameWaitResult{resp, err}
	}()

	// The server holds the result page's image. Once the browser requests
	// it, the document response headers have necessarily arrived already,
	// but the frame has not finished loading yet, so the wait must still
	// be in progress.
	waitForFrameSignal(t, fs.imgStarted, "the result page image request")
	select {
	case r := <-resCh:
		t.Fatalf("RunResponseForFrame returned before the frame finished loading: %+v", r)
	default:
	}

	// Let the image load; the frame's document load now completes.
	fs.releaseImg()

	var resp *network.Response
	select {
	case r := <-resCh:
		if r.err != nil {
			t.Fatalf("RunResponseForFrame: %v", r.err)
		}
		resp = r.resp
	case <-time.After(10 * time.Second):
		t.Fatal("RunResponseForFrame did not return after the image loaded")
	}

	if resp == nil {
		t.Fatal("expected a response, got nil")
	}
	if want := fs.s.URL + "/result?field=hello-b&who=b"; resp.URL != want {
		t.Errorf("response URL: want %q, got %q", want, resp.URL)
	}
	if resp.Status != 200 {
		t.Errorf("response status: want 200, got %d", resp.Status)
	}
	var whoHeader any
	for k, v := range resp.Headers {
		if strings.EqualFold(k, "X-Result-Who") {
			whoHeader = v
		}
	}
	if whoHeader != "b" {
		t.Errorf("response headers: want X-Result-Who to be %q, got %v in %v", "b", whoHeader, resp.Headers)
	}

	// The server must have received the fields posted by the iframe's
	// form.
	select {
	case form := <-fs.posted:
		if got := form.Get("who"); got != "b" {
			t.Errorf("posted who field: want %q, got %q", "b", got)
		}
		if got := form.Get("field"); got != "hello-b" {
			t.Errorf("posted field: want %q, got %q", "hello-b", got)
		}
	default:
		t.Error("server did not receive the form submission")
	}

	// The iframe shows the result page; the other iframe and the
	// top-level page are untouched.
	var resultText, topValue, frameAState, frameALocation, topLocation string
	if err := Run(ctx,
		Evaluate(`document.getElementById('frame-b').contentDocument.getElementById('result').textContent`, &resultText),
		Evaluate(`document.getElementById('field').value`, &topValue),
		Evaluate(`document.getElementById('frame-a').contentDocument.getElementById('state').textContent`, &frameAState),
		Evaluate(`document.getElementById('frame-a').contentWindow.location.pathname`, &frameALocation),
		Evaluate(`location.pathname`, &topLocation),
	); err != nil {
		t.Fatal(err)
	}
	if want := "result:b:hello-b"; resultText != want {
		t.Errorf("frame-b DOM: want %q, got %q", want, resultText)
	}
	if want := "top-original"; topValue != want {
		t.Errorf("top-level field: want %q, got %q", want, topValue)
	}
	if want := "frame a original"; frameAState != want {
		t.Errorf("frame-a DOM: want %q, got %q", want, frameAState)
	}
	if want := "/frame"; frameALocation != want {
		t.Errorf("frame-a location: want %q, got %q", want, frameALocation)
	}
	if want := "/"; topLocation != want {
		t.Errorf("top-level location: want %q, got %q", want, topLocation)
	}
}

func TestFrameResponseIsolation(t *testing.T) {
	t.Parallel()

	t.Run("ChildFragmentDoesNotEndTopWait", func(t *testing.T) {
		ctx, fs := setupFrameResponseTest(t)

		fragSeen := make(chan struct{})
		var fragOnce sync.Once
		lctx, lcancel := context.WithCancel(ctx)
		defer lcancel()
		ListenTarget(lctx, func(ev any) {
			if ev, ok := ev.(*page.EventNavigatedWithinDocument); ok && strings.HasSuffix(ev.URL, "#frag") {
				fragOnce.Do(func() { close(fragSeen) })
			}
		})

		// Once the child frame's fragment navigation has happened,
		// cancel the wait. If RunResponse mistook the child's
		// fragment navigation for a top-level one, it would return
		// nil, nil before the cancellation.
		waitCtx, cancelWait := context.WithCancel(ctx)
		defer cancelWait()
		go func() {
			<-fragSeen
			cancelWait()
		}()

		resp, err := RunResponse(waitCtx,
			Evaluate(`document.getElementById('frame-a').contentWindow.location.hash = 'frag'`, nil))
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("expected context.Canceled, got resp=%v err=%v", resp, err)
		}
		if resp != nil {
			t.Fatalf("expected nil response, got %+v", resp)
		}

		// The original context still works, the top-level page did
		// not navigate, and only the child frame's fragment changed.
		var topURL, childHash string
		if err := Run(ctx,
			Location(&topURL),
			Evaluate(`document.getElementById('frame-a').contentWindow.location.hash`, &childHash),
		); err != nil {
			t.Fatal(err)
		}
		if want := fs.s.URL + "/"; topURL != want {
			t.Errorf("top-level URL: want %q, got %q", want, topURL)
		}
		if want := "#frag"; childHash != want {
			t.Errorf("child fragment: want %q, got %q", want, childHash)
		}
	})

	t.Run("SiblingNavigationDoesNotEndFrameWait", func(t *testing.T) {
		ctx, fs := setupFrameResponseTest(t)
		frameA, frameB := frameTestIframeIDs(t, ctx)

		navSeen := make(chan struct{})
		var navOnce sync.Once
		lctx, lcancel := context.WithCancel(ctx)
		defer lcancel()
		ListenTarget(lctx, func(ev any) {
			if ev, ok := ev.(*page.EventFrameNavigated); ok &&
				ev.Frame.ID == frameB && strings.Contains(ev.Frame.URL, "who=b2") {
				navOnce.Do(func() { close(navSeen) })
			}
		})

		// The action navigates the sibling frame only. Once that
		// navigation has committed, cancel the wait; the sibling's
		// navigation must not have finished it.
		waitCtx, cancelWait := context.WithCancel(ctx)
		defer cancelWait()
		go func() {
			<-navSeen
			cancelWait()
		}()

		resp, err := RunResponseForFrame(waitCtx, frameA,
			Evaluate(`document.getElementById('frame-b').src = '/frame?who=b2'`, nil))
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("expected context.Canceled, got resp=%v err=%v", resp, err)
		}
		if resp != nil {
			t.Fatalf("expected nil response, got %+v", resp)
		}

		// The original context still works, the sibling navigated,
		// and the selected frame kept its document.
		var frameAState, frameBSearch string
		if err := Run(ctx,
			Evaluate(`document.getElementById('frame-a').contentDocument.getElementById('state').textContent`, &frameAState),
			Evaluate(`document.getElementById('frame-b').contentWindow.location.search`, &frameBSearch),
		); err != nil {
			t.Fatal(err)
		}
		if want := "frame a original"; frameAState != want {
			t.Errorf("frame-a DOM: want %q, got %q", want, frameAState)
		}
		if want := "?who=b2"; frameBSearch != want {
			t.Errorf("frame-b location: want %q, got %q", want, frameBSearch)
		}
		checkFrameCtxUsable(t, ctx, fs.s.URL+"/")
	})

	t.Run("ConcurrentWaitsCancelOne", func(t *testing.T) {
		ctx, fs := setupFrameResponseTest(t)
		frameA, frameB := frameTestIframeIDs(t, ctx)

		// The signal actions run after each wait has registered its
		// listener, so both waits are known to be in place before
		// one of them is cancelled.
		listening1 := make(chan struct{})
		listening2 := make(chan struct{})
		res1 := make(chan frameWaitResult, 1)
		res2 := make(chan frameWaitResult, 1)

		waitCtx, cancelWait := context.WithCancel(ctx)
		defer cancelWait()
		go func() {
			resp, err := RunResponseForFrame(waitCtx, frameA,
				ActionFunc(func(context.Context) error {
					close(listening1)
					return nil
				}))
			res1 <- frameWaitResult{resp, err}
		}()
		go func() {
			resp, err := RunResponseForFrame(ctx, frameB,
				ActionFunc(func(context.Context) error {
					close(listening2)
					return nil
				}),
				Evaluate(`document.getElementById('frame-b').src = '/frame?who=b2'`, nil))
			res2 <- frameWaitResult{resp, err}
		}()

		waitForFrameSignal(t, listening1, "the first wait to start")
		waitForFrameSignal(t, listening2, "the second wait to start")

		// Cancel only the first wait; the second wait's listener
		// must stay in place and the browser context must survive.
		cancelWait()

		select {
		case r := <-res1:
			if !errors.Is(r.err, context.Canceled) {
				t.Fatalf("first wait: expected context.Canceled, got resp=%v err=%v", r.resp, r.err)
			}
			if r.resp != nil {
				t.Fatalf("first wait: expected nil response, got %+v", r.resp)
			}
		case <-time.After(10 * time.Second):
			t.Fatal("first wait did not return after cancellation")
		}

		select {
		case r := <-res2:
			if r.err != nil {
				t.Fatalf("second wait: %v", r.err)
			}
			if r.resp == nil {
				t.Fatal("second wait: expected a response, got nil")
			}
			if want := fs.s.URL + "/frame?who=b2"; r.resp.URL != want {
				t.Errorf("second wait response URL: want %q, got %q", want, r.resp.URL)
			}
			if r.resp.Status != 200 {
				t.Errorf("second wait response status: want 200, got %d", r.resp.Status)
			}
		case <-time.After(10 * time.Second):
			t.Fatal("second wait did not return")
		}

		// The first frame never navigated, and the original context
		// still works.
		var frameASearch string
		if err := Run(ctx,
			Evaluate(`document.getElementById('frame-a').contentWindow.location.search`, &frameASearch),
		); err != nil {
			t.Fatal(err)
		}
		if want := "?who=a"; frameASearch != want {
			t.Errorf("frame-a location: want %q, got %q", want, frameASearch)
		}
		checkFrameCtxUsable(t, ctx, fs.s.URL+"/")
	})
}

func TestFrameResponseExitPaths(t *testing.T) {
	t.Parallel()

	t.Run("FragmentNavigationReturnsNilNil", func(t *testing.T) {
		ctx, fs := setupFrameResponseTest(t)
		frameA, _ := frameTestIframeIDs(t, ctx)

		resp, err := RunResponseForFrame(ctx, frameA,
			Evaluate(`document.getElementById('frame-a').contentWindow.location.hash = 'frag'`, nil))
		if err != nil {
			t.Fatalf("expected nil error, got %v", err)
		}
		if resp != nil {
			t.Fatalf("expected nil response, got %+v", resp)
		}

		var childHash string
		if err := Run(ctx,
			Evaluate(`document.getElementById('frame-a').contentWindow.location.hash`, &childHash),
		); err != nil {
			t.Fatal(err)
		}
		if want := "#frag"; childHash != want {
			t.Errorf("child fragment: want %q, got %q", want, childHash)
		}
		checkFrameCtxUsable(t, ctx, fs.s.URL+"/")
	})

	t.Run("DocumentLoadFailure", func(t *testing.T) {
		ctx, fs := setupFrameResponseTest(t)
		frameA, _ := frameTestIframeIDs(t, ctx)

		// Use the local http server as https, which fails to load
		// with a TLS error. The wait must surface the browser error
		// instead of a seemingly successful response.
		badURL := strings.Replace(fs.s.URL, "http://", "https://", 1) + "/"
		resp, err := RunResponseForFrame(ctx, frameA,
			Evaluate(fmt.Sprintf(`document.getElementById('frame-a').src = %q`, badURL), nil))
		if err == nil || !strings.Contains(err.Error(), "ERR_SSL_PROTOCOL_ERROR") {
			t.Fatalf("expected an ERR_SSL_PROTOCOL_ERROR error, got resp=%v err=%v", resp, err)
		}
		if resp != nil {
			t.Fatalf("expected nil response, got %+v", resp)
		}
		checkFrameCtxUsable(t, ctx, fs.s.URL+"/")
	})

	t.Run("FrameRemoved", func(t *testing.T) {
		ctx, fs := setupFrameResponseTest(t)
		frameA, _ := frameTestIframeIDs(t, ctx)

		// The signal action runs after the wait's listener is in
		// place, so the frame is definitely being waited on when it
		// is removed.
		listening := make(chan struct{})
		resCh := make(chan frameWaitResult, 1)
		go func() {
			resp, err := RunResponseForFrame(ctx, frameA,
				ActionFunc(func(context.Context) error {
					close(listening)
					return nil
				}))
			resCh <- frameWaitResult{resp, err}
		}()
		waitForFrameSignal(t, listening, "the wait to start")

		if err := Run(ctx, Evaluate(`document.getElementById('frame-a').remove()`, nil)); err != nil {
			t.Fatal(err)
		}

		select {
		case r := <-resCh:
			if r.err == nil || !strings.Contains(r.err.Error(), string(frameA)) {
				t.Fatalf("expected an error containing the frame ID %q, got resp=%v err=%v", frameA, r.resp, r.err)
			}
			if r.resp != nil {
				t.Fatalf("expected nil response, got %+v", r.resp)
			}
		case <-time.After(10 * time.Second):
			t.Fatal("wait did not end after the frame was removed")
		}
		checkFrameCtxUsable(t, ctx, fs.s.URL+"/")
	})

	t.Run("ActionError", func(t *testing.T) {
		ctx, fs := setupFrameResponseTest(t)
		frameA, _ := frameTestIframeIDs(t, ctx)

		sentinel := errors.New("sentinel action error")
		count := 0
		resp, err := RunResponseForFrame(ctx, frameA,
			ActionFunc(func(context.Context) error {
				count++
				return sentinel
			}))
		if !errors.Is(err, sentinel) {
			t.Fatalf("expected the action's own error, got resp=%v err=%v", resp, err)
		}
		if resp != nil {
			t.Fatalf("expected nil response, got %+v", resp)
		}
		if count != 1 {
			t.Fatalf("action ran %d times, want exactly 1", count)
		}
		checkFrameCtxUsable(t, ctx, fs.s.URL+"/")
	})

	t.Run("InvalidFrameIDs", func(t *testing.T) {
		ctx, fs := setupFrameResponseTest(t)
		frameA, _ := frameTestIframeIDs(t, ctx)

		// An empty frame ID is rejected before any action runs.
		if resp, err := RunResponseForFrame(ctx, ""); err == nil || resp != nil {
			t.Fatalf("empty frame ID: expected an error and nil response, got resp=%v err=%v", resp, err)
		}

		// An ID which does not belong to this target is rejected
		// before any action runs.
		count := 0
		resp, err := RunResponseForFrame(ctx, "no-such-frame",
			ActionFunc(func(context.Context) error {
				count++
				return nil
			}))
		if err == nil || resp != nil {
			t.Fatalf("unknown frame ID: expected an error and nil response, got resp=%v err=%v", resp, err)
		}
		if count != 0 {
			t.Fatalf("action ran %d times for an invalid frame ID, want 0", count)
		}

		// Remove the iframe and wait until the browser reports the
		// detach, so that the ID is known to be stale.
		detached := make(chan struct{})
		var detachOnce sync.Once
		lctx, lcancel := context.WithCancel(ctx)
		defer lcancel()
		ListenTarget(lctx, func(ev any) {
			if ev, ok := ev.(*page.EventFrameDetached); ok && ev.FrameID == frameA {
				detachOnce.Do(func() { close(detached) })
			}
		})
		if err := Run(ctx, Evaluate(`document.getElementById('frame-a').remove()`, nil)); err != nil {
			t.Fatal(err)
		}
		waitForFrameSignal(t, detached, "the frame to be detached")

		// The ID of the removed frame is rejected before any action
		// runs, and the error reports the ID.
		resp, err = RunResponseForFrame(ctx, frameA)
		if err == nil || resp != nil {
			t.Fatalf("removed frame ID: expected an error and nil response, got resp=%v err=%v", resp, err)
		}
		if !strings.Contains(err.Error(), string(frameA)) {
			t.Fatalf("expected the error to contain the frame ID %q, got %v", frameA, err)
		}
		checkFrameCtxUsable(t, ctx, fs.s.URL+"/")
	})

	t.Run("NoNavigationWaitsForCancel", func(t *testing.T) {
		ctx, fs := setupFrameResponseTest(t)
		frameA, _ := frameTestIframeIDs(t, ctx)

		// The actions don't navigate the frame at all, so the wait
		// only ends when the context is cancelled.
		listening := make(chan struct{})
		waitCtx, cancelWait := context.WithCancel(ctx)
		defer cancelWait()
		resCh := make(chan frameWaitResult, 1)
		go func() {
			resp, err := RunResponseForFrame(waitCtx, frameA,
				ActionFunc(func(context.Context) error {
					close(listening)
					return nil
				}))
			resCh <- frameWaitResult{resp, err}
		}()
		waitForFrameSignal(t, listening, "the wait to start")
		cancelWait()

		select {
		case r := <-resCh:
			if !errors.Is(r.err, context.Canceled) {
				t.Fatalf("expected context.Canceled, got resp=%v err=%v", r.resp, r.err)
			}
			if r.resp != nil {
				t.Fatalf("expected nil response, got %+v", r.resp)
			}
		case <-time.After(10 * time.Second):
			t.Fatal("wait did not end after the context was cancelled")
		}
		checkFrameCtxUsable(t, ctx, fs.s.URL+"/")
	})
}
