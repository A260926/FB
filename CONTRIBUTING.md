# Contributing

Thanks for your interest in `go-stac-client`. This document describes how
we work and what we expect from contributions.

## Versioning

The library follows [Semantic Versioning](https://semver.org/). The public
API consists of all exported identifiers under `pkg/stac` and `pkg/client`
(plus `pkg/client/s3`). Breaking changes require a new major version; the
release notes in `CHANGELOG.md` call them out explicitly.

`cmd/tui` is not part of the stable API surface and may change between
minor releases.

## Supported STAC versions

The library targets STAC 1.0.0 data documents and STAC API 1.0.0
endpoints, including the Filter (CQL2-JSON / CQL2-text), Sort, Fields, and
Query extensions. OGC API Features Part 1 endpoints work where they
overlap with STAC.

## Development

```bash
go test ./...           # all tests
go test -race ./...     # race detector
go test -short ./...    # skip live integration tests
go vet ./...
```

Please run `gofmt` (or `go fmt ./...`) before submitting a pull request.
New exported identifiers need doc comments.

## Filing issues

When filing a bug, include:

- The Go version (`go version`) and OS/arch.
- A minimal reproducer if possible.
- The STAC API endpoint(s) involved (URL of the root catalog).
- The full error string and, where relevant, the `*HTTPError`'s `Status`
  and `Body`.

## Pull requests

- One change per PR; rebase on `main` before requesting review.
- Add tests for new behavior; add fuzz seeds for new unmarshal paths.
- Update `CHANGELOG.md` under the "Unreleased" heading.
- Avoid adding dependencies; if a new dependency is unavoidable, justify
  it in the PR description.

## License

By contributing you agree that your contributions are licensed under the
MIT license that covers the project.
