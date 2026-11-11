# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - Unreleased

First stable release. STAC API 1.0.0 client targeting Go 1.23+.

### Added

- Streaming iteration over `/collections`, `/collections/{id}/items`, and
  `/search` results via Go 1.23 `iter.Seq2`.
- Pagination cycle detection (`ErrPaginationCycle`) and configurable
  max-page cap (`WithMaxPages`, default 10000).
- Typed errors: `*HTTPError` (with `RetryAfter`, decoded `*APIError`),
  sentinel errors `ErrNotFound`, `ErrUnauthorized`, `ErrForbidden`,
  `ErrRateLimited`, `ErrServer`, `ErrPaginationCycle`,
  `ErrPaginationLimit`, `ErrUnsupportedForGET`, `ErrResponseTooLarge`,
  `ErrUnexpectedContentType`.
- Built-in auth helpers `WithBearerToken` and `WithAPIKey` that limit
  credential propagation to the base URL's origin.
- `WithAllowedHosts` allowlist for cross-origin pagination next-links.
- Configurable response body limit (`WithMaxBodyBytes`, default 64 MiB)
  and JSON Content-Type guard.
- Default User-Agent `go-stac-client/<version>`.
- `Client.DoHTTP(ctx, *http.Request)` low-level escape hatch.
- `/conformance` endpoint fallback when the root catalog omits `conformsTo`.
- CQL2 integration via [github.com/exergy-dev/go-cql2](https://github.com/exergy-dev/go-cql2)
  with `CQL2JSON`/`MustCQL2JSON` and `CQL2Text`/`MustCQL2Text` helpers.
- Pluggable scheme downloaders via `RegisterDownloader`; S3 support is
  opt-in by importing `pkg/client/s3`.
- Fuzz tests for `Item`, `Collection`, `Catalog`, and `Link` unmarshal.
- OGC API Features Part 1 conformance constants.

### Changed (breaking from pre-1.0)

- `Provider.Url` renamed to `Provider.URL` (Go convention).
- `Item.Geometry` typed as `json.RawMessage` (was `any`) — preserves 3D
  coordinates and arbitrary GeoJSON shapes losslessly. Decode it with
  `json.Unmarshal(item.Geometry, &dst)`.
- `TemporalExtent.Interval` typed as `[][]*string` (was `[][]any`) —
  preserves nullable open-ended interval endpoints precisely.
- `Error` struct renamed to `APIError` and now implements `error`. STAC
  API error documents are surfaced via `*HTTPError.API`.
- `SearchSimple` rejects parameters that cannot be expressed in a GET
  request (`Intersects`, `Filter` with `cql2-json`) with
  `ErrUnsupportedForGET` instead of silently dropping them.
- GET `/search` `sortby` is encoded with the `+`/`-` sign convention
  (e.g. `+datetime,-eo:cloud_cover`) per the STAC API Sort extension.
- GET `/search` `bbox` is encoded with `strconv.FormatFloat(v, 'f', -1, 64)`
  to avoid scientific-notation surprises.
- `SortField.Direction` is the typed `SortDirection` (`SortAsc`/`SortDesc`).
- `Middleware` signature simplified to `func(*http.Request) error` (no
  ambient context — the request already carries its `Context()`).
- `WithTimeout` no longer mutates the caller's `*http.Client`; the
  per-request timeout is applied via `context.WithTimeout`.
- `OffsetDecoder`, `PageNumberDecoder`, `CursorDecoder` build URLs by
  merging into the existing query string and return a fresh stateful
  closure per call.
- S3 download support moved to `pkg/client/s3`. Default builds no longer
  pull in the AWS SDK.
- The custom CQL2 builder under `pkg/client` was removed in favor of
  `github.com/exergy-dev/go-cql2`. `paulmach/orb` and `planetlabs/go-ogc`
  are no longer dependencies.

### Removed

- `Client.SearchCQL2` (alias for `Search` — use `Search` directly).
- The `Geometry`, `Point`, `Polygon`, … and `FilterBuilder` symbols from
  the old custom CQL2 layer. Use `go-cql2` builders instead.

### Fixed

- `Item`/`Collection`/`Catalog` `MarshalJSON` no longer let
  `AdditionalFields["type"]` override the canonical type field.
- Asset download now removes partial files on error and propagates
  `Close()` errors.
- `LinkDecoder` returns the body verbatim through size-limited readers,
  preventing JSON-bomb / unbounded-read scenarios.
- `QueryableField.Type` is now `JSONSchemaType`, accepting both the JSON
  Schema bare-string form (`"integer"`) and the array form
  (`["integer", "null"]`) used by Microsoft Planetary Computer for
  nullable fields. Previous releases failed to decode such queryables.

### Geometry

- TUI's `cmd/tui/formatting/geometry.go` no longer walks GeoJSON via
  `map[string]any`. Parsing and type identification are delegated to
  [github.com/exergy-dev/go-topology-suite](https://github.com/exergy-dev/go-topology-suite)'s
  `geojson` and `geom` packages, so the TUI shares the same OGC-compliant
  parser as the rest of the geospatial stack and gets correct envelopes,
  ring/hole accounting, and GeometryCollection support for free.

### CQL2 in the TUI filter builder

- TUI's `cmd/tui/filter_builder.go` no longer constructs CQL2-JSON by
  hand-rolling `map[string]any` literals. Each UI condition is mapped to
  a [github.com/exergy-dev/go-cql2](https://github.com/exergy-dev/go-cql2)
  builder call (`cql2.Eq`, `cql2.Lt`, `cql2.Like`, `cql2.IsNull`, …) and
  the combined expression is encoded through `cql2/json.Encode`. All CQL2
  generation in the project now goes through go-cql2.

### CQL2 wrappers removed

- The thin convenience wrappers `CQL2JSON`, `MustCQL2JSON`, `CQL2Text`,
  `MustCQL2Text`, and the `FilterLangCQL2JSON` / `FilterLangCQL2Text`
  constants are removed from `pkg/client`. Users encode CQL2 directly
  through `go-cql2` and supply the bytes to `SearchParams.Filter`:
  ```go
  b, err := cql2json.Encode(expr.Node())
  params := client.SearchParams{
      Filter:     json.RawMessage(b),
      FilterLang: "cql2-json",
  }
  ```
  This is breaking for anyone relying on the wrappers; migration is a
  two-line change at each call site.

### Live integration

- `pkg/client/live_test.go` (build tag `live`) covers Element84 Earth
  Search and Microsoft Planetary Computer end-to-end: root catalog,
  conformance, list/get collections, queryables, GET and POST search,
  multi-page pagination, CQL2-JSON filter, typed `ErrNotFound`, and item
  round-trip. Run with `make test-live`.
