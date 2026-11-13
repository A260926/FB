go-retryablehttp
================

[![Build Status](http://img.shields.io/travis/hashicorp/go-retryablehttp.svg?style=flat-square)][travis]
[![Go Documentation](http://img.shields.io/badge/go-documentation-blue.svg?style=flat-square)][godocs]

[travis]: http://travis-ci.org/hashicorp/go-retryablehttp
[godocs]: http://godoc.org/github.com/hashicorp/go-retryablehttp

The `retryablehttp` package provides a familiar HTTP client interface with
automatic retries and exponential backoff. It is a thin wrapper over the
standard `net/http` client library and exposes nearly the same public API. This
makes `retryablehttp` very easy to drop into existing programs.

`retryablehttp` performs automatic retries under certain conditions. Mainly, if
an error is returned by the client (connection errors, etc.), or if a 500-range
response code is received (except 501), then a retry is invoked after a wait
period.  Otherwise, the response is returned and left to the caller to
interpret.

The main difference from `net/http` is that requests which take a request body
(POST/PUT et. al) can have the body provided in a number of ways (some more or
less efficient) that allow "rewinding" the request body if the initial request
fails so that the full request can be attempted again. See the
[godoc](http://godoc.org/github.com/hashicorp/go-retryablehttp) for more
details.

Version 0.6.0 and before are compatible with Go prior to 1.12. From 0.6.1 onward, Go 1.12+ is required.
From 0.6.7 onward, Go 1.13+ is required.

Example Use
===========

Using this library should look almost identical to what you would do with
`net/http`. The most simple example of a GET request is shown below:

```go
resp, err := retryablehttp.Get("/foo")
if err != nil {
    panic(err)
}
```

The returned response object is an `*http.Response`, the same thing you would
usually get from `net/http`. Had the request failed one or more times, the above
call would block and retry with exponential backoff.

## Getting a stdlib `*http.Client` with retries

It's possible to convert a `*retryablehttp.Client` directly to a `*http.Client`.
This makes use of retryablehttp broadly applicable with minimal effort. Simply
configure a `*retryablehttp.Client` as you wish, and then call `StandardClient()`:

```go
retryClient := retryablehttp.NewClient()
retryClient.RetryMax = 10

standardClient := retryClient.StandardClient() // *http.Client
```

## Coordinating per-origin pauses with an OriginGate

A normal retry only delays the request that received the failure. When several
goroutines or several clients collect from the same site, a `Retry-After`
answer to one of them does not stop the others from hitting the site while it
is asking the client to back off. An `OriginGate` shares that pause across
every request that uses it.

Create one gate and assign it to every client that should coordinate. A gate
is safe for concurrent use and may be shared by multiple clients; clients
without a gate keep their existing behavior exactly.

```go
// A single Retry-After hint may pause an origin for at most one minute.
// A nil clock selects the real wall clock; tests can pass a controllable one.
gate, err := retryablehttp.NewOriginGate(time.Minute, nil)
if err != nil {
    // maxDelay must be greater than zero
    log.Fatal(err)
}

client := retryablehttp.NewClient()
client.OriginGate = gate
```

How it works:

- Before every underlying `RoundTrip` — the first attempt, every retry, and
  every redirect hop — the client waits until the gate's deadline for the
  request origin has passed. Requests already in flight are not interrupted.
- A `429 Too Many Requests` or `503 Service Unavailable` response feeds each
  of its `Retry-After` header field values to the gate. Unsigned decimal
  seconds and HTTP dates (`http.ParseTime`) are both accepted; zero, past and
  invalid values are ignored, and one hint never contributes more than
  `maxDelay`. The latest valid hint wins, and hints can only ever move the
  deadline later. Responses with any other status never touch the gate, even
  if they carry the same header.
- Origins are keyed by scheme, lower-cased hostname and effective port.
  `http://example.com` and `http://example.com:80` share a pause, as do the
  two `https`/`443` forms; different schemes never merge, and userinfo, path
  and query are not part of the key.
- Hints are recorded independently of retry accounting. With `RetryMax = 0`,
  a `Retry-After` on the one-and-only attempt still pauses later requests,
  while the decision whether to retry stays entirely with `CheckRetry`. The
  configured `Backoff` still runs first on retries; the gate then waits only
  for whatever pause remains, so the `Retry-After` time is never counted
  twice. Waiting adds no retry attempts and does not invoke the request hooks
  again.
- The supplied `*http.Client`, its `Transport`, `CheckRedirect`, cookie jar
  and `Timeout` are never modified, so redirect and cookie handling are
  unchanged. A client produced by `StandardClient()` goes through the same
  gate.
- Pauses are stored on the gate itself, not in package-global state. Two gates
  never interact, so unrelated clients can be kept fully independent.

For more usage and examples see the
[pkg.go.dev](https://pkg.go.dev/github.com/hashicorp/go-retryablehttp).
