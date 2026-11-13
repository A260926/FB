# gofeed

[![CI](https://github.com/mmcdole/gofeed/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/mmcdole/gofeed/actions/workflows/ci.yml) [![codecov](https://codecov.io/gh/mmcdole/gofeed/branch/master/graph/badge.svg)](https://codecov.io/gh/mmcdole/gofeed) [![Go Reference](https://pkg.go.dev/badge/github.com/mmcdole/gofeed.svg)](https://pkg.go.dev/github.com/mmcdole/gofeed) [![License](http://img.shields.io/:license-mit-blue.svg)](http://doge.mit-license.org)

# Gofeed: A Robust Feed Parser for Golang

<img src="https://github.com/mmcdole/gofeed/assets/3767096/ab4e7b0e-1472-4249-880c-c6784000ed31" width="150" height="150"> 
<br /><br />

`gofeed` is a powerful and flexible library designed for parsing **RSS**, **Atom**, and **JSON** feeds across various formats and versions. It effectively manages non-standard elements and known extensions, and demonstrates resilience against common feed issues.

## Table of Contents
- [Features](#features)
- [Overview](#overview)
- [Basic Usage](#basic-usage)
- [Walking Paginated Feeds](#walking-paginated-feeds)
- [Advanced Usage](#advanced-usage)
- [Dependencies](#dependencies)
- [License](#license)
- [Credits](#credits)

## Features

### Comprehensive Feed Support
- RSS (0.90 to 2.0)
- Atom (0.3, 1.0)
- JSON (1.0, 1.1)

### Handling Invalid Feeds
`gofeed` takes a best-effort approach to deal with broken or invalid XML feeds, capable of handling issues like:
- Unescaped markup
- Undeclared namespace prefixes
- Missing or illegal tags
- Incorrect date formats
- ...and more.

### Extension Support

`gofeed` treats elements outside the feed's default namespace as extensions, storing them in tree-like structures under Feed.Extensions and Item.Extensions. This feature allows you to access custom extension elements easily.

Built-In Support for Popular Extensions
For added convenience, gofeed includes native support for parsing certain well-known extensions into dedicated structs. Currently, it supports:

- Dublin Core: Accessible via `Feed.DublinCoreExt` and `Item.DublinCoreExt`
- Apple iTunes: Accessible via `Feed.ITunesExt` and `Item.ITunesExt`
  
## Overview

In `gofeed`, you have two primary choices for feed parsing: a universal parser for handling multiple feed types seamlessly, and specialized parsers for more granular control over individual feed types.


### Universal Feed Parser 

The universal `gofeed.Parser` is designed to make it easy to work with various types of feeds—RSS, Atom, JSON—by converting them into a unified `gofeed.Feed` model. This is especially useful when you're dealing with multiple feed formats and you want to treat them the same way.

The universal parser uses built-in translators like `DefaultRSSTranslator`, `DefaultAtomTranslator`, and `DefaultJSONTranslator` to convert between the specific feed types and the universal feed. Not happy with the defaults? Implement your own `gofeed.Translator` to tailor the translation process to your needs.

### Specialized Feed Parsers: RSS, Atom, JSON

Alternatively, if your focus is on a single feed type, then using a specialized parser offers advantages in terms of performance and granularity. For example, if you're interested solely in RSS feeds, you can use `rss.Parser` directly. These feed-specific parsers map fields to their corresponding models, ensuring names and structures that match the feed type exactly.

## Basic Usage

### Universal Feed Parser

Here's how to parse feeds using `gofeed.Parser`:

#### From a URL
```go
fp := gofeed.NewParser()
feed, _ := fp.ParseURL("http://feeds.twit.tv/twit.xml")
fmt.Println(feed.Title)
```

#### From a String

```go
feedData := `<rss version="2.0">
<channel>
<title>Sample Feed</title>
</channel>
</rss>`
fp := gofeed.NewParser()
feed, _ := fp.ParseString(feedData)
fmt.Println(feed.Title)
```

#### From an io.Reader

```go
file, _ := os.Open("/path/to/a/file.xml")
defer file.Close()
fp := gofeed.NewParser()
feed, _ := fp.Parse(file)
fmt.Println(feed.Title)
```

#### From a URL with a 60s Timeout

```go
ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
defer cancel()
fp := gofeed.NewParser()
feed, _ := fp.ParseURLWithContext("http://feeds.twit.tv/twit.xml", ctx)
fmt.Println(feed.Title)
```

#### From a URL with a Custom User-Agent

```go
fp := gofeed.NewParser()
fp.UserAgent = "MyCustomAgent 1.0"
feed, _ := fp.ParseURL("http://feeds.twit.tv/twit.xml")
fmt.Println(feed.Title)
```

### Feed Specific Parsers

If you have a usage scenario that requires a specialized parser:

#### RSS Feed

```go
feedData := `<rss version="2.0">
<channel>
<webMaster>example@site.com (Example Name)</webMaster>
</channel>
</rss>`
fp := rss.Parser{}
rssFeed, _ := fp.Parse(strings.NewReader(feedData))
fmt.Println(rssFeed.WebMaster)
```

#### Atom Feed

```go
feedData := `<feed xmlns="http://www.w3.org/2005/Atom">
<subtitle>Example Atom</subtitle>
</feed>`
fp := atom.Parser{}
atomFeed, _ := fp.Parse(strings.NewReader(feedData))
fmt.Println(atomFeed.Subtitle)
```

#### JSON Feed

```go
feedData := `{"version":"1.0", "home_page_url": "https://daringfireball.net"}`
fp := json.Parser{}
jsonFeed, _ := fp.Parse(strings.NewReader(feedData))
fmt.Println(jsonFeed.HomePageURL)
```

## Walking Paginated Feeds

Subscriptions present entries newest-first, with the older archive split into
later pages. `Parse`, `ParseString` and `ParseURL` still fetch and parse a
single document, but every translated `Feed` now carries a `NextPage` field
with the address of the next page:

- JSON Feed: the top-level `next_url`.
- Atom: the `href` of the first feed-level `<link rel="next">`. A `rel="next"`
  on an individual entry is not used.
- RSS: empty, since RSS has no pagination declaration.

`NextPage` comes from the `Translator`, so a custom translator can set or
clear it and the paging logic always follows the translated value rather than
reaching into the source feed. Values may be relative; they are resolved
against the final URL of the response that returned them.

### Parser.WalkURL

`WalkURL` fetches a feed and follows its `NextPage` chain, emitting items one
at a time through a callback. Pages are requested lazily — the next page is
not fetched until the current page has been parsed, its items emitted, and
its body closed. It uses the same format detection, translators and HTTP
configuration as `ParseURLWithContext` (`UserAgent`, `Client`, `AuthConfig`,
`MaxByteSize`, `HTTPError`); no conditional caching or automatic retry is
applied.

```go
fp := gofeed.NewParser()

// Full crawl: every page and every item, until the chain ends.
report, err := fp.WalkURL(ctx, "https://example.com/feed.json", gofeed.WalkOptions{},
    func(item *gofeed.Item) error {
        fmt.Println(item.GUID, item.Title)
        return nil
    })
// report.Pages, report.Items describe what was delivered.
```

### Incremental updates

Pass `WalkOptions{Incremental: true, KnownIDs: ...}` to stop at content
already collected. The walk ends at the first item whose non-empty GUID is in
`KnownIDs`: that item and everything after it are not delivered, no further
page is requested, and its GUID is returned as `WalkReport.StoppedAt`.

```go
known := map[string]struct{}{
    "https://example.com/posts/42": {},
}
report, err := fp.WalkURL(ctx, "https://example.com/feed.json",
    gofeed.WalkOptions{Incremental: true, KnownIDs: known},
    func(item *gofeed.Item) error { return store(item) })
fmt.Println("stopped at:", report.StoppedAt)
```

GUIDs are opaque strings: they are neither trimmed nor converted to URLs.
Other rules worth knowing:

- A non-empty GUID repeated within the same walk is skipped as a duplicate,
  but it is not treated as the historical boundary. Items with empty GUIDs
  are always delivered.
- Full mode (`Incremental: false`) ignores `KnownIDs`. The map is never
  modified, and the per-walk duplicate and cycle state is discarded when the
  call returns — each call starts fresh.
- A callback error is returned unchanged, with the counts accumulated so far;
  `Items` counts only successful emits. Items already delivered belong to the
  caller and are never mutated by later pages.
- Relative next-page addresses resolve against the response's final
  `Request.URL` (after redirects), never the feed's `self` or
  `home_page_url`. Only `http`/`https` continuations are accepted.
- For cycle detection, page identity drops the URL fragment but keeps the
  query string and its parameter order. A chain returning to a page already
  requested fails with `gofeed.ErrPaginationCycle` without requesting it
  again; an alias that redirects back to a visited page is detected when the
  response arrives, and that page is not parsed or delivered. Empty pages do
  not stop the walk.
- `Pages` counts only pages whose body was parsed successfully and closed;
  a page that fails to fetch or parse increments nothing. Invalid start URLs,
  a nil context or a nil callback fail before any request is made.

## Advanced Usage

#### With Basic Authentication

```go
fp := gofeed.NewParser()
fp.AuthConfig = &gofeed.Auth{
  Username: "foo",
  Password: "bar",
}
```

#### Using Custom Translators for Advanced Parsing

If you need more control over how fields are parsed and prioritized, you can specify your own custom translator. Below is an example that shows how to create a custom translator to give the `/rss/channel/itunes:author` field higher precedence than the `/rss/channel/managingEditor` field in RSS feeds.

##### Step 1: Define Your Custom Translator

First, we'll create a new type that embeds the default RSS translator provided by the library. We'll override its Translate method to implement our custom logic.

```go
type MyCustomTranslator struct {
  defaultTranslator *gofeed.DefaultRSSTranslator
}

func NewMyCustomTranslator() *MyCustomTranslator {
  t := &MyCustomTranslator{}
  t.defaultTranslator = &gofeed.DefaultRSSTranslator{}
  return t
}

func (ct *MyCustomTranslator) Translate(feed interface{}) (*gofeed.Feed, error) {
  rss, found := feed.(*rss.Feed)
  if !found {
    return nil, fmt.Errorf("Feed did not match expected type of *rss.Feed")
  }

  f, err := ct.defaultTranslator.Translate(rss)
  if err != nil {
    return nil, err
  }

  // Custom logic to prioritize iTunes Author over Managing Editor
  if rss.ITunesExt != nil && rss.ITunesExt.Author != "" {
    f.Author = rss.ITunesExt.Author
  } else {
    f.Author = rss.ManagingEditor
  }
  
  return f, nil
}
```

##### Step 2: Use Your Custom Translator

Once your custom translator is defined, you can tell gofeed.Parser to use it instead of the default one.

```go
feedData := `<rss version="2.0">
<channel>
<managingEditor>Ender Wiggin</managingEditor>
<itunes:author>Valentine Wiggin</itunes:author>
</channel>
</rss>`

fp := gofeed.NewParser()
fp.RSSTranslator = NewMyCustomTranslator()
feed, _ := fp.ParseString(feedData)
fmt.Println(feed.Author) // Valentine Wiggin
```

## Dependencies

* [goxpp](https://github.com/mmcdole/goxpp) - XML pull parser
* [x/net](https://pkg.go.dev/golang.org/x/net) - charset conversion and HTML parsing
* [testify](https://github.com/stretchr/testify) - unit test enhancements

## License

This project is licensed under the [MIT License](https://raw.githubusercontent.com/mmcdole/gofeed/master/LICENSE)

## Credits

* [cristoper](https://github.com/cristoper) for his work on implementing xml:base relative URI handling.
* [Mark Pilgrim](https://en.wikipedia.org/wiki/Mark_Pilgrim) and [Kurt McKee](http://kurtmckee.org) for their work on the excellent [Universal Feed Parser](https://github.com/kurtmckee/feedparser) Python library. This library was the inspiration for the `gofeed` library.
* [Dan MacTough](http://blog.mact.me) for his work on [node-feedparser](https://github.com/danmactough/node-feedparser). It provided inspiration for the set of fields that should be covered in the hybrid `gofeed.Feed` model.
* [Matt Jibson](https://mattjibson.com/) for his date parsing function in the [goread](https://github.com/mjibson/goread) project.
* [Jim Teeuwen](https://github.com/jteeuwen) for his method of representing arbitrary feed extensions in the [go-pkg-rss](https://github.com/jteeuwen/go-pkg-rss) library.
* [Sudhanshu Raheja](https://revolt.ist) for supporting JSON Feed parser
