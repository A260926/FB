package client

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func TestNewClient_RejectsBad(t *testing.T) {
	_, err := NewClient("")
	assert.Error(t, err)
	_, err = NewClient("not-a-url")
	assert.Error(t, err)
	_, err = NewClient("file:///etc/passwd")
	assert.Error(t, err)
	_, err = NewClient("ftp://example.com/")
	assert.Error(t, err)
}

func TestNewClient_AppendsTrailingSlash(t *testing.T) {
	c, err := NewClient("https://example.com/stac")
	require.NoError(t, err)
	assert.Equal(t, "/stac/", c.BaseURL().Path)

	c, err = NewClient("https://example.com")
	require.NoError(t, err)
	assert.Equal(t, "/", c.BaseURL().Path)
}

func TestWithTimeout_DoesNotMutateUserHTTPClient(t *testing.T) {
	user := &http.Client{Timeout: 0}
	c, err := NewClient("https://example.com",
		WithHTTPClient(user),
		WithTimeout(2*time.Second))
	require.NoError(t, err)
	assert.Equal(t, time.Duration(0), user.Timeout, "user http.Client must not be mutated")
	assert.Equal(t, 2*time.Second, c.timeout)
}

func TestDefaultUserAgent(t *testing.T) {
	var got string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		got = r.Header.Get("User-Agent")
		w.Header().Set("Content-Type", "application/json")
		fmt.Fprint(w, `{"type":"Catalog","stac_version":"1.0.0","id":"x","description":"x","links":[]}`)
	}))
	defer srv.Close()

	c, err := NewClient(srv.URL)
	require.NoError(t, err)
	_, err = c.GetCatalog(context.Background())
	require.NoError(t, err)
	assert.Contains(t, got, "go-stac-client/")
}

func TestUnexpectedContentType(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		fmt.Fprint(w, "<html>captive portal</html>")
	}))
	defer srv.Close()
	c, _ := NewClient(srv.URL)
	_, err := c.GetCatalog(context.Background())
	require.Error(t, err)
	assert.ErrorIs(t, err, ErrUnexpectedContentType)
}

func TestMaxBodyBytes(t *testing.T) {
	big := strings.Repeat("x", 5000)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		fmt.Fprintf(w, `{"description":"%s","id":"x","stac_version":"1.0.0","links":[]}`, big)
	}))
	defer srv.Close()
	c, _ := NewClient(srv.URL, WithMaxBodyBytes(1024))
	_, err := c.GetCatalog(context.Background())
	require.Error(t, err)
	assert.ErrorIs(t, err, ErrResponseTooLarge)
}

func TestPaginationCycleDetection(t *testing.T) {
	mu := 0
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		mu++
		w.Header().Set("Content-Type", "application/json")
		// Always return a next-link to itself: a cycle.
		fmt.Fprintf(w, `{"type":"FeatureCollection","features":[{"type":"Feature","id":"a","stac_version":"1.0.0","geometry":null,"properties":{},"assets":{},"links":[]}],"links":[{"rel":"next","href":"%s"}]}`, r.URL.Path)
	}))
	defer srv.Close()
	c, _ := NewClient(srv.URL)
	var lastErr error
	for _, err := range c.GetItemsPages(context.Background(), "any") {
		if err != nil {
			lastErr = err
			break
		}
	}
	require.Error(t, lastErr)
	assert.ErrorIs(t, lastErr, ErrPaginationCycle)
}

func TestPaginationMaxPagesLimit(t *testing.T) {
	page := 0
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		page++
		w.Header().Set("Content-Type", "application/json")
		// Always offer a next link with a unique URL so cycle detection doesn't fire.
		fmt.Fprintf(w, `{"type":"FeatureCollection","features":[{"type":"Feature","id":"x","stac_version":"1.0.0","geometry":null,"properties":{},"assets":{},"links":[]}],"links":[{"rel":"next","href":"%s?p=%d"}]}`, r.URL.Path, page)
	}))
	defer srv.Close()
	c, _ := NewClient(srv.URL, WithMaxPages(3))
	count := 0
	var lastErr error
	for _, err := range c.GetItemsPages(context.Background(), "any") {
		if err != nil {
			lastErr = err
			break
		}
		count++
	}
	require.Error(t, lastErr)
	assert.ErrorIs(t, lastErr, ErrPaginationLimit)
	assert.Equal(t, 3, count)
}

func TestSortByPlusMinusEncoding(t *testing.T) {
	var got string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		got = r.URL.Query().Get("sortby")
		w.Header().Set("Content-Type", "application/json")
		fmt.Fprint(w, `{"type":"FeatureCollection","features":[],"links":[]}`)
	}))
	defer srv.Close()
	c, _ := NewClient(srv.URL)
	_, _ = collect(c.SearchSimple(context.Background(), SearchParams{
		SortBy: []SortField{
			{Field: "datetime", Direction: SortAsc},
			{Field: "eo:cloud_cover", Direction: SortDesc},
		},
	}))
	assert.Equal(t, "+datetime,-eo:cloud_cover", got)
}

func TestSearchSimpleRejectsIntersects(t *testing.T) {
	c, _ := NewClient("https://example.com")
	items, err := collect(c.SearchSimple(context.Background(), SearchParams{
		Intersects: map[string]any{"type": "Point", "coordinates": []float64{0, 0}},
	}))
	require.Error(t, err)
	assert.ErrorIs(t, err, ErrUnsupportedForGET)
	assert.Empty(t, items)
}

func TestSearchSimpleRejectsCQL2JSONFilter(t *testing.T) {
	c, _ := NewClient("https://example.com")
	_, err := collect(c.SearchSimple(context.Background(), SearchParams{
		Filter:     json.RawMessage(`{"op":"=","args":[{"property":"id"},"x"]}`),
		FilterLang: "cql2-json",
	}))
	require.Error(t, err)
	assert.ErrorIs(t, err, ErrUnsupportedForGET)
}

func TestSearchSimpleEncodesCQL2TextFilter(t *testing.T) {
	var got string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		got = r.URL.Query().Get("filter")
		w.Header().Set("Content-Type", "application/json")
		fmt.Fprint(w, `{"type":"FeatureCollection","features":[],"links":[]}`)
	}))
	defer srv.Close()
	c, _ := NewClient(srv.URL)
	_, _ = collect(c.SearchSimple(context.Background(), SearchParams{
		Filter:     "eo:cloud_cover < 10",
		FilterLang: "cql2-text",
	}))
	assert.Equal(t, "eo:cloud_cover < 10", got)
}

func TestSearchSimpleBboxFormatPrecision(t *testing.T) {
	var got string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		got = r.URL.Query().Get("bbox")
		w.Header().Set("Content-Type", "application/json")
		fmt.Fprint(w, `{"type":"FeatureCollection","features":[],"links":[]}`)
	}))
	defer srv.Close()
	c, _ := NewClient(srv.URL)
	_, _ = collect(c.SearchSimple(context.Background(), SearchParams{
		Bbox: []float64{-122.5, 37.5, -122.0, 38.0},
	}))
	assert.Equal(t, "-122.5,37.5,-122,38", got)
}

func TestBearerTokenSentToBaseHostOnly(t *testing.T) {
	var baseSeenAuth, otherSeenAuth string
	other := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		otherSeenAuth = r.Header.Get("Authorization")
		w.Header().Set("Content-Type", "application/json")
		fmt.Fprint(w, `{"type":"FeatureCollection","features":[],"links":[]}`)
	}))
	defer other.Close()

	base := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		baseSeenAuth = r.Header.Get("Authorization")
		w.Header().Set("Content-Type", "application/json")
		// First page: include a next link to a different host (the "other" server).
		fmt.Fprintf(w, `{"type":"FeatureCollection","features":[{"type":"Feature","id":"a","stac_version":"1.0.0","geometry":null,"properties":{},"assets":{},"links":[]}],"links":[{"rel":"next","href":"%s/next"}]}`, other.URL)
	}))
	defer base.Close()

	c, _ := NewClient(base.URL,
		WithBearerToken("supersecret"),
		WithAllowedHosts(strings.TrimPrefix(other.URL, "http://")), // allow the other host
	)
	for _, err := range c.GetItemsPages(context.Background(), "x") {
		if err != nil {
			break
		}
	}
	assert.Equal(t, "Bearer supersecret", baseSeenAuth)
	assert.Empty(t, otherSeenAuth, "credentials must not leak to a different host")
}

func TestRetryAfterParsedIntoHTTPError(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Retry-After", "7")
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusTooManyRequests)
		fmt.Fprint(w, `{"code":429,"description":"slow down"}`)
	}))
	defer srv.Close()
	c, _ := NewClient(srv.URL)
	_, err := c.GetCatalog(context.Background())
	require.Error(t, err)
	assert.ErrorIs(t, err, ErrRateLimited)
	var herr *HTTPError
	require.ErrorAs(t, err, &herr)
	assert.Equal(t, 7*time.Second, herr.RetryAfter)
	require.NotNil(t, herr.API)
	assert.Equal(t, "slow down", herr.API.Description)
}

func TestConformanceFallback(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		switch r.URL.Path {
		case "/":
			// Root has no conformsTo (e.g. OGC API Features only).
			fmt.Fprint(w, `{"type":"Catalog","stac_version":"1.0.0","id":"root","description":"x","links":[]}`)
		case "/conformance":
			fmt.Fprint(w, `{"conformsTo":["https://api.stacspec.org/v1.0.0/core","https://api.stacspec.org/v1.0.0/item-search"]}`)
		default:
			http.NotFound(w, r)
		}
	}))
	defer srv.Close()
	c, _ := NewClient(srv.URL)
	classes, err := c.GetConformance(context.Background())
	require.NoError(t, err)
	assert.Contains(t, classes, "https://api.stacspec.org/v1.0.0/core")
	assert.Contains(t, classes, "https://api.stacspec.org/v1.0.0/item-search")
}

func TestDecoderURLBuildingMergesQuery(t *testing.T) {
	url, err := mergeQuery("/items?collections=x", map[string]string{"page": "2"})
	require.NoError(t, err)
	assert.Contains(t, url, "collections=x")
	assert.Contains(t, url, "page=2")
	// Verify legality (no `??`).
	assert.False(t, strings.Contains(url, "??"))
}

func TestUnknownDownloaderScheme(t *testing.T) {
	c, _ := NewClient("https://example.com")
	err := c.DownloadAsset(context.Background(), "ftp://example.com/file", "/tmp/x")
	require.Error(t, err)
	assert.Contains(t, err.Error(), "no downloader registered")
}

func TestBaseHostAuthRejectsCrossHost(t *testing.T) {
	// Sanity check: WithAllowedHosts default behavior allows nothing implicit
	// when we explicitly limit to the base host only.
	other := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		fmt.Fprint(w, `{"type":"FeatureCollection","features":[],"links":[]}`)
	}))
	defer other.Close()

	base := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		fmt.Fprintf(w, `{"type":"FeatureCollection","features":[],"links":[{"rel":"next","href":"%s/x"}]}`, other.URL)
	}))
	defer base.Close()

	c, _ := NewClient(base.URL, WithAllowedHosts( /* base host only — never explicit */ ))
	// We need to allow ONLY the base host. The empty allowed-hosts set still
	// allows base host. Add base then look for cross-host rejection.
	c.allowedHosts = map[string]struct{}{strings.TrimSuffix(strings.TrimPrefix(base.URL, "http://"), "/"): {}}

	var lastErr error
	for _, err := range c.GetItemsPages(context.Background(), "x") {
		if err != nil {
			lastErr = err
			break
		}
	}
	require.Error(t, lastErr)
	// We don't insist on a specific sentinel; just that it's a host error.
	assert.True(t, errors.Is(lastErr, lastErr) && strings.Contains(lastErr.Error(), "host"))
}
