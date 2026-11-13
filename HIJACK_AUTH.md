# HijackRouter authentication

`HijackRouter` handles the two Fetch domain events, `Fetch.requestPaused` and
`Fetch.authRequired`, through one dispatch chain and one `Fetch.enable`
configuration. URL/resource-type routes and HTTP authentication therefore never
overwrite each other.

## Public API

```go
func (r *HijackRouter) SetAuthHandler(
    handler func(*proto.FetchAuthRequired) *proto.FetchAuthChallengeResponse,
    onError func(error),
) error
```

- `handler != nil` enables `Fetch.authRequired` events
  (`handleAuthRequests: true`); `handler == nil` removes the authentication
  feature.
- `onError == nil` is replaced with a no-op callback. It receives the original
  error when `Fetch.continueWithAuth` cannot be sent, exactly once per
  challenge.
- Like `Add` and `Remove`, it is configured before `Run`. Hot reloading while
  the router is running is not supported.
- The merged `Fetch.enable` always carries the current URL/resource-type
  patterns together with the auth switch. `Add`, `Remove` and `SetAuthHandler`
  can be combined in any order; changing one item keeps all the others.
- If `Fetch.enable` fails, the original error is returned and the confirmed
  local configuration is unchanged. The next operation continues from the last
  successful state.

## Dispatch rules

### `Fetch.requestPaused` (normal routes only)

- Handlers are matched by URL pattern **and** the event's actual
  `resourceType`, in `Add` order. An empty resource type matches every type.
  Chrome may pause other resource types of the same URL for another pattern,
  so the type is always checked in Rod.
- A handler is only skipped when its `Hijack.Skip` is `true`; the flag left by
  the previous candidate is cleared before each handler call.
- `ContinueRequest`, `Response.Fail` or a normal `Fulfill` is a terminal
  action: later handlers are not called and no second terminal command is
  sent. If the terminal command fails, the original error is reported once via
  that `Hijack.OnError`.
- With no matching handler, or when every candidate skips, Rod sends exactly
  one unmodified `Fetch.continueRequest`.

### `Fetch.authRequired` (auth handler only)

- Normal request handlers are never called and no `Fetch.continueRequest` is
  sent.
- The returned `*proto.FetchAuthChallengeResponse` drives
  `Fetch.continueWithAuth`; `Response`, `Username` and `Password` are
  preserved. A `nil` return answers `CancelAuth`, which suppresses the
  interactive login dialog. The object returned by the handler is copied and
  never written back.
- A send failure is reported once through `onError`. Rod does not retry, does
  not fall back to `Default` credentials and does not continue the event as a
  normal request.
- The same `requestId` may produce several challenges. Every challenge that
  arrives is passed to the handler independently; a previous answer is never
  treated as handling a later challenge.
- Normal and auth callbacks may block; other events already received keep
  being dispatched.

## Sessions

- `Page.HijackRequests` only receives the two events of that page session.
- `Browser.HijackRequests` receives events from every session.
- Every continue/fail/fulfill/auth answer is sent back to the session that
  emitted the event, including the empty (browser) session. Ownership is taken
  from the event, never inferred from the request id, and two sessions may use
  the same request id.
- Events are consumed through the existing Browser/Page broadcast
  subscriptions, so other subscribers keep receiving them.

The standalone `Browser.HandleAuth` helper is unchanged and still works on its
own. The router does not call it and does not read raw events from
`CDPClient.Event` directly.

## Example

```go
router := browser.HijackRequests()
defer router.MustStop()

router.MustAdd("*.js", func(ctx *rod.Hijack) {
    ctx.ContinueRequest(&proto.FetchContinueRequest{})
})

err := router.SetAuthHandler(func(e *proto.FetchAuthRequired) *proto.FetchAuthChallengeResponse {
    return &proto.FetchAuthChallengeResponse{
        Response: proto.FetchAuthChallengeResponseResponseProvideCredentials,
        Username: "user",
        Password: "pass",
    }
}, nil)
if err != nil {
    panic(err)
}

go router.Run()
```
