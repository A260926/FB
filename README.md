# go-webdav

[![Go Reference](https://pkg.go.dev/badge/github.com/emersion/go-webdav.svg)](https://pkg.go.dev/github.com/emersion/go-webdav)

A Go library for [WebDAV], [CalDAV] and [CardDAV].

## Discovering WebDAV bindings

`Client.ReadDir` with `recursive` set to true delegates a `Depth: infinity`
PROPFIND to the server. When a collection is mounted under several names (or
when a binding points back at an ancestor), that response does not tell which
URLs are aliases of the same resource, and it can expand the same collection
many times.

`Client.DiscoverBindings` walks the tree itself with one `Depth: 1` PROPFIND
per directory and returns a finite `BindingGraph`:

```go
package main

import (
	"context"
	"fmt"
	"log"

	"github.com/emersion/go-webdav"
)

func main() {
	client, err := webdav.NewClient(nil, "https://dav.example/dav/root/")
	if err != nil {
		log.Fatal(err)
	}

	graph, err := client.DiscoverBindings(context.Background(), "")
	if err != nil {
		log.Fatal(err)
	}

	fmt.Println("root resource ID:", graph.RootID)
	for _, node := range graph.Nodes {
		kind := "file"
		if node.IsDir {
			kind = "directory"
		}
		fmt.Printf("node %s: %s (%s)\n", node.ID, node.URL, kind)
	}
	for _, edge := range graph.Edges {
		fmt.Printf("binding %s -> %s (from %s)\n", edge.URL, edge.TargetID, edge.ParentID)
	}
}
```

Nodes and edges answer two different questions:

* A `BindingNode` is a resource. `BindingNode.ID` is the opaque, case-preserved
  value of the server's `DAV:resource-id`; each ID appears exactly once.
  `BindingNode.URL` is the absolute URL at which that resource was **first**
  discovered. It is a convenience, not an address that should be derived from
  the ID.
* A `BindingEdge` is a binding (a member entry) seen in a directory response.
  `BindingEdge.URL` is the absolute URL exactly as that binding appeared in
  the server's response. Several edges (for example `/left/` and `/right/`)
  can therefore carry different URLs while sharing the same `TargetID`, and a
  binding pointing back at an ancestor is kept as an ordinary edge. Edges are
  discovered in breadth-first order, and within one response members are
  sorted by their absolute URL so the graph does not depend on server
  response ordering.

Directories are expanded once: when a later alias points at an already
expanded directory, only the edge is recorded. Any malformed response,
duplicate/conflicting href, missing property, non-success status, out-of-bounds
member URL or XML error makes `DiscoverBindings` return `nil` and an error
instead of a partial graph.

## License

MIT

[WebDAV]: https://tools.ietf.org/html/rfc4918
[CalDAV]: https://tools.ietf.org/html/rfc4791
[CardDAV]: https://tools.ietf.org/html/rfc6352
