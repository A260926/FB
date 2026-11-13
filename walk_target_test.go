package gofeed_test

import (
	"context"
	"encoding/base64"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"sync/atomic"
	"testing"

	"github.com/mmcdole/gofeed"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// scriptedPage is one canned response in the in-memory transport. finalURL
// simulates where a redirect would have ended, without opening a socket: the
// sandbox (and plain unit tests) need no listening port.
type scriptedPage struct {
	status   int
	body     string
	finalURL string
	readErr  error
	closeErr error
}

// trackingBody records that the parser closed the body, and can fail reads or
// closes to exercise the error precedence shared with ParseURLWithContext.
type trackingBody struct {
	reader   *strings.Reader
	readErr  error
	closeErr error
	closed   atomic.Bool
}

func (b *trackingBody) Read(p []byte) (int, error) {
	n, err := b.reader.Read(p)
	if err == io.EOF && b.readErr != nil {
		return n, b.readErr
	}
	return n, err
}

func (b *trackingBody) Close() error {
	b.closed.Store(true)
	return b.closeErr
}

type scriptedTransport struct {
	mu         sync.Mutex
	pages      map[string]*scriptedPage
	requests   []string
	bodies     map[string]*trackingBody
	userAgents []string
	auth       []string
}

func newScriptedTransport(pages map[string]*scriptedPage) *scriptedTransport {
	return &scriptedTransport{
		pages:  pages,
		bodies: make(map[string]*trackingBody),
	}
}

func (t *scriptedTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	key := pageKey(req.URL)
	t.mu.Lock()
	// Fragments never leave the client, so the request log drops them just
	// like a real wire-level trace would.
	t.requests = append(t.requests, key)
	t.userAgents = append(t.userAgents, req.Header.Get("User-Agent"))
	t.auth = append(t.auth, req.Header.Get("Authorization"))
	page, ok := t.pages[key]
	t.mu.Unlock()
	if !ok {
		return nil, fmt.Errorf("scripted transport: unexpected request %s", req.URL.String())
	}

	body := &trackingBody{
		reader:   strings.NewReader(page.body),
		readErr:  page.readErr,
		closeErr: page.closeErr,
	}
	t.mu.Lock()
	t.bodies[key] = body
	t.mu.Unlock()

	final := req.URL
	if page.finalURL != "" {
		u, err := url.Parse(page.finalURL)
		if err != nil {
			return nil, err
		}
		final = u
	}
	out := req.Clone(req.Context())
	out.URL = final

	status := page.status
	if status == 0 {
		status = http.StatusOK
	}
	return &http.Response{
		StatusCode: status,
		Status:     fmt.Sprintf("%d scripted", status),
		Header:     http.Header{"Content-Type": []string{"application/feed+json"}},
		Body:       body,
		Request:    out,
	}, nil
}

func (t *scriptedTransport) requestURLs() []string {
	t.mu.Lock()
	defer t.mu.Unlock()
	return append([]string(nil), t.requests...)
}

func (t *scriptedTransport) lastHeaders() (userAgent, authorization string) {
	t.mu.Lock()
	defer t.mu.Unlock()
	if len(t.requests) == 0 {
		return "", ""
	}
	return t.userAgents[len(t.userAgents)-1], t.auth[len(t.auth)-1]
}

func (t *scriptedTransport) bodyFor(rawURL string) *trackingBody {
	u, err := url.Parse(rawURL)
	if err != nil {
		return nil
	}
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.bodies[pageKey(u)]
}

// pageKey mirrors how a real server sees the request: fragments never travel,
// while queries (including parameter order) do.
func pageKey(u *url.URL) string {
	c := *u
	c.Fragment = ""
	return c.String()
}

func newWalkParser(tr *scriptedTransport) *gofeed.Parser {
	p := gofeed.NewParser()
	p.Client = &http.Client{Transport: tr}
	return p
}

func jsonPage(next string, ids ...string) string {
	var b strings.Builder
	b.WriteString(`{"version":"https://jsonfeed.org/version/1.1","title":"j"`)
	if next != "" {
		fmt.Fprintf(&b, `,"next_url":%q`, next)
	}
	b.WriteString(`,"items":[`)
	for i, id := range ids {
		if i > 0 {
			b.WriteString(",")
		}
		fmt.Fprintf(&b, `{"id":%q,"content_text":"item %s"}`, id, id)
	}
	b.WriteString(`]}`)
	return b.String()
}

func atomPage(next string, ids ...string) string {
	var b strings.Builder
	b.WriteString(`<feed xmlns="http://www.w3.org/2005/Atom"><title>a</title>`)
	if next != "" {
		fmt.Fprintf(&b, `<link rel="next" href=%q />`, next)
	}
	for _, id := range ids {
		// An entry-level rel="next" must never be treated as feed paging.
		fmt.Fprintf(&b, `<entry><id>%s</id><link rel="next" href="http://feed.test/entry-next" /><title>%s</title></entry>`, id, id)
	}
	b.WriteString(`</feed>`)
	return b.String()
}

const rssPage = `<rss version="2.0"><channel><title>r</title>` +
	`<item><guid>r1</guid><title>R1</title></item></channel></rss>`

// collect emits every item into a slice, returning the callback so tests can
// swap in failing behavior.
func collect(t *testing.T) (*[]*gofeed.Item, func(*gofeed.Item) error) {
	items := make([]*gofeed.Item, 0)
	return &items, func(item *gofeed.Item) error {
		items = append(items, item)
		return nil
	}
}

// TestWalkFeedFormats covers format detection across pages, translated
// NextPage values (JSON next_url, Atom feed-level rel="next", RSS none),
// relative next links resolved after a simulated redirect, empty pages,
// cross-page duplicate GUIDs, custom translators and single-page ParseURL
// behavior.
func TestWalkFeedFormats(t *testing.T) {
	t.Run("parse_string_next_page", func(t *testing.T) {
		p := gofeed.NewParser()

		jf, err := p.ParseString(jsonPage("/later", "j1"))
		require.NoError(t, err)
		assert.Equal(t, "/later", jf.NextPage)

		af, err := p.ParseString(atomPage("/later", "a1"))
		require.NoError(t, err)
		assert.Equal(t, "/later", af.NextPage)

		rf, err := p.ParseString(rssPage)
		require.NoError(t, err)
		assert.Empty(t, rf.NextPage)
	})

	t.Run("atom_json_redirect_relative_empty_and_duplicates", func(t *testing.T) {
		pages := map[string]*scriptedPage{
			// First page is JSON; a simulated redirect lands on /deep/feed, so
			// its relative next_url "p2" must resolve against that final URL.
			"http://feed.test/feed": {
				body:     jsonPage("p2", "j1", "j2"),
				finalURL: "http://feed.test/deep/feed",
			},
			// Second page is Atom and repeats j1, carries an entry-level
			// rel="next" that must be ignored, and pages onward relatively.
			"http://feed.test/deep/p2": {
				body: atomPage("p3", "a1", "j1", "a2"),
			},
			// An empty page still declares a next page and the walk continues.
			"http://feed.test/deep/p3": {
				body: `<feed xmlns="http://www.w3.org/2005/Atom"><link rel="next" href="/deep/p4" /></feed>`,
			},
			// Final Atom page: one normal item and one with an empty GUID.
			"http://feed.test/deep/p4": {
				body: `<feed xmlns="http://www.w3.org/2005/Atom">` +
					`<entry><id>a3</id></entry>` +
					`<entry><title>no-guid</title></entry>` +
					`</feed>`,
			},
		}
		tr := newScriptedTransport(pages)
		p := newWalkParser(tr)

		items, emit := collect(t)
		var firstItem *gofeed.Item
		origEmit := emit
		emit = func(item *gofeed.Item) error {
			if firstItem == nil {
				firstItem = item
			}
			// Every page body is closed before any of its items is emitted.
			if item.GUID == "j1" || item.GUID == "j2" {
				require.True(t, tr.bodyFor("http://feed.test/feed").closed.Load())
			}
			if item.GUID == "a1" || item.GUID == "a2" {
				require.True(t, tr.bodyFor("http://feed.test/deep/p2").closed.Load())
			}
			return origEmit(item)
		}

		report, err := p.WalkURL(context.Background(), "http://feed.test/feed", gofeed.WalkOptions{}, emit)
		require.NoError(t, err)
		assert.Equal(t, walkReportExpect(4, 6, ""), report)

		gotIDs := make([]string, 0, len(*items))
		for _, it := range *items {
			gotIDs = append(gotIDs, it.GUID)
		}
		// j1 repeats on page two and is skipped; the empty-GUID entry is kept.
		assert.Equal(t, []string{"j1", "j2", "a1", "a2", "a3", ""}, gotIDs)

		assert.Equal(t, []string{
			"http://feed.test/feed",
			"http://feed.test/deep/p2",
			"http://feed.test/deep/p3",
			"http://feed.test/deep/p4",
		}, tr.requestURLs())

		// The entry-level next link was never followed.
		for _, u := range tr.requestURLs() {
			assert.NotContains(t, u, "entry-next")
		}

		// Earlier items stay owned by the caller and are not mutated later.
		require.NotNil(t, firstItem)
		assert.Equal(t, "j1", firstItem.GUID)
		assert.Equal(t, "item j1", firstItem.Content)

		for _, u := range []string{
			"http://feed.test/feed",
			"http://feed.test/deep/p2",
			"http://feed.test/deep/p3",
			"http://feed.test/deep/p4",
		} {
			require.True(t, tr.bodyFor(u).closed.Load(), "body not closed: %s", u)
		}
	})

	t.Run("rss_has_no_pagination", func(t *testing.T) {
		tr := newScriptedTransport(map[string]*scriptedPage{
			"http://feed.test/rss": {body: rssPage},
		})
		p := newWalkParser(tr)

		items, emit := collect(t)
		report, err := p.WalkURL(context.Background(), "http://feed.test/rss", gofeed.WalkOptions{}, emit)
		require.NoError(t, err)
		assert.Equal(t, walkReportExpect(1, 1, ""), report)
		require.Len(t, *items, 1)
		assert.Equal(t, "r1", (*items)[0].GUID)
		assert.Equal(t, []string{"http://feed.test/rss"}, tr.requestURLs())
	})

	t.Run("custom_translator_clears_next_page", func(t *testing.T) {
		tr := newScriptedTransport(map[string]*scriptedPage{
			"http://feed.test/ct":  {body: jsonPage("/ct2", "c1")},
			"http://feed.test/ct2": {body: jsonPage("", "c2")},
		})
		p := newWalkParser(tr)
		p.JSONTranslator = &nextPageOverride{inner: &gofeed.DefaultJSONTranslator{}, next: ""}

		items, emit := collect(t)
		report, err := p.WalkURL(context.Background(), "http://feed.test/ct", gofeed.WalkOptions{}, emit)
		require.NoError(t, err)
		assert.Equal(t, walkReportExpect(1, 1, ""), report)
		require.Len(t, *items, 1)
		assert.Equal(t, "c1", (*items)[0].GUID)
		assert.Equal(t, []string{"http://feed.test/ct"}, tr.requestURLs())
	})

	t.Run("custom_translator_invents_next_page", func(t *testing.T) {
		// RSS declares nothing, but the translator is authoritative: the walk
		// follows the NextPage it sets rather than inspecting the source feed.
		tr := newScriptedTransport(map[string]*scriptedPage{
			"http://feed.test/rs":  {body: rssPage},
			"http://feed.test/rs2": {body: jsonPage("", "j2")},
		})
		p := newWalkParser(tr)
		p.RSSTranslator = &nextPageOverride{inner: &gofeed.DefaultRSSTranslator{}, next: "/rs2"}

		items, emit := collect(t)
		report, err := p.WalkURL(context.Background(), "http://feed.test/rs", gofeed.WalkOptions{}, emit)
		require.NoError(t, err)
		assert.Equal(t, walkReportExpect(2, 2, ""), report)
		require.Len(t, *items, 2)
		assert.Equal(t, []string{"r1", "j2"}, []string{(*items)[0].GUID, (*items)[1].GUID})
		assert.Equal(t, []string{
			"http://feed.test/rs",
			"http://feed.test/rs2",
		}, tr.requestURLs())
	})

	t.Run("parse_url_still_fetches_one_page", func(t *testing.T) {
		tr := newScriptedTransport(map[string]*scriptedPage{
			"http://feed.test/one": {body: jsonPage("/two", "o1")},
			"http://feed.test/two": {body: jsonPage("", "o2")},
		})
		p := newWalkParser(tr)

		feed, err := p.ParseURL("http://feed.test/one")
		require.NoError(t, err)
		require.Len(t, feed.Items, 1)
		assert.Equal(t, "o1", feed.Items[0].GUID)
		assert.Equal(t, "/two", feed.NextPage)
		assert.Equal(t, []string{"http://feed.test/one"}, tr.requestURLs())
	})
}

// nextPageOverride wraps a default translator and forces the translated
// NextPage to a fixed value, proving WalkURL only trusts the translated feed.
type nextPageOverride struct {
	inner gofeed.Translator
	next  string
}

func (o *nextPageOverride) Translate(feed interface{}) (*gofeed.Feed, error) {
	translated, err := o.inner.Translate(feed)
	if err != nil {
		return nil, err
	}
	translated.NextPage = o.next
	return translated, nil
}

// WalkReportExpect builds the report literal tests compare against.
func walkReportExpect(pages, items int, stoppedAt string) gofeed.WalkReport {
	return gofeed.WalkReport{Pages: pages, Items: items, StoppedAt: stoppedAt}
}

// TestWalkFeedBoundary covers incremental stopping at KnownIDs, opaque GUID
// handling, in-walk duplicates, empty GUIDs, full-mode ignoring KnownIDs,
// callback errors and per-call state reset.
func TestWalkFeedBoundary(t *testing.T) {
	t.Run("incremental_stops_at_known_guid", func(t *testing.T) {
		tr := newScriptedTransport(map[string]*scriptedPage{
			"http://feed.test/b1": {body: jsonPage("/b2", "n1", "n2")},
			"http://feed.test/b2": {body: jsonPage("/b3", "k1", "k2")},
			"http://feed.test/b3": {body: jsonPage("", "old")},
		})
		p := newWalkParser(tr)

		known := map[string]struct{}{"k1": {}}
		items, emit := collect(t)
		report, err := p.WalkURL(context.Background(), "http://feed.test/b1", gofeed.WalkOptions{
			Incremental: true,
			KnownIDs:    known,
		}, emit)
		require.NoError(t, err)
		assert.Equal(t, walkReportExpect(2, 2, "k1"), report)
		require.Len(t, *items, 2)
		assert.Equal(t, []string{"n1", "n2"}, []string{(*items)[0].GUID, (*items)[1].GUID})
		// The boundary page is fetched, but the page after it is never asked for.
		assert.Equal(t, []string{
			"http://feed.test/b1",
			"http://feed.test/b2",
		}, tr.requestURLs())
		// KnownIDs is input only.
		assert.Equal(t, map[string]struct{}{"k1": {}}, known)
	})

	t.Run("guid_is_opaque", func(t *testing.T) {
		tr := newScriptedTransport(map[string]*scriptedPage{
			"http://feed.test/o1": {body: jsonPage("/o2", "fresh", "  spaced  ", " spaced")},
			"http://feed.test/o2": {body: jsonPage("", "tail")},
		})
		p := newWalkParser(tr)

		items, emit := collect(t)
		report, err := p.WalkURL(context.Background(), "http://feed.test/o1", gofeed.WalkOptions{
			Incremental: true,
			KnownIDs:    map[string]struct{}{"  spaced  ": {}},
		}, emit)
		require.NoError(t, err)
		// The whitespace-padded GUID matches verbatim and stops the walk; the
		// boundary item itself is not delivered, and trimming would have
		// merged it with the different " spaced" GUID that follows.
		assert.Equal(t, walkReportExpect(1, 1, "  spaced  "), report)
		require.Len(t, *items, 1)
		assert.Equal(t, "fresh", (*items)[0].GUID)
		assert.Equal(t, []string{"http://feed.test/o1"}, tr.requestURLs())
	})

	t.Run("duplicate_guid_is_not_a_boundary", func(t *testing.T) {
		tr := newScriptedTransport(map[string]*scriptedPage{
			"http://feed.test/d1": {body: jsonPage("/d2", "x")},
			"http://feed.test/d2": {body: jsonPage("", "x", "y")},
		})
		p := newWalkParser(tr)

		items, emit := collect(t)
		report, err := p.WalkURL(context.Background(), "http://feed.test/d1", gofeed.WalkOptions{
			Incremental: true,
			KnownIDs:    map[string]struct{}{"something-else": {}},
		}, emit)
		require.NoError(t, err)
		assert.Equal(t, walkReportExpect(2, 2, ""), report)
		assert.Equal(t, []string{"x", "y"}, []string{(*items)[0].GUID, (*items)[1].GUID})
	})

	t.Run("empty_guid_always_delivered", func(t *testing.T) {
		tr := newScriptedTransport(map[string]*scriptedPage{
			"http://feed.test/e1": {body: jsonPage("", "", "")},
		})
		p := newWalkParser(tr)

		items, emit := collect(t)
		report, err := p.WalkURL(context.Background(), "http://feed.test/e1", gofeed.WalkOptions{
			Incremental: true,
			KnownIDs:    map[string]struct{}{"": {}},
		}, emit)
		require.NoError(t, err)
		assert.Equal(t, walkReportExpect(1, 2, ""), report)
		require.Len(t, *items, 2)
	})

	t.Run("full_mode_ignores_known_ids", func(t *testing.T) {
		tr := newScriptedTransport(map[string]*scriptedPage{
			"http://feed.test/f1": {body: jsonPage("/f2", "n1")},
			"http://feed.test/f2": {body: jsonPage("", "k1", "k2")},
		})
		p := newWalkParser(tr)

		items, emit := collect(t)
		report, err := p.WalkURL(context.Background(), "http://feed.test/f1", gofeed.WalkOptions{
			Incremental: false,
			KnownIDs:    map[string]struct{}{"k1": {}},
		}, emit)
		require.NoError(t, err)
		assert.Equal(t, walkReportExpect(2, 3, ""), report)
		assert.Equal(t, []string{"n1", "k1", "k2"}, []string{
			(*items)[0].GUID, (*items)[1].GUID, (*items)[2].GUID,
		})
	})

	t.Run("callback_error_returned_unchanged_counts_kept", func(t *testing.T) {
		tr := newScriptedTransport(map[string]*scriptedPage{
			"http://feed.test/x1": {body: jsonPage("/x2", "x1", "x2")},
			"http://feed.test/x2": {body: jsonPage("", "x3")},
		})
		p := newWalkParser(tr)
		sentinel := errors.New("emit failed")

		var emitted int
		report, err := p.WalkURL(context.Background(), "http://feed.test/x1", gofeed.WalkOptions{}, func(item *gofeed.Item) error {
			emitted++
			if emitted == 2 {
				return sentinel
			}
			return nil
		})
		assert.ErrorIs(t, err, sentinel)
		// The first emit succeeded and is counted; the failed one is not.
		// The already-counted page is preserved alongside the callback error.
		assert.Equal(t, walkReportExpect(1, 1, ""), report)
		assert.Equal(t, []string{"http://feed.test/x1"}, tr.requestURLs())
	})

	t.Run("each_call_starts_fresh", func(t *testing.T) {
		tr := newScriptedTransport(map[string]*scriptedPage{
			"http://feed.test/solo": {body: jsonPage("", "s1")},
		})
		p := newWalkParser(tr)

		for i := 0; i < 2; i++ {
			items, emit := collect(t)
			report, err := p.WalkURL(context.Background(), "http://feed.test/solo", gofeed.WalkOptions{}, emit)
			require.NoError(t, err)
			assert.Equal(t, walkReportExpect(1, 1, ""), report)
			require.Len(t, *items, 1)
		}
		assert.Len(t, tr.requestURLs(), 2)
	})
}

// TestWalkFeedHTTP covers HTTP configuration reuse, HTTP/body errors and
// their precedence, body closing, pagination cycles (direct and via redirect
// aliases), fragment/query page identity, scheme filtering and argument
// validation.
func TestWalkFeedHTTP(t *testing.T) {
	t.Run("user_agent_auth_and_client_are_reused", func(t *testing.T) {
		tr := newScriptedTransport(map[string]*scriptedPage{
			"http://feed.test/h1": {body: jsonPage("", "h1")},
		})
		p := newWalkParser(tr)
		p.UserAgent = "walker-agent/2.0"
		p.AuthConfig = &gofeed.Auth{Username: "user", Password: "pass"}

		_, emit := collect(t)
		_, err := p.WalkURL(context.Background(), "http://feed.test/h1", gofeed.WalkOptions{}, emit)
		require.NoError(t, err)
		require.Len(t, tr.requestURLs(), 1)
		userAgent, authorization := tr.lastHeaders()
		assert.Equal(t, "walker-agent/2.0", userAgent)
		assert.Equal(t, "Basic "+base64.StdEncoding.EncodeToString([]byte("user:pass")), authorization)
	})

	t.Run("http_error_not_counted_body_closed", func(t *testing.T) {
		tr := newScriptedTransport(map[string]*scriptedPage{
			"http://feed.test/err": {status: http.StatusInternalServerError, body: "boom"},
		})
		p := newWalkParser(tr)

		report, err := p.WalkURL(context.Background(), "http://feed.test/err", gofeed.WalkOptions{}, func(*gofeed.Item) error {
			t.Fatal("no item should be emitted on an HTTP error")
			return nil
		})
		var httpErr gofeed.HTTPError
		require.ErrorAs(t, err, &httpErr)
		assert.Equal(t, http.StatusInternalServerError, httpErr.StatusCode)
		assert.Equal(t, walkReportExpect(0, 0, ""), report)
		require.True(t, tr.bodyFor("http://feed.test/err").closed.Load())
	})

	t.Run("read_error_surfaces_page_not_counted", func(t *testing.T) {
		readErr := errors.New("boom-read")
		tr := newScriptedTransport(map[string]*scriptedPage{
			"http://feed.test/read": {body: jsonPage("", "z"), readErr: readErr},
		})
		p := newWalkParser(tr)

		report, err := p.WalkURL(context.Background(), "http://feed.test/read", gofeed.WalkOptions{}, func(*gofeed.Item) error {
			t.Fatal("no item should be emitted on a read error")
			return nil
		})
		require.Error(t, err)
		assert.ErrorIs(t, err, readErr)
		assert.Equal(t, walkReportExpect(0, 0, ""), report)
		require.True(t, tr.bodyFor("http://feed.test/read").closed.Load())
	})

	t.Run("close_error_wins_after_successful_parse", func(t *testing.T) {
		closeErr := errors.New("boom-close")
		tr := newScriptedTransport(map[string]*scriptedPage{
			"http://feed.test/close": {body: jsonPage("", "z"), closeErr: closeErr},
		})
		p := newWalkParser(tr)

		report, err := p.WalkURL(context.Background(), "http://feed.test/close", gofeed.WalkOptions{}, func(*gofeed.Item) error {
			t.Fatal("items are emitted only after the body closes")
			return nil
		})
		require.Error(t, err)
		assert.ErrorIs(t, err, closeErr)
		assert.Equal(t, walkReportExpect(0, 0, ""), report)
	})

	t.Run("max_byte_size_enforced", func(t *testing.T) {
		tr := newScriptedTransport(map[string]*scriptedPage{
			"http://feed.test/big": {body: jsonPage("/next", "big-id")},
		})
		p := newWalkParser(tr)
		p.MaxByteSize = 8

		report, err := p.WalkURL(context.Background(), "http://feed.test/big", gofeed.WalkOptions{}, func(*gofeed.Item) error {
			t.Fatal("no item should be emitted when the body exceeds MaxByteSize")
			return nil
		})
		require.ErrorIs(t, err, gofeed.ErrResponseTooLarge)
		assert.Equal(t, walkReportExpect(0, 0, ""), report)
		assert.Equal(t, []string{"http://feed.test/big"}, tr.requestURLs())
	})

	t.Run("direct_cycle_detected_before_repeat_request", func(t *testing.T) {
		tr := newScriptedTransport(map[string]*scriptedPage{
			"http://feed.test/c1": {body: jsonPage("/c2", "c1")},
			"http://feed.test/c2": {body: jsonPage("/c1", "c2")},
		})
		p := newWalkParser(tr)

		items, emit := collect(t)
		report, err := p.WalkURL(context.Background(), "http://feed.test/c1", gofeed.WalkOptions{}, emit)
		require.ErrorIs(t, err, gofeed.ErrPaginationCycle)
		assert.Equal(t, walkReportExpect(2, 2, ""), report)
		require.Len(t, *items, 2)
		assert.Equal(t, []string{
			"http://feed.test/c1",
			"http://feed.test/c2",
		}, tr.requestURLs())
	})

	t.Run("redirect_alias_ring_recognized_after_response", func(t *testing.T) {
		tr := newScriptedTransport(map[string]*scriptedPage{
			"http://feed.test/a1": {body: jsonPage("/a2", "a1")},
			// A different alias that a redirect resolves back onto a1: the
			// response may be received, but the page is not parsed or emitted.
			"http://feed.test/a2": {body: jsonPage("", "a2"), finalURL: "http://feed.test/a1"},
		})
		p := newWalkParser(tr)

		items, emit := collect(t)
		report, err := p.WalkURL(context.Background(), "http://feed.test/a1", gofeed.WalkOptions{}, emit)
		require.ErrorIs(t, err, gofeed.ErrPaginationCycle)
		assert.Equal(t, walkReportExpect(1, 1, ""), report)
		require.Len(t, *items, 1)
		assert.Equal(t, "a1", (*items)[0].GUID)
		assert.Equal(t, []string{
			"http://feed.test/a1",
			"http://feed.test/a2",
		}, tr.requestURLs())
		require.True(t, tr.bodyFor("http://feed.test/a2").closed.Load())
	})

	t.Run("fragment_ignored_query_order_kept", func(t *testing.T) {
		tr := newScriptedTransport(map[string]*scriptedPage{
			"http://feed.test/f1": {body: jsonPage("/f2#section", "f1")},
			"http://feed.test/f2": {body: jsonPage("/f1#top", "f2")},
			// Same path, reversed query order: a distinct page identity.
			"http://feed.test/q?a=1&b=2": {body: jsonPage("/q?b=2&a=1", "q1")},
			"http://feed.test/q?b=2&a=1": {body: jsonPage("", "q2")},
		})
		p := newWalkParser(tr)

		// Fragments do not distinguish pages: f2's next link closes a cycle.
		_, err := p.WalkURL(context.Background(), "http://feed.test/f1", gofeed.WalkOptions{}, func(*gofeed.Item) error {
			return nil
		})
		require.ErrorIs(t, err, gofeed.ErrPaginationCycle)
		assert.Equal(t, []string{
			"http://feed.test/f1",
			"http://feed.test/f2",
		}, tr.requestURLs()[:2])

		// Query parameter order is significant: both variants are visited.
		items, emit := collect(t)
		report, err := p.WalkURL(context.Background(), "http://feed.test/q?a=1&b=2", gofeed.WalkOptions{}, emit)
		require.NoError(t, err)
		assert.Equal(t, walkReportExpect(2, 2, ""), report)
		require.Len(t, *items, 2)
		assert.Equal(t, []string{
			"http://feed.test/q?a=1&b=2",
			"http://feed.test/q?b=2&a=1",
		}, tr.requestURLs()[2:])
	})

	t.Run("non_http_next_page_rejected", func(t *testing.T) {
		tr := newScriptedTransport(map[string]*scriptedPage{
			"http://feed.test/n1": {body: jsonPage("ftp://feed.test/secret", "n1")},
		})
		p := newWalkParser(tr)

		report, err := p.WalkURL(context.Background(), "http://feed.test/n1", gofeed.WalkOptions{}, func(*gofeed.Item) error {
			return nil
		})
		require.Error(t, err)
		assert.Contains(t, err.Error(), "unsupported next page scheme")
		assert.Equal(t, walkReportExpect(1, 1, ""), report)
		assert.Equal(t, []string{"http://feed.test/n1"}, tr.requestURLs())
	})

	t.Run("invalid_arguments_fail_before_requests", func(t *testing.T) {
		tr := newScriptedTransport(map[string]*scriptedPage{
			"http://feed.test/z": {body: jsonPage("", "z")},
		})
		p := newWalkParser(tr)

		_, err := p.WalkURL(nil, "http://feed.test/z", gofeed.WalkOptions{}, func(*gofeed.Item) error { return nil })
		require.Error(t, err)

		_, err = p.WalkURL(context.Background(), "http://feed.test/z", gofeed.WalkOptions{}, nil)
		require.Error(t, err)

		_, err = p.WalkURL(context.Background(), "http://feed.test/%zz", gofeed.WalkOptions{}, func(*gofeed.Item) error { return nil })
		require.Error(t, err)

		_, err = p.WalkURL(context.Background(), "ftp://feed.test/z", gofeed.WalkOptions{}, func(*gofeed.Item) error { return nil })
		require.Error(t, err)

		assert.Empty(t, tr.requestURLs())
	})
}
