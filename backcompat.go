// Copyright (c) The microformats project authors.
// SPDX-License-Identifier: MIT

// This file includes backwards compatibility support for microformats v1.

package microformats

import (
	"net/url"
	"path"
	"slices"
	"strings"

	"golang.org/x/net/html"
	"golang.org/x/net/html/atom"
)

var (
	// map microformats v1 root classes to their v2 equivalent.
	backcompatRootMap = map[string]string{
		"adr":               "h-adr",
		"geo":               "h-geo",
		"hentry":            "h-entry",
		"hfeed":             "h-feed",
		"hnews":             "h-news",
		"hproduct":          "h-product",
		"hrecipe":           "h-recipe",
		"hresume":           "h-resume",
		"hreview":           "h-review",
		"hreview-aggregate": "h-review-aggregate",
		"vcard":             "h-card",
		"vevent":            "h-event",
	}

	// map microformats v1 property classes to their v2 equivalent. These
	// mappings are root-specific.
	backcompatPropertyMap = map[string]map[string]string{
		"h-adr": {
			"country-name":     "p-country-name",
			"extended-address": "p-extended-address",
			"locality":         "p-locality",
			"post-office-box":  "p-post-office-box",
			"postal-code":      "p-postal-code",
			"region":           "p-region",
			"street-address":   "p-street-address",
		},
		"h-card": {
			"additional-name":  "p-additional-name",
			"adr":              "p-adr",
			"agent":            "p-agent",
			"bday":             "dt-bday",
			"category":         "p-category",
			"class":            "p-class",
			"email":            "u-email",
			"family-name":      "p-family-name",
			"fn":               "p-name",
			"geo":              "p-geo",
			"given-name":       "p-given-name",
			"honorific-prefix": "p-honorific-prefix",
			"honorific-suffix": "p-honorific-suffix",
			"key":              "u-key",
			"label":            "p-label",
			"logo":             "u-logo",
			"mailer":           "p-mailer",
			"nickname":         "p-nickname",
			"note":             "p-note",
			"org":              "p-org",
			"photo":            "u-photo",
			"rev":              "dt-rev",
			"role":             "p-role",
			"sort-string":      "p-sort-string",
			"sound":            "u-sound",
			"tel":              "p-tel",
			"title":            "p-job-title",
			"tz":               "p-tz",
			"uid":              "u-uid",
			"url":              "u-url",
		},
		"h-entry": {
			"author":        "p-author",
			"category":      "p-category",
			"entry-content": "e-content",
			"entry-summary": "p-summary",
			"entry-title":   "p-name",
			"published":     "dt-published",
			"summary":       "p-summary",
			"updated":       "dt-updated",
		},
		"h-event": {
			"attendee":    "p-attendee",
			"category":    "p-category",
			"description": "p-description",
			"dtend":       "dt-end",
			"dtstart":     "dt-start",
			"duration":    "dt-duration",
			"geo":         "p-geo",
			"location":    "p-location",
			"summary":     "p-name",
			"url":         "u-url",
		},
		"h-feed": {
			"author": "p-author",
			"entry":  "p-entry",
			"photo":  "u-photo",
			"url":    "u-url",
		},
		"h-geo": {
			"latitude":  "p-latitude",
			"longitude": "p-longitude",
		},
		"h-item": {
			"fn":    "p-name",
			"photo": "u-photo",
			"url":   "u-url",
		},
		"h-news": {
			"dateline":   "p-dateline",
			"entry":      "p-entry",
			"geo":        "p-geo",
			"source-org": "p-source-org",
		},
		"h-product": {
			"brand":       "p-brand",
			"category":    "p-category",
			"description": "p-description",
			"fn":          "p-name",
			"identifier":  "u-identifier",
			"listing":     "p-listing",
			"photo":       "u-photo",
			"price":       "p-price",
			"review":      "p-review",
			"url":         "u-url",
		},
		"h-recipe": {
			"author":       "p-author",
			"category":     "p-category",
			"duration":     "dt-duration",
			"fn":           "p-name",
			"ingredient":   "p-ingredient",
			"instructions": "e-instructions",
			"nutrition":    "p-nutrition",
			"photo":        "u-photo",
			"summary":      "p-summary",
			"yield":        "p-yield",
		},
		"h-resume": {
			"affiliation":  "p-affiliation",
			"contact":      "p-contact",
			"education":    "p-education",
			"experience":   "p-experience",
			"publications": "p-publications",
			"skill":        "p-skill",
			"summary":      "p-summary",
		},
		"h-review": {
			"best":        "p-best",
			"description": "e-content",
			"dtreviewed":  "dt-reviewed",
			"item":        "p-item",
			"rating":      "p-rating",
			"reviewer":    "p-author",
			"summary":     "p-name",
			"worst":       "p-worst",
		},
		"h-review-aggregate": {
			"average": "p-average",
			"best":    "p-best",
			"count":   "p-count",
			"item":    "p-item",
			"rating":  "p-rating",
			"summary": "p-name",
			"votes":   "p-votes",
			"worst":   "p-worst",
		},
	}

	// map microformats v1 rel values to their v2 property equivalent. These
	// mappings are root-specific.
	backcompatRelMap = map[string]map[string]string{
		"h-card": {
			"tag": "u-category",
		},
		"h-entry": {
			"bookmark": "u-url",
			"tag":      "u-category",
		},
		"h-feed": {
			"tag": "u-category",
		},
		"h-news": {
			"principles": "u-principles",
		},
		"h-recipe": {
			"tag": "u-category",
		},
		"h-review": {
			"bookmark": "u-url",
			"tag":      "u-category",
		},
	}
)

// backcompatRootClasses returns the v2 root classes for the backcompat v1
// roots in the provided classes. parent identifies the parent microformat, if
// present, since some root mappings are context-specific.
func backcompatRootClasses(classes []string, parent *Microformat) []string {
	var rootclasses []string
	var itemClass bool
	for _, class := range classes {
		if c, ok := backcompatRootMap[class]; ok {
			rootclasses = append(rootclasses, c)
		}
		if class == "item" {
			itemClass = true
		}
	}

	// handle implied h-item microformat inside of h-review
	if len(rootclasses) == 0 && parent != nil && itemClass {
		for _, t := range parent.Type {
			if t == "h-review" || t == "h-review-aggregate" {
				rootclasses = append(rootclasses, "h-item")
			}
		}
	}

	return rootclasses
}

// backcompatPropertyClasses returns the v2 property classes for the backcompat
// v1 properties in the provided classes and rel values.  context identifies
// the v2 microformat types (h-card, h-adr, etc) the parsed property belongs to.
func backcompatPropertyClasses(classes []string, rels []string, context []string) []string {
	var classmap = make(map[string]string)
	for _, class := range classes {
		for _, ctx := range context {
			if c, ok := backcompatPropertyMap[ctx][class]; ok {
				parts := strings.SplitN(c, "-", 2)
				classmap[parts[1]] = c
			}
		}
	}
	for _, rel := range rels {
		for _, ctx := range context {
			if c, ok := backcompatRelMap[ctx][rel]; ok {
				parts := strings.SplitN(c, "-", 2)
				classmap[parts[1]] = c
			}
		}
	}

	var propertyclasses []string
	for _, c := range classmap {
		propertyclasses = append(propertyclasses, c)
	}
	return propertyclasses
}

// strip provided URL to its last path segment to serve as a category value.
func backcompatURLCategory(s string) string {
	if s == "" {
		return s
	}
	if p, err := url.Parse(s); err == nil {
		return path.Base(p.Path)
	}
	return s
}

// backcompatIncludeRefs returns references found using the include pattern.
//
// refs includes the IDs of referenced nodes (without any leading '#')
//
// replace is true if the referenced node was identified using a pattern  which
// instructs the referencing node to be replaced entirely, rather than the
// referenced node being amended.
//
// See http://microformats.org/wiki/include-pattern
// See http://microformats.org/wiki/microdata
func (p *parser) backcompatIncludeRefs(node *html.Node) (refs []string, replace bool) {
	classes := getClasses(node)
	for _, class := range classes {
		if class == "include" {
			var id string
			switch node.DataAtom {
			case atom.A:
				id = getAttr(node, "href")
			case atom.Object:
				id = getAttr(node, "data")
			}

			if !strings.HasPrefix(id, "#") {
				// skip links not within the current page
				continue
			}

			id = strings.TrimPrefix(id, "#")
			if id == "" {
				continue
			}

			return append(refs, id), true
		}
	}

	if node.DataAtom == atom.Td {
		refs = append(refs, strings.Fields(getAttr(node, "headers"))...)
	}
	refs = append(refs, strings.Fields(getAttr(node, "itemref"))...)

	return refs, false
}

// includeContext tracks the state needed while expanding backcompat include
// references into a private copy of the document being parsed.
type includeContext struct {
	// roots is the stack of microformat roots enclosing the node currently
	// being expanded, innermost last.
	roots []includeRoot

	// path holds the original document nodes on the active expansion path:
	// every ancestor of the current position, whether reached by normal
	// descent or by following a reference.  It is consulted to skip
	// reference back edges.
	path map[*html.Node]bool
}

// includeRoot describes a microformat root enclosing the position currently
// being expanded.
type includeRoot struct {
	types      []string
	backcompat bool
}

func newIncludeContext() *includeContext {
	return &includeContext{path: make(map[*html.Node]bool)}
}

// parent returns a microformat holding the types of the innermost root
// enclosing the current position, or nil if there is none.
func (c *includeContext) parent() *Microformat {
	if len(c.roots) == 0 {
		return nil
	}
	return &Microformat{Type: c.roots[len(c.roots)-1].types}
}

// backcompatActive reports whether the innermost root enclosing the current
// position is a backwards-compatible (v1) root, meaning include references
// at this position should be expanded.
func (c *includeContext) backcompatActive() bool {
	return len(c.roots) > 0 && c.roots[len(c.roots)-1].backcompat
}

// expandIncludes returns a copy of node's subtree with the backwards
// compatible include pattern expanded: a.include and object.include
// placeholders are replaced by the referenced element, while itemref and
// headers references append the referenced element after the node's own
// children, in token order.  Only same-page ID references (of the form
// "#id") are resolved, always against the original document rooted at
// p.root; copies produced by expansion never participate in ID lookups, and
// the input tree is never modified.
//
// References are expanded recursively while tracking the chain of original
// document nodes along the active expansion path.  A reference to a node
// already on that path is a back edge — following it would loop forever —
// so that single edge is skipped while the referencing node's other content
// and remaining references are processed normally.  This is deliberately
// neither a depth limit nor a global "already expanded" set: a fragment
// legitimately referenced from two sibling positions, listed twice in a
// single itemref attribute, or reached again through a diamond of
// references is expanded (and contributes its data) once per reference,
// because by the time the next reference is followed, the previous
// expansion has completed and the node is no longer on the path.
func (p *parser) expandIncludes(node *html.Node, ctx *includeContext) *html.Node {
	// template contents are not parsed, so copy them verbatim.
	if isAtom(node, atom.Template) {
		return cloneSubtree(node)
	}

	// Mirror walk's root class detection so that references expand exactly
	// when the innermost enclosing root is a v1 root.  The context in
	// effect at each reference position is used, so a shared fragment
	// expands independently under every root that references it.
	if node.Type == html.ElementNode {
		rootclasses, backcompat := rootClassesFor(getClasses(node), ctx.parent())
		if len(rootclasses) > 0 {
			slices.Sort(rootclasses)
			ctx.roots = append(ctx.roots, includeRoot{types: rootclasses, backcompat: backcompat})
			defer func() { ctx.roots = ctx.roots[:len(ctx.roots)-1] }()
		}
	}

	ctx.path[node] = true
	defer delete(ctx.path, node)

	var appendRefs []string
	if node.Type == html.ElementNode && ctx.backcompatActive() {
		refs, replace := p.backcompatIncludeRefs(node)
		if replace {
			for _, ref := range refs {
				target := findNodeByID(p.root, ref)
				if target == nil || ctx.path[target] {
					// Unresolvable reference or back edge: skip
					// just this edge and leave the placeholder
					// to be copied as an ordinary element.
					continue
				}
				// The referenced element replaces the placeholder
				// outright, properties of its own included.
				return p.expandIncludes(target, ctx)
			}
		} else {
			appendRefs = refs
		}
	}

	clone := cloneNodeShallow(node)
	for c := node.FirstChild; c != nil; c = c.NextSibling {
		clone.AppendChild(p.expandIncludes(c, ctx))
	}
	for _, ref := range appendRefs {
		target := findNodeByID(p.root, ref)
		if target == nil || ctx.path[target] {
			// Unresolvable reference or back edge: skip just this
			// token and keep appending any remaining references.
			continue
		}
		clone.AppendChild(p.expandIncludes(target, ctx))
	}
	return clone
}

// cloneNodeShallow copies node, including its attribute list, but none of
// its children or its links to parent and sibling nodes.
func cloneNodeShallow(node *html.Node) *html.Node {
	clone := &html.Node{
		Type:      node.Type,
		DataAtom:  node.DataAtom,
		Data:      node.Data,
		Namespace: node.Namespace,
	}
	if node.Attr != nil {
		clone.Attr = make([]html.Attribute, len(node.Attr))
		copy(clone.Attr, node.Attr)
	}
	return clone
}

// cloneSubtree returns a deep copy of node and its children, detached from
// the original tree.
func cloneSubtree(node *html.Node) *html.Node {
	clone := cloneNodeShallow(node)
	for c := node.FirstChild; c != nil; c = c.NextSibling {
		clone.AppendChild(cloneSubtree(c))
	}
	return clone
}

// findNodeByID searches node and its children, returning the node with the
// specified id value.
func findNodeByID(node *html.Node, id string) *html.Node {
	if getAttr(node, "id") == id {
		return node
	}
	for c := node.FirstChild; c != nil; c = c.NextSibling {
		if n := findNodeByID(c, id); n != nil {
			return n
		}
	}
	return nil
}

// isAncestorNode returns true if parent is an ancestor node of child.
func isAncestorNode(child, parent *html.Node) bool {
	for c := child; c != nil; c = c.Parent {
		if c == parent {
			return true
		}
	}
	return false
}
