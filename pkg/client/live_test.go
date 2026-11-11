//go:build live

// Live integration tests against real public STAC APIs.
//
//	go test -tags=live -v -run TestLive ./pkg/client/
//
// These tests are NOT run by default — they require network access and
// hit production services. They are intentionally lenient about exact
// counts and IDs (which change as collections grow) but strict about
// response shape, conformance, pagination, and error handling.
package client

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"testing"
	"time"

	cql2 "github.com/exergy-dev/go-cql2"
	cql2json "github.com/exergy-dev/go-cql2/json"
	"github.com/robert-malhotra/go-stac-client/pkg/stac"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

type liveProvider struct {
	name            string
	baseURL         string
	knownCollection string
	cqlCollection   string // collection on which to run a CQL2 cloud_cover filter
	cloudCoverField string
	bbox            []float64

	// supportsCQL2JSON gates the search_cql2_json subtest. Set false for
	// providers that do not advertise the cql2-json conformance class
	// (e.g. Earth Search v1 only supports the legacy STAC Query extension).
	supportsCQL2JSON bool
}

var liveProviders = []liveProvider{
	{
		name:             "earth-search",
		baseURL:          "https://earth-search.aws.element84.com/v1",
		knownCollection:  "sentinel-2-l2a",
		cloudCoverField:  "eo:cloud_cover",
		bbox:             []float64{-122.5, 37.7, -122.3, 37.9}, // SF Bay
		supportsCQL2JSON: false,                                 // Earth Search v1 has no CQL2 conformance
	},
	{
		name:             "planetary-computer",
		baseURL:          "https://planetarycomputer.microsoft.com/api/stac/v1",
		knownCollection:  "sentinel-2-l2a",
		cqlCollection:    "sentinel-2-l2a",
		cloudCoverField:  "eo:cloud_cover",
		bbox:             []float64{-122.5, 37.7, -122.3, 37.9},
		supportsCQL2JSON: true,
	},
}

func liveClient(t *testing.T, p liveProvider) *Client {
	t.Helper()
	c, err := NewClient(p.baseURL,
		WithTimeout(45*time.Second),
		WithUserAgent("go-stac-client-tests/1.0 (+https://github.com/robert-malhotra/go-stac-client)"),
	)
	require.NoError(t, err)
	return c
}

func newCtx(t *testing.T, d time.Duration) (context.Context, context.CancelFunc) {
	t.Helper()
	return context.WithTimeout(context.Background(), d)
}

// skipOnUpstreamFlake treats upstream 5xx, gateway timeouts, or rate-limit
// responses as skip conditions: they are not client-side bugs.
func skipOnUpstreamFlake(t *testing.T, err error) {
	t.Helper()
	if err == nil {
		return
	}
	var herr *HTTPError
	if errors.As(err, &herr) && (herr.Status >= 500 || herr.Status == 429) {
		t.Skipf("upstream flake (HTTP %d): %v", herr.Status, err)
	}
	if errors.Is(err, context.DeadlineExceeded) {
		t.Skipf("upstream slow / timed out: %v", err)
	}
}

// TestLive runs the full live-integration suite against each provider.
func TestLive(t *testing.T) {
	if testing.Short() {
		t.Skip("-short: skipping live integration tests")
	}

	for _, p := range liveProviders {
		p := p
		t.Run(p.name, func(t *testing.T) {
			t.Parallel()

			t.Run("root_catalog", func(t *testing.T) { testLiveRoot(t, p) })
			t.Run("conformance", func(t *testing.T) { testLiveConformance(t, p) })
			t.Run("list_collections", func(t *testing.T) { testLiveListCollections(t, p) })
			t.Run("get_collection", func(t *testing.T) { testLiveGetCollection(t, p) })
			t.Run("queryables", func(t *testing.T) { testLiveQueryables(t, p) })
			t.Run("search_simple_bbox", func(t *testing.T) { testLiveSearchSimple(t, p) })
			t.Run("search_post_bbox", func(t *testing.T) { testLiveSearchPost(t, p) })
			t.Run("search_paginates", func(t *testing.T) { testLivePagination(t, p) })
			if p.supportsCQL2JSON {
				t.Run("search_cql2_json", func(t *testing.T) { testLiveCQL2(t, p) })
			}
			t.Run("not_found_error", func(t *testing.T) { testLiveNotFound(t, p) })
			t.Run("get_known_item", func(t *testing.T) { testLiveGetKnownItem(t, p) })
		})
	}
}

func testLiveRoot(t *testing.T, p liveProvider) {
	cli := liveClient(t, p)
	ctx, cancel := newCtx(t, 45*time.Second)
	defer cancel()

	cat, err := cli.GetCatalog(ctx)
	require.NoError(t, err)
	assert.NotEmpty(t, cat.ID)
	assert.NotEmpty(t, cat.Description)
	assert.NotEmpty(t, cat.Links, "root catalog has no links")
	assert.NotEmpty(t, cat.ConformsTo, "root catalog must declare conformance")

	// At least one well-known link rel must be present.
	wantRel := map[string]bool{"self": false, "data": false, "search": false}
	for _, l := range cat.Links {
		if _, ok := wantRel[l.Rel]; ok {
			wantRel[l.Rel] = true
		}
	}
	for rel, ok := range wantRel {
		assert.Truef(t, ok, "root catalog missing rel=%q", rel)
	}
}

func testLiveConformance(t *testing.T, p liveProvider) {
	cli := liveClient(t, p)
	ctx, cancel := newCtx(t, 45*time.Second)
	defer cancel()

	classes, err := cli.GetConformance(ctx)
	require.NoError(t, err)
	assert.NotEmpty(t, classes)

	// Both providers MUST declare core + item-search.
	supportsCore, err := cli.SupportsConformance(ctx, stac.ConformanceCore)
	require.NoError(t, err)
	assert.True(t, supportsCore, "%s must declare STAC API core", p.name)

	supportsSearch, err := cli.SupportsConformance(ctx, stac.ConformanceItemSearch)
	require.NoError(t, err)
	assert.True(t, supportsSearch, "%s must declare item-search", p.name)
}

func testLiveListCollections(t *testing.T, p liveProvider) {
	cli := liveClient(t, p)
	ctx, cancel := newCtx(t, 45*time.Second)
	defer cancel()

	count := 0
	saw := map[string]bool{}
	for col, err := range cli.GetCollections(ctx) {
		require.NoError(t, err)
		require.NotEmpty(t, col.ID)
		saw[col.ID] = true
		count++
		if count >= 25 {
			break
		}
	}
	assert.GreaterOrEqual(t, count, 1, "expected >=1 collection from %s", p.name)
	if p.knownCollection != "" {
		// The well-known collection should be reachable; we may have stopped
		// before seeing it (alphabetical), so don't assert on saw[].
		_ = saw
	}
}

func testLiveGetCollection(t *testing.T, p liveProvider) {
	cli := liveClient(t, p)
	ctx, cancel := newCtx(t, 45*time.Second)
	defer cancel()

	col, err := cli.GetCollection(ctx, p.knownCollection)
	require.NoError(t, err)
	assert.Equal(t, p.knownCollection, col.ID)
	require.NotNil(t, col.Extent, "collection must have extent")
	assert.NotEmpty(t, col.License, "collection must have license")
}

func testLiveQueryables(t *testing.T, p liveProvider) {
	cli := liveClient(t, p)
	ctx, cancel := newCtx(t, 45*time.Second)
	defer cancel()

	// Try collection-scoped queryables; fall back to global.
	q, err := cli.GetQueryables(ctx, p.knownCollection)
	if err != nil {
		if errors.Is(err, ErrNotFound) {
			q, err = cli.GetGlobalQueryables(ctx)
			if errors.Is(err, ErrNotFound) {
				t.Skip("queryables endpoint not available")
			}
		}
	}
	skipOnUpstreamFlake(t, err)
	require.NoError(t, err)
	assert.NotNil(t, q)
}

func testLiveSearchSimple(t *testing.T, p liveProvider) {
	cli := liveClient(t, p)
	ctx, cancel := newCtx(t, 45*time.Second)
	defer cancel()

	params := SearchParams{
		Collections: []string{p.knownCollection},
		Bbox:        p.bbox,
		Datetime:    "2024-06-01T00:00:00Z/2024-06-30T23:59:59Z",
		Limit:       3,
	}
	count := 0
	for it, err := range cli.SearchSimple(ctx, params) {
		skipOnUpstreamFlake(t, err)
		require.NoError(t, err)
		require.NotEmpty(t, it.ID)
		assert.Equal(t, p.knownCollection, it.Collection, "item collection mismatch")
		count++
		if count >= 3 {
			break
		}
	}
	assert.GreaterOrEqual(t, count, 1, "GET /search bbox returned 0 items")
}

func testLiveSearchPost(t *testing.T, p liveProvider) {
	cli := liveClient(t, p)
	ctx, cancel := newCtx(t, 45*time.Second)
	defer cancel()

	params := SearchParams{
		Collections: []string{p.knownCollection},
		Bbox:        p.bbox,
		Datetime:    "2024-06-01T00:00:00Z/2024-06-30T23:59:59Z",
		Limit:       3,
	}
	matched := -1
	pages := 0
	for page, err := range cli.SearchPages(ctx, params) {
		skipOnUpstreamFlake(t, err)
		require.NoError(t, err)
		require.NotEmpty(t, page.Items, "POST /search returned an empty page")
		if page.NumberMatched != nil {
			matched = *page.NumberMatched
		}
		pages++
		if pages >= 1 {
			break
		}
	}
	t.Logf("%s POST /search bbox numberMatched=%d", p.name, matched)
}

func testLivePagination(t *testing.T, p liveProvider) {
	cli := liveClient(t, p)
	ctx, cancel := newCtx(t, 60*time.Second)
	defer cancel()

	params := SearchParams{
		Collections: []string{p.knownCollection},
		Bbox:        p.bbox,
		Datetime:    "2024-01-01T00:00:00Z/2024-12-31T23:59:59Z",
		Limit:       2, // small page size to force pagination
	}
	totalItems := 0
	totalPages := 0
	seenIDs := map[string]bool{}
	for it, err := range cli.Search(ctx, params) {
		skipOnUpstreamFlake(t, err)
		require.NoError(t, err)
		assert.False(t, seenIDs[it.ID], "duplicate item across pages: %s", it.ID)
		seenIDs[it.ID] = true
		totalItems++
		if totalItems%2 == 0 {
			totalPages++
		}
		if totalItems >= 6 { // 3 pages worth
			break
		}
	}
	assert.GreaterOrEqual(t, totalItems, 4, "expected to paginate >=2 pages on %s", p.name)
}

func testLiveCQL2(t *testing.T, p liveProvider) {
	cli := liveClient(t, p)
	ctx, cancel := newCtx(t, 45*time.Second)
	defer cancel()

	// Sanity check: the test is registered only when the provider is
	// configured to support CQL2-JSON, but verify the live conformance
	// document agrees so a server change does not produce a silent pass.
	supported, err := cli.SupportsConformance(ctx, stac.ConformanceCQL2JSON)
	require.NoError(t, err)
	require.True(t, supported, "%s no longer declares cql2-json — update liveProviders", p.name)

	// eo:cloud_cover < 5 AND collection = sentinel-2-l2a
	expr := cql2.And(
		cql2.Lt(p.cloudCoverField, 5),
		cql2.Eq("collection", p.cqlCollection),
	)
	filter, err := cql2json.Encode(expr.Node())
	require.NoError(t, err)
	t.Logf("%s cql2-json filter: %s", p.name, filter)

	params := SearchParams{
		Filter:     json.RawMessage(filter),
		FilterLang: "cql2-json",
		Bbox:       p.bbox,
		Datetime:   "2024-06-01T00:00:00Z/2024-06-30T23:59:59Z",
		Limit:      5,
	}

	count := 0
	for it, err := range cli.Search(ctx, params) {
		skipOnUpstreamFlake(t, err)
		require.NoError(t, err)
		count++
		if it.Properties != nil {
			if v, ok := it.Properties[p.cloudCoverField]; ok {
				if f, ok := v.(float64); ok {
					assert.Less(t, f, 5.0, "%s returned an item with cloud_cover=%f despite filter", p.name, f)
				}
			}
		}
		// Sanity-check geometry decodes to GeoJSON.
		if len(it.Geometry) > 0 {
			var geom map[string]any
			require.NoError(t, json.Unmarshal(it.Geometry, &geom))
			if typ, ok := geom["type"].(string); ok {
				assert.NotEmpty(t, typ)
			}
		}
		if count >= 5 {
			break
		}
	}
	assert.GreaterOrEqual(t, count, 1, "CQL2 search returned no items")
}

func testLiveNotFound(t *testing.T, p liveProvider) {
	cli := liveClient(t, p)
	ctx, cancel := newCtx(t, 30*time.Second)
	defer cancel()

	_, err := cli.GetCollection(ctx, "this-collection-definitely-does-not-exist-"+strings.Repeat("x", 10))
	require.Error(t, err)
	assert.True(t, errors.Is(err, ErrNotFound) || errors.Is(err, ErrServer),
		"expected ErrNotFound (or 5xx) from %s, got: %v", p.name, err)
}

func testLiveGetKnownItem(t *testing.T, p liveProvider) {
	cli := liveClient(t, p)
	ctx, cancel := newCtx(t, 60*time.Second)
	defer cancel()

	// Find any item id by running a tight bbox+datetime search.
	params := SearchParams{
		Collections: []string{p.knownCollection},
		Bbox:        p.bbox,
		Datetime:    "2024-06-01T00:00:00Z/2024-06-30T23:59:59Z",
		Limit:       1,
	}
	var found *stac.Item
	for it, err := range cli.Search(ctx, params) {
		require.NoError(t, err)
		found = it
		break
	}
	if found == nil {
		t.Skip("could not find a sample item to fetch")
	}
	t.Logf("%s sample item id=%s", p.name, found.ID)

	got, err := cli.GetItem(ctx, p.knownCollection, found.ID)
	require.NoError(t, err)
	assert.Equal(t, found.ID, got.ID)
	assert.Equal(t, p.knownCollection, got.Collection)
	assert.NotEmpty(t, got.Assets, "item must have assets")

	// At least one asset should have a usable href.
	for k, a := range got.Assets {
		require.NotNil(t, a)
		if a.Href != "" {
			t.Logf("%s asset %s href=%s type=%s", p.name, k, a.Href, a.Type)
			break
		}
	}

	// Geometry round-trip: marshal returned item and re-unmarshal.
	b, err := json.Marshal(got)
	require.NoError(t, err)
	var rt stac.Item
	require.NoError(t, json.Unmarshal(b, &rt))
	assert.Equal(t, got.ID, rt.ID)

	// The custom marshaller must always emit a "type":"Feature" field.
	var generic map[string]any
	require.NoError(t, json.Unmarshal(b, &generic))
	assert.Equal(t, "Feature", generic["type"])
	fmt.Println() // newline between providers in -v output
}
