package soup

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
	"testing"

	"github.com/anaskhan96/soup/task_support/taskcheck"
)

// frameTransport lets the tests observe and control every request.
type frameTransport func(*http.Request) (*http.Response, error)

func (f frameTransport) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

// frameBody counts Close calls.
type frameBody struct {
	io.Reader
	closed *int
}

func (b *frameBody) Close() error { *b.closed++; return nil }

// frameBrokenReader fails every read.
type frameBrokenReader struct{}

func (frameBrokenReader) Read([]byte) (int, error) { return 0, errFrameRead }

var errFrameRead = errors.New("fixture read failure")

// frameResp builds a response for the frame collector tests. closed counts how
// many response bodies were closed by the implementation.
func frameResp(r *http.Request, status int, body string, closed *int) *http.Response {
	return &http.Response{
		StatusCode: status,
		Header:     make(http.Header),
		Body:       &frameBody{Reader: strings.NewReader(body), closed: closed},
		Request:    r,
	}
}

func sameStrings(a, b []string) bool {
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

// TestFrameSources covers srcdoc precedence, base inheritance, template
// isolation and blank frames.
func TestFrameSources(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		var requests []string
		closed := 0
		rootBody := `<html><head>` +
			`<template><base href="https://tpl.test/x/"><iframe src="tpl-frame"></iframe></template>` +
			`<base href="javascript:void(0)">` +
			`<base href="../assets/">` +
			`<base href="/ignored/">` +
			`</head><body>` +
			`<iframe id="one" srcdoc="&lt;base href='inline/'&gt;&lt;p&gt;plain &amp;amp; simple&lt;/p&gt;&lt;iframe srcdoc='&lt;i&gt;nested&lt;/i&gt;'&gt;&lt;/iframe&gt;&lt;iframe src='sub'&gt;&lt;/iframe&gt;"></iframe>` +
			`<iframe id="two" srcdoc="" src="never"></iframe>` +
			`<iframe id="three" src=" "></iframe>` +
			`<iframe id="four"></iframe>` +
			`<iframe id="five" src="about:blank"></iframe>` +
			`<iframe id="eight" src="about:blank#fragment"></iframe>` +
			`<iframe id="six" srcdoc="&amp;lt;b&amp;gt;literal&amp;lt;/b&amp;gt;" src="never2"></iframe>` +
			`<iframe id="seven" src="deep/leaf">fallback <iframe src="hidden"></iframe></iframe>` +
			`</body></html>`
		client := &http.Client{Transport: frameTransport(func(r *http.Request) (*http.Response, error) {
			requests = append(requests, r.URL.String())
			switch r.URL.Path {
			case "/root/index.html":
				return frameResp(r, 200, rootBody, &closed), nil
			case "/assets/inline/sub":
				return frameResp(r, 200, `<html><body><p id="sub">sub</p></body></html>`, &closed), nil
			case "/assets/deep/leaf":
				return frameResp(r, 200, `<html><body><p id="leaf">leaf</p></body></html>`, &closed), nil
			default:
				return nil, fmt.Errorf("unexpected request %s", r.URL)
			}
		})}

		page, err := CollectFramesWithClient(context.Background(), "https://sources.test/root/index.html", client)
		c.Check(err == nil, "collect failed: %v", err)
		c.Check(page != nil, "nil page without error")
		if err != nil || page == nil {
			return
		}

		c.Check(page.URL == "https://sources.test/root/index.html", "root URL %q", page.URL)
		c.Check(page.BaseURL == "https://sources.test/assets/", "root base %q", page.BaseURL)
		c.Check(page.Cycle == false, "root reported as cycle")
		c.Check(len(page.Frames) == 8, "root frame count %d, want 8", len(page.Frames))

		// srcdoc wins over src and carries its own base; children inherit it.
		one := page.Frames[0]
		c.Check(one.URL == "about:srcdoc", "srcdoc URL %q", one.URL)
		c.Check(one.BaseURL == "https://sources.test/assets/inline/", "srcdoc base %q", one.BaseURL)
		c.Check(one.Document.Find("p").Text() == "plain & simple", "srcdoc text %q", one.Document.Find("p").Text())
		c.Check(len(one.Frames) == 2, "srcdoc frame count %d", len(one.Frames))
		c.Check(one.Frames[0].URL == "about:srcdoc", "nested srcdoc URL %q", one.Frames[0].URL)
		c.Check(one.Frames[0].BaseURL == "https://sources.test/assets/inline/", "nested srcdoc base %q", one.Frames[0].BaseURL)
		c.Check(one.Frames[0].Document.Find("i").Text() == "nested", "nested srcdoc text %q", one.Frames[0].Document.Find("i").Text())
		c.Check(one.Frames[1].URL == "https://sources.test/assets/inline/sub", "inherited base request URL %q", one.Frames[1].URL)
		c.Check(one.Frames[1].Document.Find("p").Text() == "sub", "nested request text %q", one.Frames[1].Document.Find("p").Text())

		// Empty srcdoc still takes precedence; src is never requested.
		two := page.Frames[1]
		c.Check(two.URL == "about:srcdoc", "empty srcdoc URL %q", two.URL)
		c.Check(two.Document.Error == nil && two.Document.Pointer != nil, "empty srcdoc has no DOM")
		c.Check(len(two.Frames) == 0, "empty srcdoc has frames")

		// Blank sources produce an empty DOM inheriting the parent base.
		for i := 2; i <= 5; i++ {
			blank := page.Frames[i]
			c.Check(blank.URL == "about:blank", "blank frame %d URL %q", i, blank.URL)
			c.Check(blank.BaseURL == "https://sources.test/assets/", "blank frame %d base %q", i, blank.BaseURL)
			c.Check(blank.Cycle == false, "blank frame %d marked as cycle", i)
			c.Check(blank.Document.Pointer != nil, "blank frame %d missing DOM", i)
			c.Check(len(blank.Document.FindAll("p")) == 0, "blank frame %d not empty", i)
			c.Check(len(blank.Frames) == 0, "blank frame %d has frames", i)
		}

		// The parser decoded the attribute exactly once: the markup stays text.
		six := page.Frames[6]
		c.Check(six.URL == "about:srcdoc", "literal srcdoc URL %q", six.URL)
		c.Check(six.Document.Find("b").Error != nil, "srcdoc entity was decoded twice into an element")
		c.Check(strings.Contains(page.Frames[6].Document.FullText(), "<b>literal</b>"), "literal text %q", six.Document.FullText())

		seven := page.Frames[7]
		c.Check(seven.URL == "https://sources.test/assets/deep/leaf", "relative src URL %q", seven.URL)
		c.Check(seven.Document.Find("p").Text() == "leaf", "relative src text %q", seven.Document.Find("p").Text())

		want := []string{
			"https://sources.test/root/index.html",
			"https://sources.test/assets/inline/sub",
			"https://sources.test/assets/deep/leaf",
		}
		c.Check(sameStrings(requests, want), "request order %v, want %v", requests, want)
		c.Check(closed == 3, "closed bodies %d, want 3", closed)
	})
}

// TestFrameIdentity covers independent documents over shared downloads,
// srcdoc expansion under different bases, and ancestor cycles.
func TestFrameIdentity(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		var requests []string
		closed := 0
		rootBody := `<html><body>` +
			`<iframe id="s1" src="shared?x=1#top"></iframe>` +
			`<iframe id="s2" src="./shared?x=1"></iframe>` +
			`<iframe id="s3" src="shared?x=1"></iframe>` +
			`<iframe id="a" src="a/page"></iframe>` +
			`<iframe id="b" src="b/page"></iframe>` +
			`<iframe id="alias" src="alias/page"></iframe>` +
			`<iframe id="direct" src="loop/direct"></iframe>` +
			`<iframe id="redir" src="loop/hop"></iframe>` +
			`</body></html>`
		client := &http.Client{Transport: frameTransport(func(r *http.Request) (*http.Response, error) {
			requests = append(requests, r.URL.String())
			switch r.URL.Path {
			case "/root.html":
				return frameResp(r, 200, rootBody, &closed), nil
			case "/shared":
				return frameResp(r, 200, `<html><body><h1>Shared</h1><iframe src="leaf"></iframe></body></html>`, &closed), nil
			case "/leaf":
				return frameResp(r, 200, `<html><body><p>leaf</p></body></html>`, &closed), nil
			case "/a/page":
				return frameResp(r, 200, `<html><head><base href="/a/"></head><body><iframe srcdoc="&lt;iframe src='leaf'&gt;&lt;/iframe&gt;"></iframe></body></html>`, &closed), nil
			case "/b/page":
				return frameResp(r, 200, `<html><head><base href="/b/"></head><body><iframe srcdoc="&lt;iframe src='leaf'&gt;&lt;/iframe&gt;"></iframe></body></html>`, &closed), nil
			case "/a/leaf":
				return frameResp(r, 200, `<html><body><p>a leaf</p></body></html>`, &closed), nil
			case "/b/leaf":
				return frameResp(r, 200, `<html><body><p>b leaf</p></body></html>`, &closed), nil
			case "/loop/direct":
				return frameResp(r, 200, `<html><body><iframe src="/root.html"></iframe></body></html>`, &closed), nil
			case "/loop/hop":
				resp := frameResp(r, 302, "redirect", &closed)
				resp.Header.Set("Location", "/root.html#frag")
				return resp, nil
			case "/alias/page":
				// Redirects to an address already downloaded through a sibling.
				resp := frameResp(r, 302, "redirect", &closed)
				resp.Header.Set("Location", "/shared?x=1")
				return resp, nil
			default:
				return nil, fmt.Errorf("unexpected request %s", r.URL)
			}
		})}

		page, err := CollectFramesWithClient(context.Background(), "https://identity.test/root.html#start", client)
		c.Check(err == nil, "collect failed: %v", err)
		c.Check(page != nil, "nil page without error")
		if err != nil || page == nil {
			return
		}
		c.Check(page.URL == "https://identity.test/root.html", "root URL kept the fragment: %q", page.URL)
		c.Check(len(page.Frames) == 8, "root frame count %d, want 8", len(page.Frames))

		// Three frames share one download but own separate documents.
		for i := 0; i < 3; i++ {
			shared := page.Frames[i]
			c.Check(shared.URL == "https://identity.test/shared?x=1", "shared frame %d URL %q", i, shared.URL)
			c.Check(shared.Cycle == false, "shared frame %d marked as cycle", i)
			c.Check(len(shared.Frames) == 1, "shared frame %d child count %d", i, len(shared.Frames))
			c.Check(shared.Frames[0].URL == "https://identity.test/leaf", "shared frame %d child URL %q", i, shared.Frames[0].URL)
		}
		h1a := page.Frames[0].Document.Find("h1")
		h1b := page.Frames[1].Document.Find("h1")
		c.Check(h1a.Pointer != h1b.Pointer, "shared download reused one DOM pointer")
		h1a.Pointer.FirstChild.Data = "Changed"
		c.Check(page.Frames[1].Document.Find("h1").Text() == "Shared", "mutating one DOM changed another: %q", page.Frames[1].Document.Find("h1").Text())
		page.Frames[0].Frames[0].URL = "https://mutated.test/"
		c.Check(page.Frames[1].Frames[0].URL == "https://identity.test/leaf", "mutating one frame slice changed another: %q", page.Frames[1].Frames[0].URL)

		// Identical srcdoc text under different parent bases fetches different
		// relative addresses.
		aPage, bPage := page.Frames[3], page.Frames[4]
		c.Check(aPage.BaseURL == "https://identity.test/a/", "a base %q", aPage.BaseURL)
		c.Check(bPage.BaseURL == "https://identity.test/b/", "b base %q", bPage.BaseURL)
		aSrcdoc := aPage.Frames[0]
		bSrcdoc := bPage.Frames[0]
		c.Check(aSrcdoc.URL == "about:srcdoc" && bSrcdoc.URL == "about:srcdoc", "srcdoc URLs %q %q", aSrcdoc.URL, bSrcdoc.URL)
		c.Check(aSrcdoc.BaseURL == "https://identity.test/a/", "a srcdoc base %q", aSrcdoc.BaseURL)
		c.Check(bSrcdoc.BaseURL == "https://identity.test/b/", "b srcdoc base %q", bSrcdoc.BaseURL)
		c.Check(len(aSrcdoc.Frames) == 1 && aSrcdoc.Frames[0].URL == "https://identity.test/a/leaf", "a srcdoc child %v", aSrcdoc.Frames)
		c.Check(len(bSrcdoc.Frames) == 1 && bSrcdoc.Frames[0].URL == "https://identity.test/b/leaf", "b srcdoc child %v", bSrcdoc.Frames)

		// A sibling that redirects to an address already downloaded reuses the
		// body and still builds its own document.
		alias := page.Frames[5]
		c.Check(alias.URL == "https://identity.test/shared?x=1", "alias URL %q", alias.URL)
		c.Check(alias.Document.Find("h1").Text() == "Shared", "alias text %q", alias.Document.Find("h1").Text())
		c.Check(len(alias.Frames) == 1, "alias child count %d", len(alias.Frames))
		c.Check(alias.Document.Find("h1").Pointer != h1a.Pointer, "alias reused a sibling DOM")

		// A direct edge back to the ancestor becomes a positioned cycle.
		direct := page.Frames[6].Frames[0]
		c.Check(direct.Cycle == true, "direct ancestor edge not marked as cycle")
		c.Check(direct.URL == "https://identity.test/root.html", "direct cycle URL %q", direct.URL)
		c.Check(direct.BaseURL == "https://identity.test/root.html", "direct cycle base %q", direct.BaseURL)
		c.Check(direct.Document.Pointer == nil, "direct cycle carries a document")
		c.Check(len(direct.Frames) == 0, "direct cycle expanded %d frames", len(direct.Frames))

		// An edge that only reaches an ancestor through a redirect also becomes
		// a cycle, and the ancestor body is never fetched twice.
		redir := page.Frames[7]
		c.Check(redir.Cycle == true, "redirect ancestor edge not marked as cycle")
		c.Check(redir.URL == "https://identity.test/root.html", "redirect cycle URL %q", redir.URL)
		c.Check(redir.Document.Pointer == nil, "redirect cycle carries a document")
		c.Check(len(redir.Frames) == 0, "redirect cycle expanded %d frames", len(redir.Frames))
		c.Check(page.Document.Find("iframe", "id", "s1").Error == nil, "cycle replaced the ancestor document")
		c.Check(page.Document.Find("h1").Error != nil, "cycle merged the shared body into the ancestor")
		c.Check(len(page.Frames) == 8, "cycle lost its position: %d frames", len(page.Frames))
		rootRequests := 0
		sharedRequests := 0
		for _, got := range requests {
			if got == "https://identity.test/root.html" {
				rootRequests++
			}
			if got == "https://identity.test/shared?x=1" {
				sharedRequests++
			}
		}
		c.Check(rootRequests == 1, "ancestor body downloaded %d times", rootRequests)
		c.Check(sharedRequests == 1, "shared body downloaded %d times", sharedRequests)

		want := []string{
			"https://identity.test/root.html",
			"https://identity.test/shared?x=1",
			"https://identity.test/leaf",
			"https://identity.test/a/page",
			"https://identity.test/a/leaf",
			"https://identity.test/b/page",
			"https://identity.test/b/leaf",
			"https://identity.test/alias/page",
			"https://identity.test/loop/direct",
			"https://identity.test/loop/hop",
		}
		c.Check(sameStrings(requests, want), "request order %v, want %v", requests, want)
		c.Check(closed == len(requests), "closed bodies %d, want %d", closed, len(requests))
	})
}

type lifecycleCtxKey struct{}

// TestFrameLifecycle covers failure stopping, cancellation, per-call isolation
// and the untouched legacy entry point.
func TestFrameLifecycle(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// A failure stops the walk: no later sibling, all bodies closed.
		var reqs []string
		closed := 0
		client := &http.Client{Transport: frameTransport(func(r *http.Request) (*http.Response, error) {
			reqs = append(reqs, r.URL.Path)
			switch r.URL.Path {
			case "/root":
				return frameResp(r, 200, `<html><body><iframe src="ok1"></iframe><iframe src="bad"></iframe><iframe src="later"></iframe></body></html>`, &closed), nil
			case "/ok1":
				return frameResp(r, 200, `<html><body><p>ok</p></body></html>`, &closed), nil
			case "/bad":
				return frameResp(r, 503, `<p>bad</p>`, &closed), nil
			default:
				return nil, fmt.Errorf("unexpected request %s", r.URL)
			}
		})}
		page, err := CollectFramesWithClient(context.Background(), "https://life.test/root", client)
		c.Check(page == nil, "failed collection returned a tree")
		c.Check(err != nil, "non-2xx response accepted")
		if err != nil {
			c.Check(strings.Contains(err.Error(), "frames[1]"), "missing embedding path: %v", err)
			c.Check(strings.Contains(err.Error(), "https://life.test/bad"), "missing failing address: %v", err)
		}
		c.Check(sameStrings(reqs, []string{"/root", "/ok1", "/bad"}), "requests after failure %v", reqs)
		c.Check(closed == 3, "closed bodies %d, want 3", closed)

		// Nested paths are reported as frames[i].frames[j].
		var nestedReqs []string
		nestedClosed := 0
		nestedClient := &http.Client{Transport: frameTransport(func(r *http.Request) (*http.Response, error) {
			nestedReqs = append(nestedReqs, r.URL.Path)
			switch r.URL.Path {
			case "/root2":
				return frameResp(r, 200, `<html><body><iframe src="mid"></iframe><iframe src="later2"></iframe></body></html>`, &nestedClosed), nil
			case "/mid":
				return frameResp(r, 200, `<html><body><iframe src="ok2"></iframe><iframe src="bad2"></iframe></body></html>`, &nestedClosed), nil
			case "/ok2":
				return frameResp(r, 200, `<html><body><p>ok</p></body></html>`, &nestedClosed), nil
			case "/bad2":
				return frameResp(r, 500, `<p>bad</p>`, &nestedClosed), nil
			default:
				return nil, fmt.Errorf("unexpected request %s", r.URL)
			}
		})}
		page, err = CollectFramesWithClient(context.Background(), "https://life.test/root2", nestedClient)
		c.Check(page == nil && err != nil, "nested failure: page=%v err=%v", page, err)
		if err != nil {
			c.Check(strings.Contains(err.Error(), "frames[0].frames[1]"), "nested path missing: %v", err)
		}
		c.Check(sameStrings(nestedReqs, []string{"/root2", "/mid", "/ok2", "/bad2"}), "nested request order %v", nestedReqs)
		c.Check(nestedClosed == 4, "nested closed bodies %d, want 4", nestedClosed)

		// A body read failure closes its body and stops the walk.
		var readReqs []string
		readClosed := 0
		readClient := &http.Client{Transport: frameTransport(func(r *http.Request) (*http.Response, error) {
			readReqs = append(readReqs, r.URL.Path)
			switch r.URL.Path {
			case "/root3":
				return frameResp(r, 200, `<html><body><iframe src="broken"></iframe><iframe src="later3"></iframe></body></html>`, &readClosed), nil
			case "/broken":
				return &http.Response{StatusCode: 200, Header: make(http.Header), Body: &frameBody{Reader: frameBrokenReader{}, closed: &readClosed}, Request: r}, nil
			default:
				return nil, fmt.Errorf("unexpected request %s", r.URL)
			}
		})}
		page, err = CollectFramesWithClient(context.Background(), "https://life.test/root3", readClient)
		c.Check(page == nil && err != nil, "read failure: page=%v err=%v", page, err)
		if err != nil {
			c.Check(strings.Contains(err.Error(), "frames[0]"), "read failure path missing: %v", err)
		}
		c.Check(sameStrings(readReqs, []string{"/root3", "/broken"}), "requests after read failure %v", readReqs)
		c.Check(readClosed == 2, "read failure closed bodies %d, want 2", readClosed)

		// A context already cancelled issues no requests.
		var cancelReqs []string
		cancelClosed := 0
		cancelClient := &http.Client{Transport: frameTransport(func(r *http.Request) (*http.Response, error) {
			cancelReqs = append(cancelReqs, r.URL.Path)
			return frameResp(r, 200, "<html><body></body></html>", &cancelClosed), nil
		})}
		ctx, cancel := context.WithCancel(context.Background())
		cancel()
		page, err = CollectFramesWithClient(ctx, "https://life.test/never", cancelClient)
		c.Check(page == nil && err != nil, "cancelled call: page=%v err=%v", page, err)
		c.Check(errors.Is(err, context.Canceled), "cancelled error not preserved: %v", err)
		c.Check(len(cancelReqs) == 0, "cancelled call issued requests %v", cancelReqs)

		// Cancellation between frames stops the walk and keeps the path.
		var midReqs []string
		midClosed := 0
		var midCtx context.Context
		var midCancel context.CancelFunc
		midCtx, midCancel = context.WithCancel(context.Background())
		midClient := &http.Client{Transport: frameTransport(func(r *http.Request) (*http.Response, error) {
			midReqs = append(midReqs, r.URL.Path)
			switch r.URL.Path {
			case "/cancelroot":
				return frameResp(r, 200, `<html><body><iframe src="c1"></iframe><iframe src="stop"></iframe><iframe src="after"></iframe></body></html>`, &midClosed), nil
			case "/c1":
				return frameResp(r, 200, `<html><body><p>ok</p></body></html>`, &midClosed), nil
			case "/stop":
				midCancel()
				return nil, midCtx.Err()
			default:
				return nil, fmt.Errorf("unexpected request after cancel: %s", r.URL)
			}
		})}
		page, err = CollectFramesWithClient(midCtx, "https://life.test/cancelroot", midClient)
		c.Check(page == nil && err != nil, "mid-cancel call: page=%v err=%v", page, err)
		c.Check(errors.Is(err, context.Canceled), "mid-cancel error not preserved: %v", err)
		if err != nil {
			c.Check(strings.Contains(err.Error(), "frames[1]"), "mid-cancel path missing: %v", err)
		}
		c.Check(sameStrings(midReqs, []string{"/cancelroot", "/c1", "/stop"}), "mid-cancel requests %v", midReqs)
		c.Check(midClosed == 2, "mid-cancel closed bodies %d, want 2", midClosed)

		// A non-HTTP child address is rejected with its path and address.
		var invalidReqs []string
		invalidClosed := 0
		invalidClient := &http.Client{Transport: frameTransport(func(r *http.Request) (*http.Response, error) {
			invalidReqs = append(invalidReqs, r.URL.Path)
			if r.URL.Path == "/ok3" {
				return frameResp(r, 200, `<html><body><p>ok</p></body></html>`, &invalidClosed), nil
			}
			return frameResp(r, 200, `<html><body><iframe src="ok3"></iframe><iframe src="javascript:void(0)"></iframe><iframe src="later4"></iframe></body></html>`, &invalidClosed), nil
		})}
		page, err = CollectFramesWithClient(context.Background(), "https://life.test/root4", invalidClient)
		c.Check(page == nil && err != nil, "invalid child address: page=%v err=%v", page, err)
		if err != nil {
			c.Check(strings.Contains(err.Error(), "frames[1]"), "invalid address path missing: %v", err)
			c.Check(strings.Contains(err.Error(), "javascript:void(0)"), "invalid address missing from error: %v", err)
		}
		c.Check(sameStrings(invalidReqs, []string{"/root4", "/ok3"}), "requests after invalid address %v", invalidReqs)

		// A transport error remains discoverable with errors.Is.
		sentinel := errors.New("dial failed")
		netClient := &http.Client{Transport: frameTransport(func(r *http.Request) (*http.Response, error) {
			if r.URL.Path == "/netroot" {
				return frameResp(r, 200, `<html><body><iframe src="net"></iframe></body></html>`, &closed), nil
			}
			return nil, sentinel
		})}
		page, err = CollectFramesWithClient(context.Background(), "https://life.test/netroot", netClient)
		c.Check(page == nil && err != nil, "network failure: page=%v err=%v", page, err)
		c.Check(errors.Is(err, sentinel), "network error not preserved: %v", err)

		// Downloads are cached per call only.
		var isoReqs []string
		isoClosed := 0
		isoClient := &http.Client{Transport: frameTransport(func(r *http.Request) (*http.Response, error) {
			isoReqs = append(isoReqs, r.URL.Path)
			switch r.URL.Path {
			case "/iso":
				return frameResp(r, 200, `<html><body><iframe src="shared"></iframe></body></html>`, &isoClosed), nil
			case "/shared":
				return frameResp(r, 200, `<html><body><p>shared</p></body></html>`, &isoClosed), nil
			default:
				return nil, fmt.Errorf("unexpected request %s", r.URL)
			}
		})}
		first, err := CollectFramesWithClient(context.Background(), "https://life.test/iso", isoClient)
		c.Check(err == nil && first != nil && len(first.Frames) == 1, "first isolated call: %v %v", first, err)
		second, err := CollectFramesWithClient(context.Background(), "https://life.test/iso", isoClient)
		c.Check(err == nil && second != nil && len(second.Frames) == 1, "second isolated call: %v %v", second, err)
		c.Check(sameStrings(isoReqs, []string{"/iso", "/shared", "/iso", "/shared"}), "cross-call cache reused: %v", isoReqs)

		// The legacy entry point neither follows frames nor rejects non-2xx.
		var legacyReqs []string
		legacyClosed := 0
		legacyClient := &http.Client{Transport: frameTransport(func(r *http.Request) (*http.Response, error) {
			legacyReqs = append(legacyReqs, r.URL.Path)
			switch r.URL.Path {
			case "/legacy":
				return frameResp(r, 200, `<html><body><iframe src="child"></iframe></body></html>`, &legacyClosed), nil
			case "/legacy-bad":
				return frameResp(r, 503, "Unavailable", &legacyClosed), nil
			default:
				return nil, fmt.Errorf("unexpected request %s", r.URL)
			}
		})}
		text, err := GetWithClient("https://life.test/legacy", legacyClient)
		c.Check(err == nil && strings.Contains(text, "<iframe"), "legacy get: %q %v", text, err)
		c.Check(sameStrings(legacyReqs, []string{"/legacy"}), "legacy followed frames: %v", legacyReqs)
		text, err = GetWithClient("https://life.test/legacy-bad", legacyClient)
		c.Check(err == nil && text == "Unavailable", "legacy non-2xx: %q %v", text, err)
		c.Check(sameStrings(legacyReqs, []string{"/legacy", "/legacy-bad"}), "legacy requests %v", legacyReqs)

		// The new entry point forwards headers, cookies and the context.
		oldHeaders, oldCookies := Headers, Cookies
		Headers = map[string]string{"X-Collect": "frames"}
		Cookies = map[string]string{"vendor": "two"}
		defer func() { Headers, Cookies = oldHeaders, oldCookies }()
		var headerOK, cookieOK, ctxOK bool
		ctx = context.WithValue(context.Background(), lifecycleCtxKey{}, "marker")
		headerClient := &http.Client{Transport: frameTransport(func(r *http.Request) (*http.Response, error) {
			headerOK = r.Header.Get("X-Collect") == "frames"
			cookie, cookieErr := r.Cookie("vendor")
			cookieOK = cookieErr == nil && cookie.Value == "two"
			ctxOK = r.Context().Value(lifecycleCtxKey{}) == "marker"
			body := `<html><body><iframe src="header-child"></iframe></body></html>`
			if r.URL.Path == "/header-child" {
				body = `<html><body><p>child</p></body></html>`
			}
			return frameResp(r, 200, body, &closed), nil
		})}
		page, err = CollectFramesWithClient(ctx, "https://life.test/header", headerClient)
		c.Check(err == nil && page != nil && len(page.Frames) == 1, "header collection: %v %v", page, err)
		c.Check(headerOK, "Headers not forwarded")
		c.Check(cookieOK, "Cookies not forwarded")
		c.Check(ctxOK, "context not forwarded")

		// A nil client falls back to the package default client.
		oldDefault := defaultClient
		defaultSeen := 0
		defaultClosed := 0
		defaultClient = &http.Client{Transport: frameTransport(func(r *http.Request) (*http.Response, error) {
			defaultSeen++
			return frameResp(r, 200, `<html><body><p>default</p></body></html>`, &defaultClosed), nil
		})}
		page, err = CollectFramesWithClient(context.Background(), "https://life.test/default", nil)
		defaultClient = oldDefault
		c.Check(err == nil && page != nil && defaultSeen == 1, "nil client: seen=%d page=%v err=%v", defaultSeen, page, err)
		c.Check(page != nil && page.Document.Find("p").Text() == "default", "nil client DOM: %v", page)
	})
}
