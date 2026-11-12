# microformats

[![GoDoc](https://img.shields.io/badge/godoc-reference-blue)](https://pkg.go.dev/willnorris.com/go/microformats)
[![Test Status](https://github.com/willnorris/microformats/workflows/ci/badge.svg)](https://github.com/willnorris/microformats/actions?query=workflow%3Aci)
[![Test Coverage](https://codecov.io/gh/willnorris/microformats/branch/main/graph/badge.svg)](https://codecov.io/gh/willnorris/microformats)

microformats is a go library and tool for parsing [microformats][], supporting both classic v1 and [v2 syntax][].
It is based on Andy Leap's [original library][andyleap/microformats].

[microformats]: https://microformats.io/
[v2 syntax]: https://microformats.org/wiki/microformats-2
[andyleap/microformats]: https://github.com/andyleap/microformats

## Usage

To see this package in action, the simplest way is to install the command line
app and use it to fetch and parse a webpage with microformats on it:

```sh
% go install willnorris.com/go/microformats/cmd/gomf@latest
% gomf https://indieweb.org
```

To use it in your own code, import the package:

```go
import "willnorris.com/go/microformats"
```

If you have the HTML contents of a page in an [io.Reader][], call [Parse][] like in this example:

```go
content := `<article class="h-entry"><h1 class="p-name">Hello</h1></article>`
r := strings.NewReader(content)

data := microformats.Parse(r, nil)

// do something with data, or just print it out as JSON:
enc := json.NewEncoder(os.Stdout)
enc.SetIndent("", "  ")
enc.Encode(data)
```

Alternately, if you have already parsed the page and have an [html.Node][], then call [ParseNode][].
For example, you might want to select a subset of the DOM, and parse only that for microformats.
An example of doing this with the [goquery package] can be seen in [cmd/gomf/main.go](cmd/gomf/main.go).

To see that in action using the gomf app installed above,
you can parse the microformats from indieweb.org that appear within the `#content` element:

```sh
% gomf https://indieweb.org "#content"

{
  "items": [
    {
      "id": "content",
      "type": [
        "h-entry"
      ],
      "properties": ...
      "children": ...
    }
  ],
  "rels": {},
  "rel-urls": {}
}
```

[Parse]: https://pkg.go.dev/willnorris.com/go/microformats#Parse
[ParseNode]: https://pkg.go.dev/willnorris.com/go/microformats#ParseNode
[io.Reader]: https://golang.org/pkg/io/#Reader
[html.Node]: https://pkg.go.dev/golang.org/x/net/html#Node
[goquery package]: https://github.com/PuerkitoBio/goquery

## Include pattern and parse stability

Classic (v1) pages may pull shared fragments into a microformat with the
[include pattern][] (`a.include`, `object.include`) or with
`itemref`/`headers` references.  This parser expands those references into a
private copy of the document, so calling Parse or ParseNode never modifies
the supplied tree, and repeated parses of the same tree yield identical
results.

Reference cycles are terminated by skipping only the back edge: while
following a chain of references, a reference to a node already on the active
expansion path is ignored, but the rest of that node's normal content and
its sibling references are processed normally.  This is different from
legitimately referencing the same fragment more than once — two sibling
positions referencing one fragment, a duplicated token in a single
`itemref="x x"` attribute, or a diamond of references all cause the fragment
to contribute its data once per reference, since each reference is expanded
independently.

[include pattern]: https://microformats.org/wiki/include-pattern

## Additional helper packages

Use the [ptd package] to perform [Post Type Discovery].

Use the [rhc package] to find a [Representative h-card].

[ptd package]: https://pkg.go.dev/willnorris.com/go/microformats/ptd
[Post Type Discovery]: https://www.w3.org/TR/post-type-discovery/
[rhc package]: https://pkg.go.dev/willnorris.com/go/microformats/rhc
[Representative h-card]: http://microformats.org/wiki/representative-hcard
