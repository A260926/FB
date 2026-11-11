// This program only witnesses the unmodified starting behavior.
package main

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"

	"github.com/headzoo/surf"
)

const pageA = `<html><head><title>A</title><base href="/a/assets/"></head><body><a href="item">A</a></body></html>`
const pageB = `<html><head><title>B</title></head><body><a href="item">B</a></body></html>`

type readOnly struct{ io.Reader }
type roundTripFunc func(*http.Request) (*http.Response, error)

func (f roundTripFunc) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

type brokenReader struct{}

func (brokenReader) Read([]byte) (int, error) { return 0, errors.New("fixture-body-read") }
func check(ok bool, msg string) {
	if !ok {
		panic(msg)
	}
}
func must(err error) {
	if err != nil {
		panic(err)
	}
}
func path(s string) string { u, e := url.Parse(s); must(e); return u.Path }

func main() {
	posted := make(chan string, 8)
	uploads := make(chan string, 8)
	s := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		switch r.URL.Path {
		case "/a/page":
			fmt.Fprint(w, pageA)
		case "/b/page":
			fmt.Fprint(w, pageB)
		case "/post":
			body, e := io.ReadAll(r.Body)
			if e != nil {
				panic(e)
			}
			posted <- string(body)
			fmt.Fprint(w, `<html><title>POST</title><body>saved</body></html>`)
		case "/form":
			fmt.Fprint(w, `<form method="POST" action="/upload" enctype="multipart/form-data"><input name="q"><input type="file" name="attachment"><button type="submit" name="send" value="yes">send</button></form>`)
		case "/upload":
			must(r.ParseMultipartForm(1024))
			defer r.MultipartForm.RemoveAll()
			f, h, e := r.FormFile("attachment")
			must(e)
			defer f.Close()
			data, e := io.ReadAll(f)
			must(e)
			uploads <- r.FormValue("q") + "|" + h.Filename + "|" + string(data)
			fmt.Fprint(w, `<html><title>UPLOAD</title><body>accepted</body></html>`)
		default:
			http.NotFound(w, r)
		}
	}))
	defer s.Close()

	// Normal navigation and a real multipart form submit already work.
	normal := surf.NewBrowser()
	must(normal.Open(s.URL + "/form"))
	f, e := normal.Form("form")
	must(e)
	must(f.Input("q", "stations"))
	must(f.File("attachment", "query.bin", readOnly{strings.NewReader("file bytes")}))
	must(f.Submit())
	check(<-uploads == "stations|query.bin|file bytes", "multipart normal path")
	check(normal.Title() == "UPLOAD", "normal response title")
	fmt.Println("NORMAL_MULTIPART_OK fields=stations filename=query.bin bytes=10")

	// The request body is a non-seekable reader with no net/http GetBody factory.
	replay := surf.NewBrowser()
	must(replay.Post(s.URL+"/post", "application/octet-stream", readOnly{strings.NewReader("station=7")}))
	first := <-posted
	must(replay.Reload())
	second := <-posted
	check(first == "station=7" && second == "", "reload witness changed")
	fmt.Printf("RELOAD_WITNESS first=%q second=%q\n", first, second)

	// The DOM comes back from history; raw bytes and base remain outside that state.
	nav := surf.NewBrowser()
	must(nav.Open(s.URL + "/a/page"))
	check(nav.Title() == "A" && path(nav.Links()[0].URL.String()) == "/a/assets/item", "normal base path")
	must(nav.Open(s.URL + "/b/page"))
	forward := path(nav.Links()[0].URL.String())
	check(forward == "/a/assets/item", "forward base witness changed")
	check(nav.Back(), "back did not restore prior page")
	var downloaded bytes.Buffer
	_, e = nav.Download(&downloaded)
	must(e)
	check(nav.Title() == "A" && path(nav.Url().String()) == "/a/page", "history DOM control")
	check(downloaded.String() == pageB, "back body witness changed")
	fmt.Printf("SNAPSHOT_WITNESS forward_link=%s back_title=%s back_download=B\n", forward, nav.Title())

	// Response-body failure leaves the old DOM but has already overwritten raw bytes.
	fail := surf.NewBrowser()
	must(fail.Open(s.URL + "/a/page"))
	before := fail.HistoryJar().Len()
	closed := 0
	fail.SetTransport(roundTripFunc(func(r *http.Request) (*http.Response, error) {
		return &http.Response{StatusCode: 200, Header: http.Header{"Content-Type": {"text/html"}}, Request: r, Body: &countClose{Reader: io.MultiReader(strings.NewReader("partial"), brokenReader{}), closed: &closed}}, nil
	}))
	err := fail.Open("http://fixture.invalid/fail")
	check(err != nil && strings.Contains(err.Error(), "fixture-body-read"), "read failure lost")
	downloaded.Reset()
	_, e = fail.Download(&downloaded)
	must(e)
	check(downloaded.String() == "partial" && fail.Title() == "A" && fail.HistoryJar().Len() == before && closed == 1, "failure witness changed")
	fmt.Println("READ_FAILURE_WITNESS title=A download=partial history_unchanged=true response_closed=1")
	fmt.Println("SURF_NAVIGATION_BASELINE_OK")
}

type countClose struct {
	io.Reader
	closed *int
}

func (r *countClose) Close() error { *r.closed++; return nil }
