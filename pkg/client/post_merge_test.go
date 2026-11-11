package client

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"iter"
	"net/http"
	"net/http/httptest"
	"net/url"
	"testing"

	"github.com/robert-malhotra/go-stac-client/pkg/stac"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// recordedRequest captures what the client actually put on the wire.
type recordedRequest struct {
	method  string
	path    string
	headers http.Header
	body    map[string]any
	rawBody []byte
}

// newRecordingServer builds an httptest server that records every request and
// delegates response scripting to handler. The handler receives the just-
// recorded request plus the full history so closures do not need to capture
// the returned slice (which is out of scope inside the initializer).
func newRecordingServer(t *testing.T, handler func(w http.ResponseWriter, r *http.Request, rec *recordedRequest, all *[]recordedRequest)) (*httptest.Server, *[]recordedRequest) {
	t.Helper()
	var recorded []recordedRequest
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		raw := new(bytes.Buffer)
		_, _ = raw.ReadFrom(r.Body)
		rec := recordedRequest{
			method:  r.Method,
			path:    r.URL.RequestURI(),
			headers: r.Header.Clone(),
			rawBody: raw.Bytes(),
		}
		if len(raw.Bytes()) > 0 {
			_ = json.Unmarshal(raw.Bytes(), &rec.body)
		}
		recorded = append(recorded, rec)
		handler(w, r, &rec, &recorded)
	}))
	t.Cleanup(srv.Close)
	return srv, &recorded
}

// featurePage writes a one-item FeatureCollection, optionally carrying a
// pre-rendered next link (raw JSON object, no surrounding array brackets).
func featurePage(w http.ResponseWriter, id, nextLinkJSON string) {
	w.Header().Set("Content-Type", "application/geo+json")
	links := "[]"
	if nextLinkJSON != "" {
		links = "[" + nextLinkJSON + "]"
	}
	fmt.Fprintf(w, `{"type":"FeatureCollection","features":[{"type":"Feature","id":%q,"stac_version":"1.0.0","geometry":null,"properties":{},"links":[],"assets":{}}],"links":%s}`, id, links)
}

func postNext(href string, merge bool, bodyJSON string, headersJSON string) string {
	link := fmt.Sprintf(`{"rel":"next","href":%q,"method":"POST"`, href)
	if merge {
		link += `,"merge":true`
	}
	if bodyJSON != "" {
		link += `,"body":` + bodyJSON
	}
	if headersJSON != "" {
		link += `,"headers":` + headersJSON
	}
	return link + "}"
}

// absURL builds an absolute URL for the server handling r. Using the request
// avoids capturing the httptest server inside its own initializer.
func absURL(r *http.Request, path string) string {
	scheme := "http"
	if r.TLS != nil {
		scheme = "https"
	}
	return scheme + "://" + r.Host + path
}

// TestPostSearchMergeContext is the umbrella entry point for STAC Item
// Search POST next-link merge support.
func TestPostSearchMergeContext(t *testing.T) {
	t.Run("replacement then merge restores initial scope", testReplaceThenMerge)
	t.Run("SearchPages Next untouched and repeated run independent", testSearchPagesRepeat)
	t.Run("interleaved searches keep independent baselines", testInterleavedSearches)
	t.Run("map and RawMessage bodies merge like structs", testMapAndRawMessageBodies)
	t.Run("empty merge body cycles back to initial request", testEmptyMergeCycle)
	t.Run("same URL with different tokens continues", testSameURLDifferentTokens)
	t.Run("illegal bodies error without sending the page", testIllegalBodies)
	t.Run("headers merge case-insensitively without accumulation", testHeaderMerge)
	t.Run("GET compatibility and method defaults to GET", testGETCompatibility)
	t.Run("pure merge helpers", testMergeHelpers)
}

// testReplaceThenMerge drives three pages: initial scoped POST, a
// replacement-style next (no merge), then a merge=true next that must restore
// the initial collections/filter/limit rather than inherit the token-only
// replacement page.
func testReplaceThenMerge(t *testing.T) {
	filter := map[string]any{
		"op":   "=",
		"args": []any{map[string]any{"property": "platform"}, "sentinel-2a"},
	}
	srv, recorded := newRecordingServer(t, func(w http.ResponseWriter, r *http.Request, rec *recordedRequest, recorded *[]recordedRequest) {
		switch len(*recorded) {
		case 1:
			require.Equal(t, http.MethodPost, rec.method)
			require.Equal(t, []any{"sentinel-2"}, rec.body["collections"])
			require.NotNil(t, rec.body["filter"])
			require.EqualValues(t, 5, rec.body["limit"])
			featurePage(w, "p1", postNext(absURL(r, "/search"), false, `{"token":"t2"}`, ""))
		case 2:
			// Replacement semantics: the link's body is sent verbatim.
			require.Equal(t, map[string]any{"token": "t2"}, rec.body)
			featurePage(w, "p2", postNext(absURL(r, "/search"), true, `{"token":"t3"}`, ""))
		case 3:
			// Merge semantics: initial scope is back, token is t3 only.
			require.Equal(t, []any{"sentinel-2"}, rec.body["collections"])
			require.NotNil(t, rec.body["filter"])
			require.EqualValues(t, 5, rec.body["limit"])
			require.Equal(t, "t3", rec.body["token"])
			require.NotContains(t, rec.body, "t2")
			featurePage(w, "p3", "")
		default:
			t.Errorf("unexpected request %d: %+v", len(*recorded), rec)
		}
	})

	cli, err := NewClient(srv.URL)
	require.NoError(t, err)
	params := SearchParams{
		Collections: []string{"sentinel-2"},
		FilterLang:  "cql2-json",
		Filter:      filter,
		Limit:       5,
	}
	var ids []string
	for it, e := range cli.Search(context.Background(), params) {
		require.NoError(t, e)
		ids = append(ids, it.ID)
	}
	assert.Equal(t, []string{"p1", "p2", "p3"}, ids)
	require.Len(t, *recorded, 3)

	// The caller's params must be untouched after the run.
	assert.Equal(t, []string{"sentinel-2"}, params.Collections)
	assert.Equal(t, filter, params.Filter)
}

// testSearchPagesRepeat checks that PageResult.Next is never mutated and that
// re-running the same iterator rebuilds identical requests from its own
// baseline.
func testSearchPagesRepeat(t *testing.T) {
	srv, recorded := newRecordingServer(t, func(w http.ResponseWriter, r *http.Request, rec *recordedRequest, recorded *[]recordedRequest) {
		if len(*recorded)%2 == 1 {
			require.Equal(t, []any{"s2"}, rec.body["collections"])
			featurePage(w, fmt.Sprintf("p%d", len(*recorded)),
				postNext(absURL(r, "/search"), true, `{"token":"imm"}`, ""))
			return
		}
		require.Equal(t, []any{"s2"}, rec.body["collections"])
		require.Equal(t, "imm", rec.body["token"])
		featurePage(w, fmt.Sprintf("p%d", len(*recorded)), "")
	})
	cli, err := NewClient(srv.URL)
	require.NoError(t, err)
	params := SearchParams{Collections: []string{"s2"}, Limit: 7}

	run := func() []*PageResult[stac.Item] {
		var pages []*PageResult[stac.Item]
		for p, e := range cli.SearchPages(context.Background(), params) {
			require.NoError(t, e)
			pages = append(pages, p)
		}
		return pages
	}

	first := run()
	require.Len(t, first, 2)
	second := run()
	require.Len(t, second, 2)
	require.Len(t, *recorded, 4)

	// The decoded next link survives the iteration unchanged.
	require.NotNil(t, first[0].Next)
	assert.True(t, first[0].Next.Merge)
	assert.Equal(t, http.MethodPost, first[0].Next.Method)
	assert.Equal(t, map[string]any{"token": "imm"}, first[0].Next.Body)

	// Both runs produce byte-identical request bodies.
	for i := 0; i < 2; i++ {
		assert.JSONEq(t, string((*recorded)[i].rawBody), string((*recorded)[i+2].rawBody))
	}
	// Caller's params still intact.
	assert.Equal(t, []string{"s2"}, params.Collections)
}

// testInterleavedSearches pulls pages from two searches over the same client
// in alternation; each must merge against its own initial request.
func testInterleavedSearches(t *testing.T) {
	srv, recorded := newRecordingServer(t, func(w http.ResponseWriter, r *http.Request, rec *recordedRequest, recorded *[]recordedRequest) {
		token, _ := rec.body["token"].(string)
		switch rec.body["collections"].([]any)[0] {
		case "col-a":
			if token == "a2" {
				featurePage(w, "a-a2", "")
			} else {
				featurePage(w, "a-", postNext(absURL(r, "/search"), true, `{"token":"a2"}`, ""))
			}
		case "col-b":
			if token == "b2" {
				featurePage(w, "b-b2", "")
			} else {
				featurePage(w, "b-", postNext(absURL(r, "/search"), true, `{"token":"b2"}`, ""))
			}
		default:
			t.Errorf("request lost its collection scope: %v", rec.body)
		}
	})
	cli, err := NewClient(srv.URL)
	require.NoError(t, err)

	pullA, stopA := iter.Pull2(cli.Search(context.Background(), SearchParams{Collections: []string{"col-a"}, Limit: 4}))
	defer stopA()
	pullB, stopB := iter.Pull2(cli.Search(context.Background(), SearchParams{Collections: []string{"col-b"}, Limit: 4}))
	defer stopB()

	var ids []string
	for i := 0; i < 2; i++ {
		it, e, ok := pullA()
		require.True(t, ok)
		require.NoError(t, e)
		ids = append(ids, it.ID)
		it, e, ok = pullB()
		require.True(t, ok)
		require.NoError(t, e)
		ids = append(ids, it.ID)
	}
	// Both iterators must now report exhaustion without error.
	if it, e, ok := pullA(); ok || e != nil || it != nil {
		t.Fatalf("search A not exhausted cleanly: item=%v err=%v", it, e)
	}
	if it, e, ok := pullB(); ok || e != nil || it != nil {
		t.Fatalf("search B not exhausted cleanly: item=%v err=%v", it, e)
	}

	assert.Equal(t, []string{"a-", "b-", "a-a2", "b-b2"}, ids)
	require.Len(t, *recorded, 4)
	for i, rec := range *recorded {
		want := "col-a"
		if i%2 == 1 {
			want = "col-b"
		}
		assert.Equal(t, []any{want}, rec.body["collections"], "request %d", i+1)
		assert.EqualValues(t, 4, rec.body["limit"], "request %d", i+1)
	}
}

// testMapAndRawMessageBodies verifies the generic IteratePages path gives
// map and json.RawMessage bodies the same JSON-object semantics as structs.
func testMapAndRawMessageBodies(t *testing.T) {
	cases := []struct {
		name string
		body any
	}{
		{
			name: "map",
			body: map[string]any{
				"collections": []string{"landsat"},
				"limit":       float64(2),
				"filter":      map[string]any{"op": "=", "args": []any{map[string]any{"property": "platform"}, "landsat-9"}},
			},
		},
		{
			name: "raw-message",
			body: json.RawMessage(`{"collections":["landsat"],"limit":2,"filter":{"op":"=","args":[{"property":"platform"},"landsat-9"]}}`),
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			srv, recorded := newRecordingServer(t, func(w http.ResponseWriter, r *http.Request, rec *recordedRequest, recorded *[]recordedRequest) {
				switch len(*recorded) {
				case 1:
					featurePage(w, "m1", postNext(absURL(r, "/search"), true, `{"token":"rt"}`, ""))
				case 2:
					require.Equal(t, []any{"landsat"}, rec.body["collections"])
					require.EqualValues(t, 2, rec.body["limit"])
					require.NotNil(t, rec.body["filter"])
					require.Equal(t, "rt", rec.body["token"])
					featurePage(w, "m2", "")
				}
			})
			cli, err := NewClient(srv.URL)
			require.NoError(t, err)

			var ids []string
			for page, e := range IteratePages[stac.Item](context.Background(), cli, Post("search", tc.body), ItemDecoder()) {
				require.NoError(t, e)
				for _, it := range page.Items {
					ids = append(ids, it.ID)
				}
			}
			assert.Equal(t, []string{"m1", "m2"}, ids)
			require.Len(t, *recorded, 2)
		})
	}
}

// testEmptyMergeCycle: a merge=true link back to the same URL with no body
// reconstructs the initial request exactly, so the existing cycle guard fires
// before a second network call.
func testEmptyMergeCycle(t *testing.T) {
	srv, recorded := newRecordingServer(t, func(w http.ResponseWriter, r *http.Request, rec *recordedRequest, recorded *[]recordedRequest) {
		featurePage(w, "cyc", postNext(absURL(r, "/search"), true, "", ""))
	})
	cli, err := NewClient(srv.URL)
	require.NoError(t, err)

	var lastErr error
	var pages int
	for page, e := range cli.SearchPages(context.Background(), SearchParams{
		Collections: []string{"s2"},
		Filter:      map[string]any{"op": "=", "args": []any{"x", "y"}},
		Limit:       3,
	}) {
		if e != nil {
			lastErr = e
			break
		}
		pages++
		_ = page
	}
	require.Error(t, lastErr)
	assert.ErrorIs(t, lastErr, ErrPaginationCycle)
	assert.Equal(t, 1, pages)
	assert.Len(t, *recorded, 1, "duplicate request must never reach the network")
}

// testSameURLDifferentTokens proves a same-URL POST whose merged body differs
// (different token) is not a cycle, and that consecutive merges never
// accumulate the previous token.
func testSameURLDifferentTokens(t *testing.T) {
	srv, recorded := newRecordingServer(t, func(w http.ResponseWriter, r *http.Request, rec *recordedRequest, recorded *[]recordedRequest) {
		require.Equal(t, []any{"s2"}, rec.body["collections"])
		require.EqualValues(t, 3, rec.body["limit"])
		switch len(*recorded) {
		case 1:
			featurePage(w, "t1", postNext(absURL(r, "/search"), true, `{"token":"a"}`, ""))
		case 2:
			require.Equal(t, "a", rec.body["token"])
			featurePage(w, "t2", postNext(absURL(r, "/search"), true, `{"token":"b"}`, ""))
		case 3:
			require.Equal(t, "b", rec.body["token"])
			require.NotContains(t, rec.body, "a")
			featurePage(w, "t3", "")
		}
	})
	cli, err := NewClient(srv.URL)
	require.NoError(t, err)

	var ids []string
	for it, e := range cli.Search(context.Background(), SearchParams{Collections: []string{"s2"}, Limit: 3}) {
		require.NoError(t, e)
		ids = append(ids, it.ID)
	}
	assert.Equal(t, []string{"t1", "t2", "t3"}, ids)
	require.Len(t, *recorded, 3)
}

// testIllegalBodies covers every non-mergeable shape: the error must be
// returned clearly, the page must not be requested, and nothing panics.
func testIllegalBodies(t *testing.T) {
	t.Run("unencodable initial body fails before any request", func(t *testing.T) {
		srv, recorded := newRecordingServer(t, func(w http.ResponseWriter, r *http.Request, _ *recordedRequest, _ *[]recordedRequest) {
			featurePage(w, "x", "")
		})
		cli, err := NewClient(srv.URL)
		require.NoError(t, err)

		bad := struct{ Ch chan int }{Ch: make(chan int)}
		var lastErr error
		for _, e := range IteratePages[stac.Item](context.Background(), cli, Post("search", bad), ItemDecoder()) {
			lastErr = e
			break
		}
		require.Error(t, lastErr)
		assert.Contains(t, lastErr.Error(), "encode initial request body")
		assert.Empty(t, *recorded)
	})

	t.Run("initial array cannot be merged", func(t *testing.T) {
		srv, recorded := newRecordingServer(t, func(w http.ResponseWriter, r *http.Request, _ *recordedRequest, _ *[]recordedRequest) {
			featurePage(w, "x", postNext(absURL(r, "/search"), true, `{"token":"z"}`, ""))
		})
		cli, err := NewClient(srv.URL)
		require.NoError(t, err)

		lastErr := drainPages(t, cli, Post("search", json.RawMessage(`[1,2,3]`)))
		require.Error(t, lastErr)
		assert.Contains(t, lastErr.Error(), "not a JSON object")
		assert.Len(t, *recorded, 1)
	})

	t.Run("malformed initial RawMessage errors at merge time", func(t *testing.T) {
		srv, recorded := newRecordingServer(t, func(w http.ResponseWriter, r *http.Request, _ *recordedRequest, _ *[]recordedRequest) {
			featurePage(w, "x", postNext(absURL(r, "/search"), true, `{"token":"z"}`, ""))
		})
		cli, err := NewClient(srv.URL)
		require.NoError(t, err)

		lastErr := drainPages(t, cli, Post("search", json.RawMessage(`{"broken":`)))
		require.Error(t, lastErr)
		assert.Contains(t, lastErr.Error(), "not a JSON object")
		assert.Len(t, *recorded, 1)
	})

	t.Run("explicit null link body is not mergeable", func(t *testing.T) {
		srv, recorded := newRecordingServer(t, func(w http.ResponseWriter, r *http.Request, _ *recordedRequest, _ *[]recordedRequest) {
			link := fmt.Sprintf(`{"rel":"next","href":%q,"method":"POST","merge":true,"body":null}`, absURL(r, "/search"))
			featurePage(w, "x", link)
		})
		cli, err := NewClient(srv.URL)
		require.NoError(t, err)

		lastErr := drainPages(t, cli, Post("search", map[string]any{"collections": []string{"s2"}}))
		require.Error(t, lastErr)
		assert.Contains(t, lastErr.Error(), "null")
		assert.Len(t, *recorded, 1)
	})

	t.Run("array link body is not mergeable", func(t *testing.T) {
		srv, recorded := newRecordingServer(t, func(w http.ResponseWriter, r *http.Request, _ *recordedRequest, _ *[]recordedRequest) {
			featurePage(w, "x", postNext(absURL(r, "/search"), true, `["token","z"]`, ""))
		})
		cli, err := NewClient(srv.URL)
		require.NoError(t, err)

		lastErr := drainPages(t, cli, Post("search", map[string]any{"collections": []string{"s2"}}))
		require.Error(t, lastErr)
		assert.Contains(t, lastErr.Error(), "not a JSON object")
		assert.Len(t, *recorded, 1)
	})
}

// testHeaderMerge checks case-insensitive header override, explicit JSON null
// preservation, top-level replacement (no array concatenation), and that
// temporary link headers do not accumulate across consecutive merges.
func testHeaderMerge(t *testing.T) {
	srv, recorded := newRecordingServer(t, func(w http.ResponseWriter, r *http.Request, rec *recordedRequest, recorded *[]recordedRequest) {
		switch len(*recorded) {
		case 1:
			featurePage(w, "h1",
				postNext(absURL(r, "/search"), true, `{"token":"h2"}`, `{"X-TRACE":"p2"}`))
		case 2:
			assert.Equal(t, "p2", rec.headers.Get("X-Trace"))
			assert.Equal(t, "keep", rec.headers.Get("X-Other"))
			assert.Equal(t, "h2", rec.body["token"])
			// limit replaced wholesale with explicit null; collections survive.
			featurePage(w, "h2",
				postNext(absURL(r, "/search"), true, `{"token":"h3","limit":null}`, `{"X-New":"n3"}`))
		case 3:
			// Baseline header value is restored — the page-2 override did not stick.
			assert.Equal(t, "init", rec.headers.Get("X-Trace"))
			assert.Equal(t, "keep", rec.headers.Get("X-Other"))
			assert.Equal(t, "n3", rec.headers.Get("X-New"))
			assert.Equal(t, "h3", rec.body["token"])
			_, hasNullLimit := rec.body["limit"]
			assert.True(t, hasNullLimit, "explicit null must be retained as a key")
			assert.Nil(t, rec.body["limit"])
			assert.Equal(t, []any{"c1"}, rec.body["collections"])
			assert.NotContains(t, rec.body, "h2")
			featurePage(w, "h3", "")
		}
	})
	cli, err := NewClient(srv.URL)
	require.NoError(t, err)

	initial := &Request{
		Method:  http.MethodPost,
		URL:     "search",
		Body:    map[string]any{"collections": []string{"c1"}, "limit": 3},
		Headers: map[string]string{"X-Trace": "init", "x-other": "keep"},
	}
	var ids []string
	for page, e := range IteratePages[stac.Item](context.Background(), cli, initial, ItemDecoder()) {
		require.NoError(t, e)
		for _, it := range page.Items {
			ids = append(ids, it.ID)
		}
	}
	assert.Equal(t, []string{"h1", "h2", "h3"}, ids)
	require.Len(t, *recorded, 3)

	// Initial request headers must not have been modified.
	assert.Equal(t, map[string]string{"X-Trace": "init", "x-other": "keep"}, initial.Headers)
}

// testGETCompatibility covers ordinary GET pagination and the rule that the
// link's method decides: a merge link without a method stays GET even when
// the initial request was POST.
func testGETCompatibility(t *testing.T) {
	t.Run("ordinary GET paging is unchanged", func(t *testing.T) {
		srv, recorded := newRecordingServer(t, func(w http.ResponseWriter, r *http.Request, rec *recordedRequest, recorded *[]recordedRequest) {
			require.Equal(t, http.MethodGet, rec.method)
			if len(*recorded) == 1 {
				link := fmt.Sprintf(`{"rel":"next","href":%q}`, absURL(r, "/search?page=2"))
				featurePage(w, "g1", link)
			} else {
				require.Equal(t, "2", r.URL.Query().Get("page"))
				featurePage(w, "g2", "")
			}
		})
		cli, err := NewClient(srv.URL)
		require.NoError(t, err)

		var ids []string
		for it, e := range cli.SearchSimple(context.Background(), SearchParams{Collections: []string{"s2"}}) {
			require.NoError(t, e)
			ids = append(ids, it.ID)
		}
		assert.Equal(t, []string{"g1", "g2"}, ids)
		require.Len(t, *recorded, 2)
	})

	t.Run("merge link without method defaults to GET after POST", func(t *testing.T) {
		srv, recorded := newRecordingServer(t, func(w http.ResponseWriter, r *http.Request, rec *recordedRequest, recorded *[]recordedRequest) {
			if len(*recorded) == 1 {
				require.Equal(t, http.MethodPost, rec.method)
				// No "method" member: the next request must be GET, not POST.
				link := fmt.Sprintf(`{"rel":"next","href":%q,"merge":true,"body":{"token":"g"}}`, absURL(r, "/search-next"))
				featurePage(w, "n1", link)
				return
			}
			assert.Equal(t, http.MethodGet, rec.method)
			assert.Equal(t, "/search-next", rec.path)
			assert.Equal(t, "g", rec.body["token"])
			assert.Equal(t, []any{"s2"}, rec.body["collections"])
			featurePage(w, "n2", "")
		})
		cli, err := NewClient(srv.URL)
		require.NoError(t, err)

		var ids []string
		for it, e := range cli.Search(context.Background(), SearchParams{Collections: []string{"s2"}}) {
			require.NoError(t, e)
			ids = append(ids, it.ID)
		}
		assert.Equal(t, []string{"n1", "n2"}, ids)
		require.Len(t, *recorded, 2)
	})
}

// testMergeHelpers exercises the pure merge/canonicalization logic without a
// server, including struct-vs-map page-key equivalence.
func testMergeHelpers(t *testing.T) {
	t.Run("top-level overlay with null and no recursion", func(t *testing.T) {
		initial := json.RawMessage(`{"collections":["a","b"],"limit":5,"filter":{"op":"=","args":["x","y"]}}`)
		merged, err := mergeJSONBodies(initial, map[string]any{
			"token":  "tk",
			"limit":  nil,
			"filter": map[string]any{"op": "like"},
		})
		require.NoError(t, err)
		assert.Equal(t, []any{"a", "b"}, merged["collections"])
		assert.Equal(t, "tk", merged["token"])
		assert.Nil(t, merged["limit"])
		assert.Equal(t, map[string]any{"op": "like"}, merged["filter"], "nested values are replaced, not blended")
	})

	t.Run("missing link body is empty object", func(t *testing.T) {
		merged, err := mergeJSONBodies(json.RawMessage(`{"a":1}`), nil)
		require.NoError(t, err)
		assert.Equal(t, map[string]any{"a": float64(1)}, merged)

		empty, err := mergeJSONBodies(nil, nil)
		require.NoError(t, err)
		assert.Nil(t, empty)
	})

	t.Run("array and null bodies are rejected", func(t *testing.T) {
		_, err := mergeJSONBodies(json.RawMessage(`[1]`), map[string]any{"x": 1})
		require.Error(t, err)
		assert.Contains(t, err.Error(), "not a JSON object")

		_, err = mergeJSONBodies(json.RawMessage(`{"a":1}`), json.RawMessage(`null`))
		require.Error(t, err)
		assert.Contains(t, err.Error(), "null")

		_, err = mergeJSONBodies(json.RawMessage(`{"a":1}`), json.RawMessage(`[1]`))
		require.Error(t, err)
		assert.Contains(t, err.Error(), "not a JSON object")
	})

	t.Run("headers merge case-insensitively into a fresh map", func(t *testing.T) {
		base := map[string]string{"X-Trace": "init", "X-Other": "keep"}
		out := mergeHeaders(base, map[string]string{"x-trace": "next"})
		assert.Equal(t, "next", out["x-trace"])
		assert.Equal(t, "keep", out["X-Other"])
		assert.Len(t, out, 2)
		// Inputs untouched.
		assert.Equal(t, "init", base["X-Trace"])
		assert.Nil(t, mergeHeaders(nil, nil))
	})

	t.Run("pageKey collides for equal JSON from struct and map", func(t *testing.T) {
		params := SearchParams{Collections: []string{"s2"}, Limit: 5}
		asMap := map[string]any{"collections": []string{"s2"}, "limit": 5}
		u := mustParseURL(t, "https://example.com/search")
		assert.Equal(t,
			pageKey(http.MethodPost, u, params),
			pageKey(http.MethodPost, u, asMap),
		)
		assert.NotEqual(t,
			pageKey(http.MethodPost, u, asMap),
			pageKey(http.MethodPost, u, map[string]any{"collections": []string{"s2"}, "limit": 6}),
		)
	})

	t.Run("snapshot does not alias caller maps", func(t *testing.T) {
		req := Post("search", map[string]any{"collections": []string{"s2"}})
		req.Headers = map[string]string{"X-Trace": "init"}
		snap, err := snapshotMerge(req)
		require.NoError(t, err)
		merged, err := snap.applyMerge(&Request{
			Method:  http.MethodPost,
			URL:     "search",
			Body:    map[string]any{"token": "z"},
			Headers: map[string]string{"X-Trace": "z"},
			Merge:   true,
		})
		require.NoError(t, err)
		assert.Equal(t, "z", merged.Headers["X-Trace"])
		assert.Equal(t, "init", req.Headers["X-Trace"], "initial request must stay untouched")
		body := merged.Body.(map[string]any)
		assert.Equal(t, "z", body["token"])
	})
}

func drainPages(t *testing.T, cli *Client, req *Request) error {
	t.Helper()
	var lastErr error
	for _, e := range IteratePages[stac.Item](context.Background(), cli, req, ItemDecoder()) {
		if e != nil {
			lastErr = e
		}
	}
	return lastErr
}

func mustParseURL(t *testing.T, raw string) *url.URL {
	t.Helper()
	u, err := url.Parse(raw)
	require.NoError(t, err)
	return u
}
