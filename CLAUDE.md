# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Test Commands

```bash
make build        # Build all packages
make test         # Run all tests
make tui          # Launch the TUI browser
go test ./... -short  # Skip live network integration tests
go test -run TestName ./pkg/client  # Run a single test
```

## Architecture

### Package Structure

- **pkg/client/** - STAC API HTTP client with middleware support and automatic pagination
- **pkg/stac/** - STAC domain types (Item, Collection, Asset, Link, etc.) with foreign member support
- **cmd/tui/** - Terminal UI browser built on tview/tcell for exploring STAC catalogs

### Client Design

The client (`pkg/client/client.go`) uses functional options pattern:
- `WithHTTPClient`, `WithTimeout`, `WithMiddleware`, `WithNextHandler`
- All HTTP calls go through `doRequest()` which applies middleware chain
- Pagination handled by generic `iteratePages[T]()` function using Go 1.23 `iter.Seq2`

### STAC Types

Types in `pkg/stac/` implement custom JSON marshaling to preserve "foreign members" (extension fields not in STAC spec) via `AdditionalFields` maps. This allows round-tripping arbitrary STAC extensions.

### TUI Structure

The TUI (`cmd/tui/`) follows a page-based navigation model:
- `pages.go` - Page setup and navigation
- `handlers.go` - Key event handling
- `formatting/` - Display formatting helpers for collections/items
- Uses streaming iterators for on-demand item loading without buffering entire result sets

### Search Methods

- `SearchSimple()` - GET-based search with URL query parameters
- `SearchCQL2()` - POST-based search with JSON body (initial request POST, pagination follows GET)

### Download Support

`DownloadAsset`/`DownloadAssetWithProgress` handle both HTTP/HTTPS and S3 URLs, with progress callbacks for TUI integration.
