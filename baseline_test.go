package soup

import (
	"fmt"
	"io"
	"net/http"
	"reflect"
	"strings"
	"testing"
)

type testTransport func(*http.Request) (*http.Response, error)

func (f testTransport) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

type testBody struct {
	io.Reader
	closed *int
}

func (b *testBody) Close() error { *b.closed++; return nil }

type testBrokenReader struct{}

func (testBrokenReader) Read(p []byte) (int, error) { return 0, fmt.Errorf("fixture read failure") }

func TestAcquisitionBaseline(t *testing.T) {
	oldHeaders, oldCookies := Headers, Cookies
	Headers = map[string]string{"X-Collect": "baseline"}
	Cookies = map[string]string{"vendor": "one"}
	defer func() { Headers, Cookies = oldHeaders, oldCookies }()
	var paths []string
	closed := 0
	client := &http.Client{Transport: testTransport(func(r *http.Request) (*http.Response, error) {
		paths = append(paths, r.URL.Path)
		if r.Method != "GET" || r.Header.Get("X-Collect") != "baseline" {
			t.Fatalf("request: %s %v", r.Method, r.Header)
		}
		c, err := r.Cookie("vendor")
		if err != nil || c.Value != "one" {
			t.Fatalf("cookie: %v %v", c, err)
		}
		status := 200
		h := make(http.Header)
		var body io.Reader
		switch r.URL.Path {
		case "/entry":
			status = 302
			h.Set("Location", "/catalog/page/")
			body = strings.NewReader("redirect")
		case "/catalog/page/":
			body = strings.NewReader(`<html><head><base href="../assets/"></head><body><h1>Vendor &amp; Parts</h1><a href="parts">Parts</a><iframe src="inside"></iframe><iframe srcdoc="&lt;p&gt;Inline&lt;/p&gt;"></iframe></body></html>`)
		case "/broken":
			body = testBrokenReader{}
		case "/status":
			status = 503
			body = strings.NewReader(`<p>Unavailable</p>`)
		default:
			t.Fatalf("unexpected implicit child request: %s", r.URL)
			return nil, nil
		}
		return &http.Response{StatusCode: status, Header: h, Body: &testBody{Reader: body, closed: &closed}, Request: r}, nil
	})}
	text, err := GetWithClient("https://vendor.test/entry", client)
	if err != nil {
		t.Fatal(err)
	}
	root := HTMLParse(text)
	if root.Error != nil {
		t.Fatal(root.Error)
	}
	if got := root.Find("h1").Text(); got != "Vendor & Parts" {
		t.Fatalf("heading content: %q", got)
	}
	if got := root.Find("base").Attrs()["href"]; got != "../assets/" {
		t.Fatalf("base content: %q", got)
	}
	frames := root.FindAll("iframe")
	if len(frames) != 2 {
		t.Fatalf("iframe extraction: %d", len(frames))
	}
	if frames[0].Attrs()["src"] != "inside" || frames[1].Attrs()["srcdoc"] != "<p>Inline</p>" {
		t.Fatal("iframe attributes")
	}
	if !reflect.DeepEqual(paths, []string{"/entry", "/catalog/page/"}) || closed != 2 {
		t.Fatalf("acquisition paths=%v closed=%d", paths, closed)
	}
	if _, err = GetWithClient("https://vendor.test/broken", client); err == nil {
		t.Fatal("read failure swallowed")
	}
	if closed != 3 {
		t.Fatalf("failed read body not closed: %d", closed)
	}
	text, err = GetWithClient("https://vendor.test/status", client)
	if err != nil || HTMLParse(text).Find("p").Text() != "Unavailable" {
		t.Fatalf("legacy status behavior: %q %v", text, err)
	}
	if closed != 4 {
		t.Fatalf("status body not closed: %d", closed)
	}
	t.Log("BASELINE soup requests=4 closed=4 dom_frames=2 recursive_requests=0")
}
