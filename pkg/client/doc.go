// Package client implements a STAC API client.
//
// # Overview
//
// The Client is constructed from a base URL plus optional ClientOptions:
//
//	cli, err := client.NewClient("https://earth-search.aws.element84.com/v1",
//	    client.WithTimeout(15*time.Second),
//	    client.WithBearerToken(os.Getenv("STAC_TOKEN")),
//	    client.WithAllowedHosts("cdn.example.com"),
//	)
//
// All STAC API endpoints are exposed as methods on *Client:
//
//	cat, err := cli.GetCatalog(ctx)
//	conf, err := cli.GetConformance(ctx)
//	col, err := cli.GetCollection(ctx, "sentinel-2-l2a")
//	q, err := cli.GetQueryables(ctx, "sentinel-2-l2a")
//	item, err := cli.GetItem(ctx, "sentinel-2-l2a", "S2A_…")
//
// # Streaming results
//
// Lists and search results are exposed as Go 1.23 iter.Seq2[*T, error]
// iterators that page on demand. Stop early with break, or use the *Pages
// variants to access numberMatched and numberReturned:
//
//	for col, err := range cli.GetCollections(ctx) {
//	    if err != nil { return err }
//	    fmt.Println(col.ID)
//	}
//
//	for page, err := range cli.SearchPages(ctx, params) {
//	    if err != nil { return err }
//	    if page.NumberMatched != nil {
//	        fmt.Printf("matched=%d returned=%d\n", *page.NumberMatched, *page.NumberReturned)
//	    }
//	}
//
// # Search
//
// Search supports both POST (full-fidelity) and GET (subset) forms:
//
//	cli.Search(ctx, params)        // POST /search, JSON body
//	cli.SearchSimple(ctx, params)  // GET  /search?...
//
// SearchSimple cannot represent the Intersects parameter or CQL2-JSON
// filters; both return ErrUnsupportedForGET.
//
// # CQL2 filters
//
// Build filters with github.com/exergy-dev/go-cql2 and encode them through
// its cql2/json (or cql2/text) codec. Pass the resulting bytes via
// SearchParams.Filter as a json.RawMessage so the body embeds the filter
// verbatim:
//
//	import (
//	    cql2 "github.com/exergy-dev/go-cql2"
//	    cql2json "github.com/exergy-dev/go-cql2/json"
//	)
//
//	expr := cql2.And(
//	    cql2.Lt("eo:cloud_cover", 10),
//	    cql2.SIntersects("geometry", cql2.Geom(myGeometry)),
//	)
//	b, err := cql2json.Encode(expr.Node())
//	if err != nil { return err }
//	params := client.SearchParams{
//	    Collections: []string{"sentinel-2-l2a"},
//	    Filter:      json.RawMessage(b),
//	    FilterLang:  "cql2-json",
//	}
//
// # Authentication
//
// WithBearerToken and WithAPIKey install middleware that send credentials
// only to the base URL's origin (scheme+host+port), so credentials are not
// leaked when the server returns next-page links pointing at a different
// host (a common CDN-fronted pattern). For finer-grained control, supply
// your own Middleware via WithMiddleware.
//
// # Error model
//
// Non-2xx HTTP responses return *HTTPError, which:
//
//   - implements error
//   - matches sentinels via errors.Is: ErrNotFound, ErrUnauthorized,
//     ErrForbidden, ErrRateLimited, ErrServer
//   - exposes RetryAfter parsed from the Retry-After header
//   - wraps a decoded *APIError when the body is a STAC API error document
//
// Pagination has its own sentinels: ErrPaginationCycle (next-link revisited
// a page already fetched) and ErrPaginationLimit (configured MaxPages
// reached — default 10000).
//
// Bodies are size-limited (default 64 MiB) to mitigate memory exhaustion;
// override with WithMaxBodyBytes.
//
// # Asset downloads
//
// DownloadAsset and DownloadAssetWithProgress fetch resources by URL,
// dispatching by scheme. The default builds support http and https; import
// pkg/client/s3 to enable s3:// downloads. Custom schemes can be plugged in
// via RegisterDownloader.
package client
