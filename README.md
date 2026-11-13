
[![Go Documentation](http://godoc.org/github.com/raff/godet?status.svg)](http://godoc.org/github.com/raff/godet)
[![Go Report Card](https://goreportcard.com/badge/github.com/raff/godet)](https://goreportcard.com/report/github.com/raff/godet)
[![Actions Status](https://github.com/raff/godet/workflows/Go/badge.svg)](https://github.com/raff/godet/actions)


# godet
Remote client for Chrome DevTools

## Installation

    $ go get github.com/raff/godet
    
## Documentation
http://godoc.org/github.com/raff/godet

## Example
A pretty complete example is available at [`cmd/godet/main.go`](https://github.com/raff/godet/blob/master/cmd/godet/main.go).
This example is available at [`examples/example.go`](https://github.com/raff/godet/blob/master/examples/example.go).

```go
import "github.com/raff/godet"

// connect to Chrome instance
remote, err := godet.Connect("localhost:9222", true)
if err != nil {
    fmt.Println("cannot connect to Chrome instance:", err)
    return
}

// disconnect when done
defer remote.Close()

// get browser and protocol version
version, _ := remote.Version()
fmt.Println(version)

// get list of open tabs
tabs, _ := remote.TabList("")
fmt.Println(tabs)

// install some callbacks
remote.CallbackEvent(godet.EventClosed, func(params godet.Params) {
    fmt.Println("RemoteDebugger connection terminated.")
})

remote.CallbackEvent("Network.requestWillBeSent", func(params godet.Params) {
    fmt.Println("requestWillBeSent",
        params["type"],
        params["documentURL"],
        params["request"].(map[string]interface{})["url"])
})

remote.CallbackEvent("Network.responseReceived", func(params godet.Params) {
    fmt.Println("responseReceived",
        params["type"],
        params["response"].(map[string]interface{})["url"])
})

remote.CallbackEvent("Log.entryAdded", func(params godet.Params) {
    entry := params["entry"].(map[string]interface{})
    fmt.Println("LOG", entry["type"], entry["level"], entry["text"])
})

// block loading of most images
_ = remote.SetBlockedURLs("*.jpg", "*.png", "*.gif")

// create new tab
tab, _ := remote.NewTab("https://www.google.com")
fmt.Println(tab)

// enable event processing
remote.RuntimeEvents(true)
remote.NetworkEvents(true)
remote.PageEvents(true)
remote.DOMEvents(true)
remote.LogEvents(true)

// navigate in existing tab
_ = remote.ActivateTab(tabs[0])

// re-enable events when changing active tab
remote.AllEvents(true) // enable all events

_, _ = remote.Navigate("https://www.google.com")

// evaluate Javascript expression in existing context
res, _ := remote.EvaluateWrap(`
    console.log("hello from godet!")
    return 42;
`)
fmt.Println(res)

// take a screenshot
_ = remote.SaveScreenshot("screenshot.png", 0644, 0, true)

// or save page as PDF
_ = remote.SavePDF("page.pdf", 0644)

```

## Finding controls by accessible role and name

When a backend control has no stable CSS class, it can still be located through
the browser's accessibility tree. `FindAX` walks the **exposed** accessibility
tree and matches a path of `(role, accessible name)` steps against a control and
its continuous exposed ancestors; `FocusAX` focuses the real DOM node with a
single `DOM.focus` call using the node's `backendNodeId`. No HTML is parsed, no
accessible name is recomputed and no JavaScript is evaluated to find the
control.

Enable the `Accessibility` domain first so that AX ids stay consistent across
the requests of one collection. The AX methods never disable that domain:

```go
if err := remote.DomainEvents("Accessibility", true); err != nil {
    log.Fatal(err)
}
```

A step matches `role.value` exactly. `Name == nil` matches any name; a non-nil
`Name` (including a pointer to `""`) matches `name.value` exactly, with no
trimming, case folding or Unicode normalization. A missing name is treated as
`""`. Set an explicit budget on every call; it counts the raw AX tree (ignored
nodes included, the root at depth 1) and the `getRootAXNode` plus
`getChildAXNodes` requests actually sent:

```go
name := func(s string) *string { return &s }

limits := godet.AXLimits{MaxNodes: 5000, MaxRequests: 200, MaxDepth: 100}

// Empty frameID means the current target's main frame; pass a frame id to
// scope the walk to that document. Other frame/document boundaries are not
// entered and OOPIF cross-session trees are not stitched together.
const frameID = ""

path := []godet.AXStep{
    {Role: "form", Name: name("Invoice")},
    {Role: "group", Name: name("Billing")},
    {Role: "textbox", Name: name("Amount")},
}

matches, err := remote.FindAX(frameID, path, limits)
// matches is a non-nil empty slice when nothing matches.

// FocusAX reruns the whole lookup and sends DOM.focus(backendNodeId) only
// when exactly one node matches and it has a positive backendDOMNodeId.
err = remote.FocusAX(frameID, path, limits)
```

`ignored` nodes never match themselves, but their children are lifted in
original order to the nearest non-ignored ancestor, so a path cannot skip an
exposed group: `form(Invoice) → group(Billing) → textbox(Amount)` matches,
while `form(Invoice) → textbox(Amount)` does not reach a textbox that still has
the exposed `group(Billing)` between it and the form. Results are returned in
exposed-tree depth-first preorder and distinct AX nodes are never merged by
name. The original `Focus(nodeID)` (front-end DOM id) is unchanged.
