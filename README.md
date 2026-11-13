Agouti
======

[![Build Status](https://api.travis-ci.org/sclevine/agouti.png?branch=master)](http://travis-ci.org/sclevine/agouti)
[![GoDoc](https://godoc.org/github.com/sclevine/agouti?status.svg)](https://godoc.org/github.com/sclevine/agouti)

**NOTE: Agouti is no longer actively maintained. This was one of my earliest Go projects, and I am no longer happy with the API design. I recommend selecting an alternative WebDriver client for Go.**

Agouti is a library for writing browser-based acceptance tests in Google Go. It provides [Gomega](https://github.com/onsi/gomega) matchers and plays nicely with [Ginkgo](https://github.com/onsi/ginkgo) or [Spec](https://github.com/sclevine/spec). See [agouti.org](http://agouti.org) and the [GoDoc](https://godoc.org/github.com/sclevine/agouti) for documentation. Have questions? Check out the [Agouti mailing list](https://groups.google.com/d/forum/agouti) or the #agouti IRC channel on Freenode.

The [integration tests](https://github.com/sclevine/agouti/blob/master/internal/integration/) are a great place to see everything in action and get started quickly!

Relational queries
------------------

Linear selector chains cannot express conditions that relate one candidate
element to others. A `Query` adds an immutable relational expression on top of
the existing selection and element-repository chains:

```go
// In every order section, keep rows that contain a pending button and do not
// contain an error, then take the second such row in each section.
rows := page.All("section.order").Query(
    agouti.QueryCSS("tr").
        Has(agouti.QueryCSS("button.pending")).
        HasNot(agouti.QueryCSS(".error")).
        At(1),
)
```

`Query` expressions are evaluated through WebDriver's element-scoped
find-elements commands. They never execute whole-page JavaScript and never
retrieve page HTML to build a second DOM in the client.

### Operations

- `QueryCSS(selector)` finds descendants of the current context, in the order
  returned by the WebDriver server.
- `Then(right)` applies `right` to every left result and concatenates the
  results in left-element order. Results are not de-duplicated across parent
  contexts.
- `Has(right)` keeps left elements for which `right` finds at least one result.
- `HasNot(right)` keeps left elements for which `right` finds no result at all.
  A remote error is never treated as absence: stale-element and invalid-selector
  errors are returned to the caller.
- `Or(right)` evaluates both sides against exactly the same input context. Left
  results keep their order; right results that have not already appeared are
  appended. De-duplication uses the remote element ID within that input context
  only.
- `At(index)` selects one zero-based result from the complete expression that
  immediately precedes it. Out-of-range indices fail at evaluation time and
  negative indices are rejected as invalid expressions. `At` is applied before
  the next `All`/`Find` in the chain, so it never borrows candidates from
  another parent context.

The right side of `Has`, `HasNot`, and `Then` is rooted at the element being
tested. It is never re-rooted at the page. Conditions may be nested freely:
`Has` can contain `Then`, `Or`, and further `Has`/`HasNot` expressions.

All construction, copying, combination, and `String` calls are local. Building a
`Query` never contacts the server, and later construction never mutates an
existing expression.

### Precedence example

```go
// Prefer the primary control, fall back to the secondary one, then take the
// first surviving candidate in the current parent context.
control := page.Query(
    agouti.QueryCSS("input.primary").
        Or(agouti.QueryCSS("input.fallback")).
        At(0),
)
```

The `Or` is evaluated first, both sides against the same parent context, and
`At(0)` applies to the merged, de-duplicated result. This is deliberately not
the same as `QueryCSS("input.primary").At(0).Or(QueryCSS("input.fallback").At(0))`.

### Evaluation, errors, and actions

- An empty CSS selector, a zero-value `Query`, a negative `At`, or an invalid
  nested sub-expression is rejected before any remote request is sent for the
  expression. No partial query is executed.
- Every terminal operation re-evaluates the query; remote element IDs are not
  cached in a `Query` or `Selection`.
- Batch actions such as `Fill` resolve the complete query first. If any parent
  branch fails, no element has received an action. On success, the same result
  snapshot is used for all actions (for example, one resolve per `Fill`, not a
  separate lookup for `Clear` and `Value`). A remote failure partway through the
  actions is returned by the old interface without promising to undo actions
  that already ran.
- Returned selections support `Elements`, `Count`, text/property reads,
  `Click`, `Fill`, and further `Find`/`All`/`At` calls. `Elements` returns the
  concrete `*api.Element` handles for that evaluation.
- `GetExactlyOne`/`GetAtLeastOne` quantity rules are unchanged.
- Plain CSS chains still merge into a single descendant-selector request. A
  `Query` is a hard barrier: CSS around `Has`, `HasNot`, `Or`, or `At` is never
  merged across it, and `All`/`Find` after a `Query` is rooted at the query's
  results rather than slipped into its filter.
- Reusing the same `Query` on two `Page` values keeps each evaluation on its own
  session, including when the pages are used interleaved.
