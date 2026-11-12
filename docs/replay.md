# Page state, replay and resource ownership

Every successful navigation publishes one complete, internally consistent page
state. All of the following describe the same request/response pair:

* the decoded response body served by `Download` (gzip/deflate are inflated),
* the parsed DOM exposed by `Title`, `Body`, `Dom`, `Find`, `Links` and
  `Forms`,
* the final document URL after redirects,
* the status code and response headers,
* the relative URL base used to resolve links and form actions,
* the original navigation request kept for `Reload`.

A failed navigation never publishes half a page. If preparing the request
body, creating a spill file, sending the request, reading the response or
decoding it fails, the current page, its `Download` bytes, its base and the
history stay untouched, and any resource allocated for the failed attempt is
released. Preparation failures do not send a request at all; failures after
the request was sent are not retried automatically and the returned error
wraps the original cause.

## Back

`Back` restores only completed pages from the local history; it never
contacts the origin server. A browser that has loaded a single page can go
back once, and the empty state of a freshly created browser is never exposed.
Going back discards the current page. After restoring a page, `Download`,
`Links` and relative form actions all resolve against that page.

The base URL is computed per page from its final document URL and its
`<base>` tag. A page without a `<base>` tag uses its own document URL and
never inherits the base of the previously visited page. A relative `<base>`
is resolved against the page's final document URL regardless of whether
`MetaRefreshHandling` is enabled.

## Reload and its side effects

`Reload` re-sends the request that produced the current page, preserving the
original method, entry URL, `Content-Type`, applied request headers and
request body. Redirects are handled by `net/http` again: for example a POST
answered with a 303 See Other is replayed as that same POST, not as the final
GET of the receipt page.

Reload has the same side effects as the original navigation:

* non-idempotent requests such as POST are executed again on the server,
* cookies are recomputed by the cookie jar for the new request URL. Cookies
  appended to a previous send are never stored as permanent request headers,
* each replay reads the stored request body through a brand new stream, so
  repeated reloads and reloads after `Back` work even when the original body
  was a one-shot `io.Reader`.

Reload replaces the current record in place and does not add another history
entry.

## Replay storage

Request bodies are fully prepared and stored before the first request is
sent, which is what makes replay possible without seeking the original
reader, re-reading an attachment source or regenerating a multipart boundary.

```go
// Bodies up to 2 MiB stay in memory; larger bodies spill into temp files.
if err := bow.SetReplayStorage(2<<20, ""); err != nil {
    panic(err)
}
```

* `memoryLimit` is measured on the complete encoded HTTP body and applies to
  `Post`, `PostForm`, `PostMultipart` and `Form.Submit`.
* Bodies at or below the limit stay in memory; larger bodies migrate to a
  temporary file created in `dir`. The default limit is 1 MiB.
* A limit of zero spills every body straight to disk. A negative limit
  returns an error and leaves the previous settings unchanged.
* An empty `dir` uses the operating system's temporary directory.
* Multipart bodies are produced as a stream, so a large attachment does not
  require holding the whole encoded body in memory; memory use does not grow
  with the attachment length.

Stream ownership is unchanged: an `io.ReadCloser` passed to `Post` is closed
exactly once by the library, attachment streams passed through
`Form.File`/`SetFile` remain managed by the caller, and streams and storage
handles opened by the library are closed by the library.

## Close, history and ownership

`Browser.Close` releases the resources owned by the browser, including the
replay temporary files held by the current page and the history. It is safe
to call repeatedly. After `Close`, `Open`, `Head`, `Post`, `PostForm`,
`PostMultipart` and `Reload` return an error and `Back` returns `false`.

Replay resources live as long as at least one page state references them.
With the default `jar.MemoryHistory` they are released when:

* a state is evicted by the history limit (`Push` trimming or `SetMax`
  shrinking; `SetMax(0)` keeps the infinite-history meaning),
* the history is cleared with `Clear`,
* the current page is discarded by `Back` or replaced by `Reload`,
* or the last owning browser calls `Close`.

`NewTab` shares the configuration, state, cookie jar, bookmarks and history.
The current page and every history entry stay valid for every tab: closing
one tab releases only its ownership, and backing temporary files are removed
when the last owning tab releases them. A tab closed this way does not
invalidate pages another tab is currently using.

The `jar.History` interface itself is unchanged. A custom implementation
owns the states it receives through `Push`: a state removed by `Pop`,
eviction, `Clear` or any other path must have `State.ReleaseResources`
called once no other reference remains. `Pop` transfers ownership, including
the replay resources, to the caller. Custom histories that cannot honor this
contract should not store states whose `Replay` field is non-nil.
