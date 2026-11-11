# go-stac-client

A Go client library for [SpatioTemporal Asset Catalog (STAC)](https://stacspec.org/) APIs.

- Typed STAC 1.0.0 model types (`Item`, `Collection`, `Catalog`, `Link`, `Asset`, `Queryables`, …) with foreign-member preservation
- Streaming iteration over collections, items, and search results via Go 1.23 `iter.Seq2`
- Automatic pagination with cycle detection and a configurable max-page cap
- Typed errors (`HTTPError`, `APIError`, `ErrNotFound`, `ErrRateLimited`, …) with `Retry-After` parsing
- Per-request timeouts that never mutate caller-supplied `*http.Client`
- Bearer-token / API-key middleware that sends credentials only to the base URL's origin
- CQL2 filter integration via [github.com/exergy-dev/go-cql2](https://github.com/exergy-dev/go-cql2)
- Pluggable scheme downloaders (HTTP/HTTPS by default, S3 via `pkg/client/s3`)
- A terminal UI (`cmd/tui`) for interactive STAC exploration

## Installation

```bash
go get github.com/robert-malhotra/go-stac-client@latest
```

To launch the TUI:

```bash
go run github.com/robert-malhotra/go-stac-client/cmd/tui@latest
```

## Quick start

```go
package main

import (
    "context"
    "fmt"

    stacclient "github.com/robert-malhotra/go-stac-client/pkg/client"
)

func main() {
    cli, err := stacclient.NewClient("https://earth-search.aws.element84.com/v1")
    if err != nil {
        panic(err)
    }

    // Stream collections.
    for col, err := range cli.GetCollections(context.Background()) {
        if err != nil {
            panic(err)
        }
        fmt.Println(col.ID)
    }

    // Search items by bbox + datetime, paginating on demand.
    params := stacclient.SearchParams{
        Collections: []string{"sentinel-2-l2a"},
        Bbox:        []float64{-123.3, 45.2, -122.5, 46.0},
        Datetime:    "2024-01-01T00:00:00Z/2024-02-01T00:00:00Z",
        Limit:       50,
    }
    n := 0
    for it, err := range cli.Search(context.Background(), params) {
        if err != nil {
            panic(err)
        }
        n++
        if n >= 100 {
            break // iterator stops cleanly
        }
        fmt.Println(it.ID)
    }
}
```

## CQL2 filters

Build CQL2 expressions with [`go-cql2`](https://github.com/exergy-dev/go-cql2)
and encode them through its `cql2/json` (or `cql2/text`) codec. Pass the
bytes into `SearchParams.Filter` as a `json.RawMessage`:

```go
import (
    "encoding/json"

    cql2 "github.com/exergy-dev/go-cql2"
    cql2json "github.com/exergy-dev/go-cql2/json"
    stacclient "github.com/robert-malhotra/go-stac-client/pkg/client"
)

expr := cql2.And(
    cql2.Lt("eo:cloud_cover", 10),
    cql2.Eq("collection", "sentinel-2-l2a"),
)
b, err := cql2json.Encode(expr.Node())
if err != nil { return err }

params := stacclient.SearchParams{
    Filter:     json.RawMessage(b),
    FilterLang: "cql2-json",
}
```

For CQL2-text on `GET /search`, encode via `cql2/text.Encode` and set
`FilterLang: "cql2-text"`.

## Authentication

```go
cli, _ := stacclient.NewClient(baseURL,
    stacclient.WithBearerToken(os.Getenv("STAC_TOKEN")),
)

// Or a custom header:
cli, _ := stacclient.NewClient(baseURL,
    stacclient.WithAPIKey("X-Api-Key", os.Getenv("API_KEY")),
)
```

Credentials are sent only to the base URL's origin (scheme + host + port).
If a server returns a next-page link pointing at a different origin (for
example a CDN), the credential is **not** sent there. Use
`WithAllowedHosts(host…)` to additionally allowlist alternate hosts the
client may follow during pagination.

## Error handling

```go
_, err := cli.GetItem(ctx, "no-such-collection", "no-such-item")
switch {
case errors.Is(err, stacclient.ErrNotFound):
    // 404
case errors.Is(err, stacclient.ErrRateLimited):
    var herr *stacclient.HTTPError
    errors.As(err, &herr)
    time.Sleep(herr.RetryAfter)
case errors.Is(err, stacclient.ErrUnauthorized):
    // refresh token
case err != nil:
    return err
}
```

## S3 asset downloads

`Client.DownloadAsset` handles `http`/`https` out of the box. To enable
`s3://`, side-effect-import the s3 subpackage:

```go
import (
    stacclient "github.com/robert-malhotra/go-stac-client/pkg/client"
    _ "github.com/robert-malhotra/go-stac-client/pkg/client/s3"
)

cli.DownloadAsset(ctx, "s3://my-bucket/path/to/asset.tif", "/tmp/asset.tif")
```

This isolates the AWS SDK dependency to consumers who need it.

## Examples

See the `examples/` directory for runnable programs covering basic search,
CQL2 filtering, pagination, and authenticated requests.

## Testing

```bash
go test -short ./...                 # offline unit tests only
go test -short -race ./...           # with race detector
make test-live                       # live tests against Earth Search + PC
```

The `live` build tag gates an integration suite that runs against
[Element84 Earth Search](https://earth-search.aws.element84.com/v1) and
[Microsoft Planetary Computer](https://planetarycomputer.microsoft.com/api/stac/v1).
Each provider is exercised end-to-end: root catalog, conformance, listing
and fetching collections, queryables, GET and POST search, multi-page
pagination, CQL2-JSON filter, typed `ErrNotFound`, and item round-trip.

## License

[MIT](LICENSE)
