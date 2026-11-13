What
====

This is a robots.txt exclusion protocol implementation for Go language (golang).


Build
=====

To build and run tests run `go test` in source directory.


Contribute
==========

Warm welcome.

* If desired, add your name in README.rst, section Who.
* Run `script/test && script/clean && echo ok`
* You can ignore linter warnings, but everything else must pass.
* Send your change as pull request or just a regular patch to current maintainer (see section Who).

Thank you.


Usage
=====

As usual, no special installation is required, just

    import "github.com/temoto/robotstxt"

run `go get` and you're ready.

1. Parse
^^^^^^^^

First of all, you need to parse robots.txt data. You can do it with
functions `FromBytes(body []byte) (*RobotsData, error)` or same for `string`::

    robots, err := robotstxt.FromBytes([]byte("User-agent: *\nDisallow:"))
    robots, err := robotstxt.FromString("User-agent: *\nDisallow:")

All parse entry points share one incremental (streaming) scanner and parser.
The whole body is never buffered: working memory grows only with the longest
token, not with body size, comment length or the number of tokens read.
Large comment blocks and blank lines are discarded while reading.

For an `io.Reader` (for example an `http.Response.Body` you stream straight
from the network), use `FromReader(r io.Reader) (*RobotsData, error)`::

    robots, err := robotstxt.FromReader(resp.Body)

`FromReader` works even if the Reader hands over one byte at a time, splits
multi-byte UTF-8, the leading BOM, CRLF, the field colon or the last value
across reads, or returns data together with `io.EOF`. `FromReader` never
closes the Reader; closing it stays the caller's responsibility. Passing a
nil Reader returns the sentinel error `robotstxt.ErrNilReader` instead of
panicking.

Error handling for `FromReader` (and the 2xx path of `FromResponse`):

* A non-EOF failure of the Reader is returned unchanged, so `errors.Is` can
  recover the original error. The result data is always nil in that case:
  a truncated body is never accepted as a complete policy, even when the
  failing Read also delivered the last valid bytes.
* A read error takes precedence over any syntax (`ParseError`) error.
* A Reader that keeps returning zero bytes without an error eventually
  fails with `io.ErrNoProgress`; parsing cannot loop forever.

There are few convenient constructors for various purposes:

* `FromResponse(*http.Response) (*RobotsData, error)` to init robots data
from HTTP response. Successful (2xx) responses are parsed incrementally
through `FromReader`. For 4xx/5xx and other unsupported statuses the body
is still read to the end (or until a read error) but neither parsed nor
fully buffered before the status is interpreted. It *does not* call
`response.Body.Close()`::

    robots, err := robotstxt.FromResponse(resp)
    resp.Body.Close()
    if err != nil {
        log.Println("Error parsing robots.txt:", err.Error())
    }

* `FromStatusAndBytes(statusCode int, body []byte) (*RobotsData, error)` or
`FromStatusAndString` if you prefer to read bytes (string) yourself.
Passing status code applies following logic in line with Google's interpretation
of robots.txt files:

    * status 2xx  -> parse body with `FromBytes` and apply rules listed there.
    * status 4xx  -> allow all (even 401/403, as recommended by Google).
    * other (5xx) -> disallow all, consider this a temporary unavailability.

2. Query
^^^^^^^^

Parsing robots.txt content builds a kind of logic database, which you can
query with `(r *RobotsData) TestAgent(url, agent string) (bool)`.

Explicit passing of agent is useful if you want to query for different agents. For
single agent users there is an efficient option: `RobotsData.FindGroup(userAgent string)`
returns a structure with `.Test(path string)` method and `.CrawlDelay time.Duration`.

Simple query with explicit user agent. Each call will scan all rules.

::

    allow := robots.TestAgent("/", "FooBot")

Or query several paths against same user agent for performance.

::

    group := robots.FindGroup("BarBot")
    group.Test("/")
    group.Test("/download.mp3")
    group.Test("/news/article-2012-1")


Who
===

Honorable contributors (in undefined order):

    * Ilya Grigorik (igrigorik)
    * Martin Angers (PuerkitoBio)
    * Micha Gorelick (mynameisfiber)

Initial commit and other: Sergey Shepelev temotor@gmail.com


Flair
=====

.. image:: https://travis-ci.org/temoto/robotstxt.svg?branch=master
    :target: https://travis-ci.org/temoto/robotstxt

.. image:: https://codecov.io/gh/temoto/robotstxt/branch/master/graph/badge.svg
    :target: https://codecov.io/gh/temoto/robotstxt

.. image:: https://goreportcard.com/badge/github.com/temoto/robotstxt
    :target: https://goreportcard.com/report/github.com/temoto/robotstxt
