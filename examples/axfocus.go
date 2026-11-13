//go:build ignore
// +build ignore

// Example: locate a control by its accessible role and name (form/group/
// input) instead of a CSS class, then focus the real DOM node through
// DOM.focus with a backend node id.
package main

import (
	"fmt"

	"github.com/raff/godet"
)

func main() {
	remote, err := godet.Connect("localhost:9222", false)
	if err != nil {
		fmt.Println("cannot connect to Chrome:", err)
		return
	}
	defer remote.Close()

	// Accessibility must be enabled before FindAX/FocusAX so that the AX ids
	// stay consistent across the root and child requests of one collection.
	// The AX entry points never disable this domain themselves.
	if err := remote.DomainEvents("Accessibility", true); err != nil {
		fmt.Println("cannot enable Accessibility:", err)
		return
	}

	// Bound every collection explicitly. The budget counts the raw AX tree
	// (ignored nodes included, root at depth 1) and the root plus child
	// requests actually sent.
	limits := godet.AXLimits{MaxNodes: 5000, MaxRequests: 200, MaxDepth: 100}

	// Scope the walk to one document. An empty frameID means the current
	// target's main frame; pass a frame id to query that frame only. Other
	// frame/document boundaries are not crossed.
	const frameID = ""

	name := func(s string) *string { return &s }

	path := []godet.AXStep{
		{Role: "form", Name: name("Invoice")},
		{Role: "group", Name: name("Billing")},
		{Role: "textbox", Name: name("Amount")},
	}

	matches, err := remote.FindAX(frameID, path, limits)
	if err != nil {
		fmt.Println("FindAX failed:", err)
		return
	}
	fmt.Println("matching controls:", len(matches))
	for _, m := range matches {
		fmt.Printf("ax=%s role=%s name=%q backend=%d\n", m.AXNodeID, m.Role, m.Name, m.BackendNodeID)
	}

	// FocusAX reruns the full lookup and only sends DOM.focus when exactly one
	// node matches and it carries a positive backendDOMNodeId.
	if err := remote.FocusAX(frameID, path, limits); err != nil {
		fmt.Println("FocusAX failed:", err)
		return
	}
	fmt.Println("focused the Amount textbox")
}
