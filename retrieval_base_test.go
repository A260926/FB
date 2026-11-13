package gofeed_test

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/mmcdole/gofeed"
	"github.com/mmcdole/gofeed/atom"
	"github.com/mmcdole/gofeed/internal/testutil"
	"github.com/mmcdole/gofeed/rss"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// retrievalAtomDoc declares a relative root xml:base and a nested per-entry
// base, the shape described in the issue: the entry link only becomes absolute
// once the address the document was actually fetched from is part of the base
// chain.
const retrievalAtomDoc = `<feed xmlns="http://www.w3.org/2005/Atom" xml:base="../shared/">
<title>Feed</title>
<id>urn:feed</id>
<updated>2026-01-01T00:00:00Z</updated>
<link href="home"/>
<entry xml:base="items/">
  <title>One</title>
  <id>urn:one</id>
  <link href="one"/>
  <content type="html">&lt;img src="pic.png"&gt;</content>
</entry>
<entry>
  <title>Two</title>
  <id>urn:two</id>
  <link href="two"/>
</entry>
</feed>`

const retrievalRSSDoc = `<rss version="2.0" xml:base="../shared/">
<channel>
<title>Feed</title>
<link>home</link>
<description>d</description>
<item xml:base="items/">
  <title>One</title>
  <link>one</link>
</item>
<item>
  <title>Two</title>
  <link>two</link>
</item>
</channel>
</rss>`

// retrievalHandler serves the headline document after a redirect, and sets a
// Content-Location that must not be mistaken for the document address.
func retrievalHandler(w http.ResponseWriter, r *http.Request) {
	switch r.URL.Path {
	case "/entry":
		http.Redirect(w, r, "/feeds/2026/current.xml", http.StatusFound)
	case "/feeds/2026/current.xml":
		w.Header().Set("Content-Type", "application/atom+xml")
		w.Header().Set("Content-Location", "http://ignored.invalid/elsewhere/")
		io.WriteString(w, retrievalAtomDoc)
	default:
		http.NotFound(w, r)
	}
}

type roundTripFunc func(*http.Request) (*http.Response, error)

func (f roundTripFunc) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

// recordingAtomTranslator records the raw *atom.Feed handed to Translate, so a
// test can prove the parser resolved addresses before translation rather than
// patching the universal Feed afterwards.
type recordingAtomTranslator struct {
	inner    gofeed.Translator
	rawLinks []string
}

func (r *recordingAtomTranslator) Translate(feed interface{}) (*gofeed.Feed, error) {
	if af, ok := feed.(*atom.Feed); ok {
		for _, e := range af.Entries {
			if len(e.Links) > 0 {
				r.rawLinks = append(r.rawLinks, e.Links[0].Href)
			}
		}
	}
	return r.inner.Translate(feed)
}

func TestRetrievalBaseHTTP(t *testing.T) {
	srv := testutil.NewMemoryServer(t, http.HandlerFunc(retrievalHandler))
	feedURL := srv.URL("example.invalid", "/entry")

	const (
		wantFeedLink   = "http://example.invalid/feeds/shared/home"
		wantOneLink    = "http://example.invalid/feeds/shared/items/one"
		wantTwoLink    = "http://example.invalid/feeds/shared/two"
		wantImageInDoc = "http://example.invalid/feeds/shared/items/pic.png"
	)

	t.Run("redirect uses the address that served the document", func(t *testing.T) {
		p := gofeed.NewParser()
		p.Client = srv.Client
		p.UseResponseURL = true

		f, err := p.ParseURLWithContext(feedURL, context.Background())
		require.NoError(t, err)
		require.NotNil(t, f)
		require.Len(t, f.Items, 2)
		assert.Equal(t, wantFeedLink, f.Link)
		assert.Equal(t, wantOneLink, f.Items[0].Link)
		assert.Equal(t, wantTwoLink, f.Items[1].Link)
		// Content-Location points elsewhere; it must not be used as the base.
		assert.Contains(t, f.Items[0].Content, wantImageInDoc)
	})

	t.Run("switch off keeps historical relative values", func(t *testing.T) {
		p := gofeed.NewParser()
		p.Client = srv.Client

		f, err := p.ParseURLWithContext(feedURL, context.Background())
		require.NoError(t, err)
		require.Len(t, f.Items, 2)
		assert.Equal(t, "/shared/home", f.Link)
		assert.Equal(t, "/shared/items/one", f.Items[0].Link)
		assert.Equal(t, "/shared/two", f.Items[1].Link)
	})

	t.Run("custom client response without Request falls back to sent request", func(t *testing.T) {
		client := &http.Client{Transport: roundTripFunc(func(req *http.Request) (*http.Response, error) {
			rec := httptest.NewRecorder()
			retrievalHandler(rec, req)
			resp := rec.Result()
			resp.Request = nil // a minimal custom client need not populate it
			return resp, nil
		})}

		p := gofeed.NewParser()
		p.Client = client
		p.UseResponseURL = true

		f, err := p.ParseURLWithContext(srv.URL("example.invalid", "/feeds/2026/current.xml"), context.Background())
		require.NoError(t, err)
		require.Len(t, f.Items, 2)
		assert.Equal(t, wantOneLink, f.Items[0].Link)
		assert.Equal(t, wantTwoLink, f.Items[1].Link)
	})

	t.Run("max byte size still applies", func(t *testing.T) {
		p := gofeed.NewParser()
		p.Client = srv.Client
		p.UseResponseURL = true

		p.MaxByteSize = 100
		_, err := p.ParseURLWithContext(feedURL, context.Background())
		assert.ErrorIs(t, err, gofeed.ErrResponseTooLarge)

		p.MaxByteSize = 1 << 20
		f, err := p.ParseURLWithContext(feedURL, context.Background())
		require.NoError(t, err)
		assert.Equal(t, wantOneLink, f.Items[0].Link)
	})

	t.Run("native parsers and translator see resolved values", func(t *testing.T) {
		af, err := (&atom.Parser{}).ParseWithBase(strings.NewReader(retrievalAtomDoc), "http://example.invalid/feeds/2026/current.xml")
		require.NoError(t, err)
		require.Len(t, af.Entries, 2)
		assert.Equal(t, wantOneLink, af.Entries[0].Links[0].Href)
		assert.Equal(t, wantTwoLink, af.Entries[1].Links[0].Href)

		rf, err := (&rss.Parser{}).ParseWithBase(strings.NewReader(retrievalRSSDoc), "http://example.invalid/feeds/2026/current.xml")
		require.NoError(t, err)
		require.Len(t, rf.Items, 2)
		assert.Equal(t, "http://example.invalid/feeds/shared/items/one", rf.Items[0].Link)
		assert.Equal(t, "http://example.invalid/feeds/shared/two", rf.Items[1].Link)

		rec := &recordingAtomTranslator{inner: &gofeed.DefaultAtomTranslator{}}
		p := gofeed.NewParser()
		p.Client = srv.Client
		p.UseResponseURL = true
		p.KeepOriginalFeed = true
		p.AtomTranslator = rec

		f, err := p.ParseURLWithContext(feedURL, context.Background())
		require.NoError(t, err)
		require.Len(t, f.Items, 2)
		assert.Equal(t, wantOneLink, f.Items[0].Link)
		assert.Equal(t, wantTwoLink, f.Items[1].Link)
		// The raw object the Translator received is already resolved.
		assert.Equal(t, []string{wantOneLink, wantTwoLink}, rec.rawLinks)
		// So is the original retained for KeepOriginalFeed.
		orig, ok := f.OriginalFeed().(*atom.Feed)
		require.True(t, ok)
		assert.Equal(t, wantOneLink, orig.Entries[0].Links[0].Href)
	})

	t.Run("ParseWithBase matches the reported example", func(t *testing.T) {
		f, err := gofeed.NewParser().ParseWithBase(strings.NewReader(retrievalAtomDoc), "https://example.invalid/feeds/2026/current.xml")
		require.NoError(t, err)
		require.Len(t, f.Items, 2)
		assert.Equal(t, "https://example.invalid/feeds/shared/items/one", f.Items[0].Link)
		assert.Equal(t, "https://example.invalid/feeds/shared/two", f.Items[1].Link)
	})

	t.Run("JSON feeds get no relative address conversion", func(t *testing.T) {
		const jsonDoc = `{"version":"https://jsonfeed.org/version/1.1","title":"j",
"home_page_url":"home","feed_url":"feed.xml","items":[{"id":"1","url":"items/one","content_html":"<img src=\"pic.png\">"}]}`

		f, err := gofeed.NewParser().ParseWithBase(strings.NewReader(jsonDoc), "https://example.invalid/feeds/2026/current.xml")
		require.NoError(t, err)
		assert.Equal(t, "home", f.Link)
		require.Len(t, f.Items, 1)
		assert.Equal(t, "items/one", f.Items[0].Link)
		assert.Equal(t, `<img src="pic.png">`, f.Items[0].Content)

		// UseResponseURL is likewise not applied to JSON documents.
		srv := testutil.NewMemoryServer(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			io.WriteString(w, jsonDoc)
		}))
		p := gofeed.NewParser()
		p.Client = srv.Client
		p.UseResponseURL = true
		f, err = p.ParseURLWithContext(srv.URL("example.invalid", "/feed.json"), context.Background())
		require.NoError(t, err)
		assert.Equal(t, "items/one", f.Items[0].Link)
		assert.Equal(t, "home", f.Link)
	})

	t.Run("invalid base is rejected before reading input", func(t *testing.T) {
		for _, base := range []string{"", "   ", "/relative", "not a url", "ftp://example.invalid/x", "https:///nohost", "http://"} {
			_, err := gofeed.NewParser().ParseWithBase(testutil.PanicReader{TB: t}, base)
			assert.Error(t, err, "base %q", base)
		}
	})
}

func TestRetrievalBaseIsolation(t *testing.T) {
	t.Run("interleaved fetches keep their own bases", func(t *testing.T) {
		arrived := make(chan struct{}, 2)
		release := make(chan struct{})
		handler := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			arrived <- struct{}{}
			<-release
			w.Header().Set("Content-Type", "application/rss+xml")
			fmt.Fprintf(w, `<rss version="2.0"><channel><title>%s</title>`+
				`<item><link>items/1</link></item></channel></rss>`, r.Host)
		})
		srv := testutil.NewMemoryServer(t, handler)

		// One shared parser, configured once and then read-only.
		p := gofeed.NewParser()
		p.Client = srv.Client
		p.UseResponseURL = true

		hosts := []string{"a.invalid", "b.invalid"}
		results := make([]*gofeed.Feed, len(hosts))
		errs := make([]error, len(hosts))
		var wg sync.WaitGroup
		for i, host := range hosts {
			wg.Add(1)
			go func(i int, host string) {
				defer wg.Done()
				results[i], errs[i] = p.ParseURLWithContext(srv.URL(host, "/feeds/a.xml"), context.Background())
			}(i, host)
		}

		// Both requests are in flight before either response is written, so
		// the two parses genuinely interleave.
		for i := 0; i < len(hosts); i++ {
			select {
			case <-arrived:
			case <-time.After(5 * time.Second):
				close(release)
				t.Fatal("feed requests did not reach the server concurrently")
			}
		}
		close(release)
		wg.Wait()

		for i, host := range hosts {
			require.NoError(t, errs[i])
			require.NotNil(t, results[i])
			require.Len(t, results[i].Items, 1)
			assert.Equal(t, host, results[i].Title)
			assert.Equal(t, "http://"+host+"/feeds/items/1", results[i].Items[0].Link)
		}
	})

	t.Run("ParseWithBase base does not leak into the parser", func(t *testing.T) {
		p := gofeed.NewParser()
		doc := retrievalRSSDoc

		first, err := p.ParseWithBase(strings.NewReader(doc), "https://one.invalid/a/b.xml")
		require.NoError(t, err)
		second, err := p.ParseWithBase(strings.NewReader(doc), "https://two.invalid/c/d.xml")
		require.NoError(t, err)
		plain, err := p.ParseString(doc)
		require.NoError(t, err)

		assert.Equal(t, "https://one.invalid/shared/items/one", first.Items[0].Link)
		assert.Equal(t, "https://two.invalid/shared/items/one", second.Items[0].Link)
		// A later ordinary parse still resolves only what the document declares.
		assert.Equal(t, "/shared/items/one", plain.Items[0].Link)
	})

	t.Run("read-only parser serves concurrent mixed sources", func(t *testing.T) {
		srv := testutil.NewMemoryServer(t, http.HandlerFunc(retrievalHandler))
		p := gofeed.NewParser()
		p.Client = srv.Client
		p.UseResponseURL = true

		const goroutines = 8
		var wg sync.WaitGroup
		errCh := make(chan error, goroutines*2)
		for i := 0; i < goroutines; i++ {
			wg.Add(2)
			go func() {
				defer wg.Done()
				f, err := p.ParseURLWithContext(srv.URL("example.invalid", "/entry"), context.Background())
				if err != nil {
					errCh <- err
					return
				}
				if got := f.Items[0].Link; got != "http://example.invalid/feeds/shared/items/one" {
					errCh <- fmt.Errorf("URL parse item link = %q", got)
				}
			}()
			go func() {
				defer wg.Done()
				f, err := p.ParseString(retrievalRSSDoc)
				if err != nil {
					errCh <- err
					return
				}
				// ParseString owns no document address: the switch must not
				// change it.
				if got := f.Items[0].Link; got != "/shared/items/one" {
					errCh <- fmt.Errorf("string parse item link = %q", got)
				}
			}()
		}
		wg.Wait()
		close(errCh)
		for err := range errCh {
			assert.NoError(t, err)
		}
	})
}
