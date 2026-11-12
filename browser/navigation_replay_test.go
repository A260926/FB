package browser

import (
	"bytes"
	"errors"
	"io"
	"mime"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

// readOnly is a forward-only reader: it deliberately hides Seek and
// WriteTo so the browser cannot rewind the original source.
type readOnly struct{ io.Reader }

type brokenSource struct{}

func (brokenSource) Read([]byte) (int, error) { return 0, errors.New("fixture-source-read") }

type brokenBody struct {
	io.Reader
	closed *int
}

func (b *brokenBody) Close() error {
	*b.closed = *b.closed + 1
	return nil
}

type roundTripFunc func(*http.Request) (*http.Response, error)

func (f roundTripFunc) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

func htmlResponse(r *http.Request, html string) *http.Response {
	return &http.Response{
		StatusCode: 200,
		Header:     http.Header{"Content-Type": {"text/html"}},
		Request:    r,
		Body:       io.NopCloser(strings.NewReader(html)),
	}
}

func dirEntries(t *testing.T, dir string) []string {
	t.Helper()
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatalf("reading dir %s: %v", dir, err)
	}
	names := make([]string, 0, len(entries))
	for _, e := range entries {
		names = append(names, e.Name())
	}
	return names
}

// TestNavigationReplay exercises full page-state snapshots and reliable
// request replay against a real loopback server, plus failure handling
// through controllable readers, transports and replay directories.
func TestNavigationReplay(t *testing.T) {
	const (
		pageA = `<html><head><title>A</title><base href="/a/assets/"></head><body><a href="item">A</a></body></html>`
		pageB = `<html><head><title>B</title></head><body><a href="item">B</a></body></html>`
		pageC = `<html><head><title>C</title><base href="assets/"></head><body><a href="item">C</a></body></html>`
	)

	var mu sync.Mutex
	uploadBodies := map[int][]byte{}
	uploadMeta := map[int]struct {
		field    string
		filename string
		fileData []byte
		ctype    string
	}{}
	postBodies := map[int][]byte{}
	postMethods := map[int]string{}
	cookieSeen := map[int]string{}
	var startPosts, receiptGets int

	mux := http.NewServeMux()
	mux.HandleFunc("/form", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		io.WriteString(w, `<form method="POST" action="/upload" enctype="multipart/form-data">`+
			`<input name="q"><input type="file" name="attachment">`+
			`<button type="submit" name="send" value="yes">send</button></form>`)
	})
	mux.HandleFunc("/upload", func(w http.ResponseWriter, r *http.Request) {
		raw, err := io.ReadAll(r.Body)
		if err != nil {
			t.Errorf("reading upload body: %v", err)
		}
		_, params, err := mime.ParseMediaType(r.Header.Get("Content-Type"))
		if err != nil {
			t.Errorf("parsing content type: %v", err)
		}
		mr := multipart.NewReader(bytes.NewReader(raw), params["boundary"])
		form, err := mr.ReadForm(32 << 20)
		if err != nil {
			t.Errorf("parsing multipart: %v", err)
		}
		defer form.RemoveAll()

		idx := len(uploadBodies) + 1
		meta := struct {
			field    string
			filename string
			fileData []byte
			ctype    string
		}{ctype: r.Header.Get("Content-Type")}
		if vs := form.Value["q"]; len(vs) > 0 {
			meta.field = vs[0]
		}
		if fhs := form.File["attachment"]; len(fhs) > 0 {
			meta.filename = fhs[0].Filename
			f, err := fhs[0].Open()
			if err != nil {
				t.Errorf("opening upload: %v", err)
			}
			meta.fileData, _ = io.ReadAll(f)
			f.Close()
		}
		mu.Lock()
		uploadBodies[idx] = raw
		uploadMeta[idx] = meta
		mu.Unlock()
		io.WriteString(w, `<html><title>UPLOAD</title><body>accepted</body></html>`)
	})
	mux.HandleFunc("/post", func(w http.ResponseWriter, r *http.Request) {
		raw, _ := io.ReadAll(r.Body)
		mu.Lock()
		idx := len(postBodies) + 1
		postBodies[idx] = raw
		postMethods[idx] = r.Method
		mu.Unlock()
		io.WriteString(w, `<html><title>POST</title><body>saved</body></html>`)
	})
	mux.HandleFunc("/a/page", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		io.WriteString(w, pageA)
	})
	mux.HandleFunc("/b/page", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		io.WriteString(w, pageB)
	})
	mux.HandleFunc("/c/page", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		io.WriteString(w, pageC)
	})
	mux.HandleFunc("/start", func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		startPosts++
		mu.Unlock()
		http.Redirect(w, r, "/receipt", http.StatusSeeOther)
	})
	mux.HandleFunc("/receipt", func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		receiptGets++
		mu.Unlock()
		io.WriteString(w, `<html><title>RECEIPT</title></html>`)
	})
	mux.HandleFunc("/setcookie", func(w http.ResponseWriter, r *http.Request) {
		http.SetCookie(w, &http.Cookie{Name: "c", Value: "v", Path: "/"})
		mu.Lock()
		cookieSeen[len(cookieSeen)+1] = r.Header.Get("Cookie")
		mu.Unlock()
		io.WriteString(w, `<html><title>COOKIE</title></html>`)
	})

	ts := httptest.NewServer(mux)
	defer ts.Close()

	t.Run("multipart submit replays identical raw body", func(t *testing.T) {
		dir := t.TempDir()
		bow := newDefaultTestBrowser()
		if err := bow.SetReplayStorage(1<<20, dir); err != nil {
			t.Fatal(err)
		}
		defer bow.Close()

		if err := bow.Open(ts.URL + "/form"); err != nil {
			t.Fatal(err)
		}
		form, err := bow.Form("form")
		if err != nil {
			t.Fatal(err)
		}
		if err := form.Input("q", "stations"); err != nil {
			t.Fatal(err)
		}
		attachment := []byte("file bytes")
		if err := form.File("attachment", "query.bin", readOnly{bytes.NewReader(attachment)}); err != nil {
			t.Fatal(err)
		}
		if err := form.Submit(); err != nil {
			t.Fatal(err)
		}
		if bow.Title() != "UPLOAD" {
			t.Fatalf("title = %q, want UPLOAD", bow.Title())
		}
		if err := bow.Reload(); err != nil {
			t.Fatalf("reload: %v", err)
		}
		if bow.Title() != "UPLOAD" {
			t.Fatalf("reloaded title = %q", bow.Title())
		}

		mu.Lock()
		defer mu.Unlock()
		if len(uploadBodies) != 2 {
			t.Fatalf("want two upload requests, got %d", len(uploadBodies))
		}
		if !bytes.Equal(uploadBodies[1], uploadBodies[2]) {
			t.Fatal("reloaded multipart body differs from the first request")
		}
		m1, m2 := uploadMeta[1], uploadMeta[2]
		if m1.field != "stations" || m2.field != "stations" {
			t.Fatalf("multipart field not preserved: %q / %q", m1.field, m2.field)
		}
		if m1.filename != "query.bin" || m2.filename != "query.bin" {
			t.Fatalf("filename not preserved: %q / %q", m1.filename, m2.filename)
		}
		if !bytes.Equal(m1.fileData, attachment) || !bytes.Equal(m2.fileData, attachment) {
			t.Fatal("attachment bytes not preserved across replay")
		}
		if m1.ctype != m2.ctype || !strings.HasPrefix(m1.ctype, "multipart/form-data;") {
			t.Fatalf("content type not preserved: %q / %q", m1.ctype, m2.ctype)
		}
	})

	t.Run("one-shot POST body replays with full bytes", func(t *testing.T) {
		bow := newDefaultTestBrowser()
		defer bow.Close()
		if err := bow.Post(ts.URL+"/post", "application/octet-stream",
			readOnly{strings.NewReader("station=7")}); err != nil {
			t.Fatal(err)
		}
		if got := string(postBodies[1]); got != "station=7" {
			t.Fatalf("first body = %q", got)
		}
		if err := bow.Reload(); err != nil {
			t.Fatal(err)
		}
		if got := string(postBodies[2]); got != "station=7" {
			t.Fatalf("reloaded body = %q, want station=7", got)
		}
		if postMethods[1] != "POST" || postMethods[2] != "POST" {
			t.Fatalf("reload changed method: %q / %q", postMethods[1], postMethods[2])
		}
		if bow.State().Request.Method != "POST" || bow.State().Request.URL.String() != ts.URL+"/post" {
			t.Fatal("stored request lost its method or entry URL")
		}
		if bow.State().Request.Header.Get("Content-Type") != "application/octet-stream" {
			t.Fatal("stored request lost its Content-Type")
		}
	})

	t.Run("reload repeats and works after back without growing history", func(t *testing.T) {
		bow := newDefaultTestBrowser()
		defer bow.Close()
		if err := bow.Open(ts.URL + "/a/page"); err != nil {
			t.Fatal(err)
		}
		if err := bow.Open(ts.URL + "/b/page"); err != nil {
			t.Fatal(err)
		}
		if bow.HistoryJar().Len() != 1 {
			t.Fatalf("history len = %d, want 1", bow.HistoryJar().Len())
		}

		if err := bow.Reload(); err != nil {
			t.Fatal(err)
		}
		if err := bow.Reload(); err != nil {
			t.Fatal(err)
		}
		if bow.Title() != "B" || bow.HistoryJar().Len() != 1 {
			t.Fatalf("reload must replace in place: title=%q len=%d", bow.Title(), bow.HistoryJar().Len())
		}

		if !bow.Back() {
			t.Fatal("Back failed with one history entry")
		}
		if bow.Title() != "A" {
			t.Fatalf("after back title = %q, want A", bow.Title())
		}
		var download bytes.Buffer
		if _, err := bow.Download(&download); err != nil {
			t.Fatal(err)
		}
		if download.String() != pageA {
			t.Fatal("Download after Back returned the wrong page bytes")
		}
		if link := bow.Links()[0].URL.String(); link != ts.URL+"/a/assets/item" {
			t.Fatalf("restored link = %q", link)
		}
		if err := bow.Reload(); err != nil {
			t.Fatalf("reload after back: %v", err)
		}
		if bow.Title() != "A" {
			t.Fatalf("reload after back changed page: %q", bow.Title())
		}
		// Back discards the current page, so B is gone and no older
		// completed page remains.
		if bow.Back() {
			t.Fatal("Back must not expose the empty initial state")
		}

		fresh := newDefaultTestBrowser()
		defer fresh.Close()
		if fresh.Back() {
			t.Fatal("fresh browser must not expose an empty initial state")
		}
	})

	t.Run("base is per page, independent of meta refresh handling", func(t *testing.T) {
		bow := newDefaultTestBrowser()
		defer bow.Close()
		if err := bow.Open(ts.URL + "/a/page"); err != nil {
			t.Fatal(err)
		}
		if got := bow.Links()[0].URL.String(); got != ts.URL+"/a/assets/item" {
			t.Fatalf("base link = %q", got)
		}
		if err := bow.Open(ts.URL + "/b/page"); err != nil {
			t.Fatal(err)
		}
		if got := bow.Links()[0].URL.String(); got != ts.URL+"/b/item" {
			t.Fatalf("page without <base> inherited the previous base: %q", got)
		}
		if !bow.Back() {
			t.Fatal("Back failed")
		}
		if got := bow.Links()[0].URL.String(); got != ts.URL+"/a/assets/item" {
			t.Fatalf("restored base link = %q", got)
		}

		noRefresh := newDefaultTestBrowser()
		defer noRefresh.Close()
		noRefresh.SetAttribute(MetaRefreshHandling, false)
		if err := noRefresh.Open(ts.URL + "/c/page"); err != nil {
			t.Fatal(err)
		}
		if got := noRefresh.Links()[0].URL.String(); got != ts.URL+"/c/assets/item" {
			t.Fatalf("relative base ignored with MetaRefreshHandling off: %q", got)
		}
		if _, err := noRefresh.Form("form"); err == nil {
			t.Fatal("unexpected form on page C")
		}
	})

	t.Run("cookies are recomputed by the jar, never stored as headers", func(t *testing.T) {
		bow := newDefaultTestBrowser()
		defer bow.Close()
		if err := bow.Post(ts.URL+"/setcookie", "text/plain", readOnly{strings.NewReader("first")}); err != nil {
			t.Fatal(err)
		}
		if got := bow.State().Request.Header.Get("Cookie"); got != "" {
			t.Fatalf("cookie pinned into the stored request: %q", got)
		}
		if err := bow.Reload(); err != nil {
			t.Fatal(err)
		}
		if len(cookieSeen) != 2 || cookieSeen[1] != "" || !strings.Contains(cookieSeen[2], "c=v") {
			t.Fatalf("cookie handling wrong: %v", cookieSeen)
		}
	})

	t.Run("303 reload re-sends the original POST", func(t *testing.T) {
		bow := newDefaultTestBrowser()
		defer bow.Close()
		beforePost, beforeGet := startPosts, receiptGets
		if err := bow.Post(ts.URL+"/start", "application/x-www-form-urlencoded",
			readOnly{strings.NewReader("x=1")}); err != nil {
			t.Fatal(err)
		}
		if bow.Title() != "RECEIPT" {
			t.Fatalf("title = %q, want RECEIPT", bow.Title())
		}
		req := bow.State().Request
		if req.Method != "POST" || req.URL.String() != ts.URL+"/start" {
			t.Fatalf("stored request = %s %s, want POST /start", req.Method, req.URL)
		}
		if err := bow.Reload(); err != nil {
			t.Fatal(err)
		}
		if startPosts != beforePost+2 || receiptGets != beforeGet+2 {
			t.Fatalf("reload did not replay the original POST: posts=%d gets=%d", startPosts-beforePost, receiptGets-beforeGet)
		}
		if bow.Title() != "RECEIPT" {
			t.Fatalf("title after reload = %q", bow.Title())
		}
	})

	t.Run("large attachment spills to disk and still replays", func(t *testing.T) {
		dir := t.TempDir()
		bow := newDefaultTestBrowser()
		if err := bow.SetReplayStorage(1<<20, dir); err != nil {
			t.Fatal(err)
		}
		defer bow.Close()

		payload := bytes.Repeat([]byte{0, 1, 2, 3, 4, 5, 6, 7}, 256*1024) // 2 MiB
		if err := bow.PostMultipart(ts.URL+"/upload",
			url.Values{"q": {"big"}},
			FileSet{"attachment": {fileName: "big.bin", data: readOnly{bytes.NewReader(payload)}}}); err != nil {
			t.Fatal(err)
		}
		if names := dirEntries(t, dir); len(names) != 1 {
			t.Fatalf("want one spill file during navigation, got %v", names)
		}
		if err := bow.Reload(); err != nil {
			t.Fatal(err)
		}
		mu.Lock()
		first, second := uploadBodies[len(uploadBodies)-1], uploadBodies[len(uploadBodies)]
		meta := uploadMeta[len(uploadBodies)]
		mu.Unlock()
		if !bytes.Equal(first, second) {
			t.Fatal("spilled multipart body did not replay identically")
		}
		if meta.field != "big" || meta.filename != "big.bin" || !bytes.Equal(meta.fileData, payload) {
			t.Fatal("spilled multipart fields or attachment corrupted")
		}
		if err := bow.Close(); err != nil {
			t.Fatal(err)
		}
		if names := dirEntries(t, dir); len(names) != 0 {
			t.Fatalf("spill files left after Close: %v", names)
		}
		if err := bow.Close(); err != nil {
			t.Fatalf("repeated Close must be safe: %v", err)
		}
	})

	t.Run("failures never publish a partial page or leave files", func(t *testing.T) {
		dir := t.TempDir()
		bow := newDefaultTestBrowser()
		if err := bow.SetReplayStorage(0, dir); err != nil { // everything spills
			t.Fatal(err)
		}
		defer bow.Close()
		if err := bow.Open(ts.URL + "/a/page"); err != nil {
			t.Fatal(err)
		}
		wantHistory := bow.HistoryJar().Len()

		// Invalid replay settings are rejected without changing behavior.
		if err := bow.SetReplayStorage(-1, dir); err == nil {
			t.Fatal("negative memoryLimit must error")
		}
		missing := filepath.Join(dir, "does-not-exist")
		if err := bow.SetReplayStorage(0, missing); err == nil {
			t.Fatal("nonexistent replay dir must error")
		}

		// A storage failure while spilling (the directory vanished) aborts
		// before any request is sent and leaves no files behind.
		vanished := filepath.Join(dir, "vanished")
		if err := os.Mkdir(vanished, 0o755); err != nil {
			t.Fatal(err)
		}
		if err := bow.SetReplayStorage(0, vanished); err != nil {
			t.Fatal(err)
		}
		if err := os.Remove(vanished); err != nil {
			t.Fatal(err)
		}
		sendCalls := 0
		bow.SetTransport(roundTripFunc(func(r *http.Request) (*http.Response, error) {
			sendCalls++
			return htmlResponse(r, pageB), nil
		}))
		var err error
		err = bow.Post("http://fixture.invalid/post", "text/plain",
			readOnly{strings.NewReader("station=9")})
		if err == nil {
			t.Fatal("storage failure must surface as an error")
		}
		if sendCalls != 0 {
			t.Fatal("request was sent despite storage preparation failing")
		}
		if names := dirEntries(t, dir); len(names) != 1 {
			t.Fatalf("storage failure leaked files: %v", names)
		}
		if bow.Title() != "A" || bow.HistoryJar().Len() != wantHistory {
			t.Fatal("storage failure changed the current page")
		}
		if err := bow.SetReplayStorage(0, dir); err != nil {
			t.Fatal(err)
		}
		// Restore the real loopback transport before continuing.
		bow.SetTransport(nil)

		// Settings were untouched by the rejected calls: disk spills still
		// land in dir.
		if err := bow.Post(ts.URL+"/post", "text/plain", readOnly{strings.NewReader("ok")}); err != nil {
			t.Fatalf("navigation after rejected settings failed: %v", err)
		}
		if names := dirEntries(t, dir); len(names) != 1 {
			t.Fatalf("want one spill file, got %v", names)
		}

		// Source read failure: no request is sent, page and files untouched.
		calls := 0
		bow.SetTransport(roundTripFunc(func(r *http.Request) (*http.Response, error) {
			calls++
			return htmlResponse(r, pageB), nil
		}))
		before := calls
		err = bow.Post("http://fixture.invalid/post", "text/plain", brokenSource{})
		if err == nil || !strings.Contains(err.Error(), "fixture-source-read") {
			t.Fatalf("source error lost: %v", err)
		}
		if calls != before {
			t.Fatal("a request was sent despite body preparation failing")
		}
		if bow.Title() != "POST" || bow.HistoryJar().Len() != wantHistory+1 {
			t.Fatal("failed preparation changed the current page")
		}

		// Response read failure halfway through the body.
		closed := 0
		bow.SetTransport(roundTripFunc(func(r *http.Request) (*http.Response, error) {
			return &http.Response{
				StatusCode: 200,
				Header:     http.Header{"Content-Type": {"text/html"}},
				Request:    r,
				Body: &brokenBody{
					Reader: io.MultiReader(strings.NewReader("partial"), brokenSource{}),
					closed: &closed,
				},
			}, nil
		}))
		err = bow.Open("http://fixture.invalid/broken-read")
		if err == nil || !strings.Contains(err.Error(), "fixture-source-read") {
			t.Fatalf("response read error lost: %v", err)
		}
		if closed != 1 {
			t.Fatalf("response body closed %d times, want 1", closed)
		}
		if bow.Title() != "POST" {
			t.Fatal("response read failure changed the DOM")
		}
		var download bytes.Buffer
		if _, err := bow.Download(&download); err != nil {
			t.Fatal(err)
		}
		if download.String() != `<html><title>POST</title><body>saved</body></html>` {
			t.Fatalf("Download kept partial bytes: %q", download.String())
		}

		// Decompression failure.
		bow.SetTransport(roundTripFunc(func(r *http.Request) (*http.Response, error) {
			return &http.Response{
				StatusCode: 200,
				Header:     http.Header{"Content-Type": {"text/html"}, "Content-Encoding": {"gzip"}},
				Request:    r,
				Body:       io.NopCloser(strings.NewReader("not gzip at all")),
			}, nil
		}))
		if err := bow.Open("http://fixture.invalid/bad-gzip"); err == nil {
			t.Fatal("want gzip decode error")
		} else if !strings.Contains(err.Error(), "gzip") {
			t.Fatalf("decode error not traceable: %v", err)
		}
		if bow.Title() != "POST" {
			t.Fatal("decode failure changed the DOM")
		}

		// Transport failure: no retry, original cause preserved.
		sentinel := errors.New("fixture-roundtrip")
		sendCalls = 0
		bow.SetTransport(roundTripFunc(func(r *http.Request) (*http.Response, error) {
			sendCalls++
			return nil, sentinel
		}))
		err = bow.Reload()
		if err == nil || !errors.Is(err, sentinel) {
			t.Fatalf("transport error not traceable: %v", err)
		}
		if sendCalls != 1 {
			t.Fatalf("want exactly one send attempt, got %d", sendCalls)
		}
		if bow.Title() != "POST" {
			t.Fatal("transport failure changed the DOM")
		}
		if names := dirEntries(t, dir); len(names) != 1 {
			t.Fatalf("failure handling leaked spill files: %v", names)
		}
	})

	t.Run("closing one tab keeps shared pages replayable", func(t *testing.T) {
		dir := t.TempDir()
		tab1 := newDefaultTestBrowser()
		if err := tab1.SetReplayStorage(0, dir); err != nil {
			t.Fatal(err)
		}
		if err := tab1.Post(ts.URL+"/post", "text/plain", readOnly{strings.NewReader("shared-body")}); err != nil {
			t.Fatal(err)
		}
		if err := tab1.Open(ts.URL + "/a/page"); err != nil {
			t.Fatal(err)
		}
		if names := dirEntries(t, dir); len(names) != 1 {
			t.Fatalf("want one spill file, got %v", names)
		}

		tab2 := tab1.NewTab()
		if tab2.HistoryJar() != tab1.HistoryJar() {
			t.Fatal("NewTab must share the history jar")
		}
		if err := tab1.Close(); err != nil {
			t.Fatal(err)
		}
		if names := dirEntries(t, dir); len(names) != 1 {
			t.Fatalf("closing one tab released a file the other tab still uses: %v", names)
		}
		if !tab2.Back() {
			t.Fatal("second tab cannot go back after the first closed")
		}
		if tab2.Title() != "POST" {
			t.Fatalf("restored title = %q, want POST", tab2.Title())
		}
		if err := tab2.Reload(); err != nil {
			t.Fatalf("second tab reload: %v", err)
		}
		if got := string(postBodies[len(postBodies)]); got != "shared-body" {
			t.Fatalf("replayed body after tab close = %q", got)
		}
		if err := tab2.Close(); err != nil {
			t.Fatal(err)
		}
		if names := dirEntries(t, dir); len(names) != 0 {
			t.Fatalf("files survived the last owner: %v", names)
		}

		closed := newDefaultTestBrowser()
		if err := closed.Close(); err != nil {
			t.Fatal(err)
		}
		if err := closed.Open(ts.URL + "/b/page"); err == nil {
			t.Fatal("Open after Close must error")
		}
		if err := closed.Post(ts.URL+"/post", "text/plain", strings.NewReader("x")); err == nil {
			t.Fatal("Post after Close must error")
		}
		if err := closed.Reload(); err == nil {
			t.Fatal("Reload after Close must error")
		}
		if closed.Back() {
			t.Fatal("Back after Close must return false")
		}
	})
}
