package webdav

import (
	"bytes"
	"context"
	"encoding/xml"
	"fmt"
	"net/http"
	"net/url"
	"sort"
	"strings"

	"github.com/emersion/go-webdav/internal"
)

// BindingGraph describes the bindings discovered by Client.DiscoverBindings.
//
// A BindingNode is identified by the opaque DAV:resource-id reported by the
// server. The same node can be reached through several binding URLs: those
// URLs are kept on the edges, not on the node. BindingNode.URL is the first
// absolute URL at which the node was discovered.
type BindingGraph struct {
	RootID string
	Nodes  []BindingNode
	Edges  []BindingEdge
}

// BindingNode is a WebDAV resource in a BindingGraph.
type BindingNode struct {
	ID    string
	URL   string
	IsDir bool
}

// BindingEdge is a binding from a parent directory to a member resource.
// URL is the absolute URL of the binding as it appeared in the server's
// response, which can differ from the target node's URL when the same
// resource is mounted under several paths.
type BindingEdge struct {
	ParentID string
	URL      string
	TargetID string
}

// bindingResourceIDName is only used to ask for the property. Values are
// decoded into bindingResourceID.
var bindingResourceIDName = xml.Name{Space: internal.Namespace, Local: "resource-id"}

// https://tools.ietf.org/html/rfc6578#section-3.1 (DAV:resource-id)
type bindingResourceID struct {
	XMLName xml.Name `xml:"DAV: resource-id"`
	Hrefs   []string `xml:"DAV: href"`
}

var bindingPropFind = internal.NewPropNamePropFind(bindingResourceIDName, internal.ResourceTypeName)

type bindingEntry struct {
	id    string
	url   *url.URL
	isDir bool
}

type bindingPage struct {
	base    *url.URL
	self    bindingEntry
	members []bindingEntry
}

// DiscoverBindings walks the bindings below name using one Depth: 1 PROPFIND
// per directory. It builds a finite resource graph where nodes are identified
// by DAV:resource-id and edges preserve the absolute URL of each binding.
//
// Unlike ReadDir with recursive set to true, this method controls the walk
// itself: it can detect when several paths point at the same resource (or at
// an ancestor) and it only expands each directory once. Any malformed or
// inconsistent response makes the whole call fail; a partial graph is never
// returned.
func (c *Client) DiscoverBindings(ctx context.Context, name string) (*BindingGraph, error) {
	rootHref := c.ic.ResolveHref(name)
	page, err := c.discoverBindingPage(ctx, rootHref.String(), nil)
	if err != nil {
		return nil, err
	}
	rootOrigin := page.base

	g := &BindingGraph{RootID: page.self.id}
	nodeIndex := make(map[string]int)
	idForURL := make(map[string]string)
	expanded := make(map[string]bool)
	var queue []string

	rootNode := BindingNode{
		ID:    page.self.id,
		URL:   page.base.String(),
		IsDir: page.self.isDir,
	}
	g.Nodes = append(g.Nodes, rootNode)
	nodeIndex[rootNode.ID] = 0
	idForURL[rootNode.URL] = rootNode.ID
	expanded[rootNode.ID] = true

	if err := addBindingMembers(g, rootNode.ID, page.members, nodeIndex, idForURL, &queue); err != nil {
		return nil, err
	}

	for i := 0; i < len(queue); i++ {
		id := queue[i]
		if expanded[id] {
			continue
		}
		idx, ok := nodeIndex[id]
		if !ok {
			return nil, fmt.Errorf("webdav: internal error: queued node %q not in graph", id)
		}
		node := g.Nodes[idx]
		expanded[id] = true

		next, err := c.discoverBindingPage(ctx, node.URL, rootOrigin)
		if err != nil {
			return nil, err
		}
		if next.self.id != id {
			return nil, fmt.Errorf("webdav: resource at %q changed resource-id from %q to %q", node.URL, id, next.self.id)
		}
		if next.self.isDir != node.IsDir {
			return nil, fmt.Errorf("webdav: resource ID %q is both a file and a directory", id)
		}

		if err := addBindingMembers(g, id, next.members, nodeIndex, idForURL, &queue); err != nil {
			return nil, err
		}
	}

	return g, nil
}

// addBindingMembers discovers nodes and appends edges in the deterministic
// order in which members were sorted by their absolute URL. idForURL
// remembers which resource ID was seen at a binding URL in an earlier page, so
// that a href which resolves to the same URL but names a different resource
// is rejected instead of silently producing two bindings.
func addBindingMembers(g *BindingGraph, parentID string, members []bindingEntry, nodeIndex map[string]int, idForURL map[string]string, queue *[]string) error {
	for _, m := range members {
		key := m.url.String()
		if prevID, ok := idForURL[key]; ok && prevID != m.id {
			return fmt.Errorf("webdav: href %q was reported with conflicting resource IDs %q and %q", key, prevID, m.id)
		}
		idForURL[key] = m.id

		idx, ok := nodeIndex[m.id]
		if !ok {
			g.Nodes = append(g.Nodes, BindingNode{
				ID:    m.id,
				URL:   m.url.String(),
				IsDir: m.isDir,
			})
			idx = len(g.Nodes) - 1
			nodeIndex[m.id] = idx
			if m.isDir {
				*queue = append(*queue, m.id)
			}
		} else if g.Nodes[idx].IsDir != m.isDir {
			return fmt.Errorf("webdav: resource ID %q is both a file and a directory", m.id)
		}

		g.Edges = append(g.Edges, BindingEdge{
			ParentID: parentID,
			URL:      key,
			TargetID: m.id,
		})
	}
	return nil
}

// discoverBindingPage fetches one Depth: 1 page. rootOrigin is the final URL
// of the root page (nil for the root itself); member URLs must stay within
// that origin.
func (c *Client) discoverBindingPage(ctx context.Context, rawURL string, rootOrigin *url.URL) (*bindingPage, error) {
	var buf bytes.Buffer
	buf.WriteString(xml.Header)
	if err := xml.NewEncoder(&buf).Encode(bindingPropFind); err != nil {
		return nil, fmt.Errorf("webdav: failed to encode PROPFIND body: %w", err)
	}

	req, err := http.NewRequest("PROPFIND", rawURL, &buf)
	if err != nil {
		return nil, fmt.Errorf("webdav: failed to create PROPFIND request: %w", err)
	}
	req.Header.Set("Content-Type", "text/xml; charset=\"utf-8\"")
	req.Header.Set("Depth", internal.DepthOne.String())
	req = req.WithContext(ctx)

	resp, err := c.ic.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusMultiStatus {
		return nil, fmt.Errorf("webdav: PROPFIND %q returned %v, expected 207 Multi-Status", rawURL, resp.Status)
	}
	if resp.Request == nil || resp.Request.URL == nil {
		return nil, fmt.Errorf("webdav: PROPFIND %q response has no final request URL", rawURL)
	}
	base := resp.Request.URL

	var ms internal.MultiStatus
	if err := xml.NewDecoder(resp.Body).Decode(&ms); err != nil {
		return nil, fmt.Errorf("webdav: failed to decode PROPFIND %q response: %w", rawURL, err)
	}

	if rootOrigin == nil {
		rootOrigin = base
	}
	return parseBindingPage(base, rootOrigin, &ms)
}

func parseBindingPage(base, rootOrigin *url.URL, ms *internal.MultiStatus) (*bindingPage, error) {
	basePath := base.EscapedPath()

	page := &bindingPage{base: base}
	var members []bindingEntry
	selfCount := 0
	seenURLs := make(map[string]bindingEntry)

	for i := range ms.Responses {
		resp := &ms.Responses[i]

		entry, err := parseBindingResponse(base, rootOrigin, resp)
		if err != nil {
			return nil, err
		}

		if bindingSamePath(basePath, entry.url.EscapedPath()) {
			selfCount++
			if selfCount > 1 {
				return nil, fmt.Errorf("webdav: page %q contains more than one response for the directory itself", base)
			}
			page.self = entry
			continue
		}

		if !bindingDirectMember(basePath, entry.url.EscapedPath()) {
			return nil, fmt.Errorf("webdav: response URL %q is not a direct member of %q", entry.url, base)
		}

		key := entry.url.String()
		if prev, ok := seenURLs[key]; ok {
			if prev.id != entry.id {
				return nil, fmt.Errorf("webdav: duplicate href %q has conflicting resource IDs %q and %q", key, prev.id, entry.id)
			}
			if prev.isDir != entry.isDir {
				return nil, fmt.Errorf("webdav: duplicate href %q is marked as both a file and a directory", key)
			}
			// Completely identical duplicate members are merged.
			continue
		}
		seenURLs[key] = entry
		members = append(members, entry)
	}

	if selfCount != 1 {
		return nil, fmt.Errorf("webdav: page %q has %d responses for the directory itself, expected exactly one", base, selfCount)
	}

	// Server response order must not influence the resulting graph.
	sort.Slice(members, func(i, j int) bool {
		return members[i].url.String() < members[j].url.String()
	})
	page.members = members
	return page, nil
}

// countBindingProp counts a property across all successful propstat elements.
// A property reported exactly once is required; duplicates are ambiguous.
func countBindingProp(resp *internal.Response, name xml.Name) int {
	count := 0
	for _, propstat := range resp.PropStats {
		for i := range propstat.Prop.Raw {
			if n, ok := propstat.Prop.Raw[i].XMLName(); ok && n == name {
				count++
			}
		}
	}
	return count
}

func parseBindingResponse(base, rootOrigin *url.URL, resp *internal.Response) (bindingEntry, error) {
	var zero bindingEntry

	if len(resp.Hrefs) != 1 {
		return zero, fmt.Errorf("webdav: response has %d href elements, expected exactly one", len(resp.Hrefs))
	}

	if err := resp.Err(); err != nil {
		return zero, fmt.Errorf("webdav: non-success response: %w", err)
	}
	// A requested property reported under a non-success propstat is an error
	// (typically a 404 for a missing property); propstats about unrelated
	// properties don't invalidate the response.
	for _, propstat := range resp.PropStats {
		if propstat.Status.Code >= 200 && propstat.Status.Code < 300 {
			continue
		}
		for _, name := range []xml.Name{bindingResourceIDName, internal.ResourceTypeName} {
			if propstat.Prop.Get(name) != nil {
				return zero, fmt.Errorf("webdav: %v property has non-success status %v", name.Local, propstat.Status.Code)
			}
		}
	}
	for _, name := range []xml.Name{bindingResourceIDName, internal.ResourceTypeName} {
		if count := countBindingProp(resp, name); count != 1 {
			return zero, fmt.Errorf("webdav: response has %d %v properties, expected exactly one", count, name.Local)
		}
	}

	var idProp bindingResourceID
	if err := resp.DecodeProp(&idProp); err != nil {
		return zero, fmt.Errorf("webdav: missing or invalid DAV:resource-id: %w", err)
	}
	if len(idProp.Hrefs) != 1 {
		return zero, fmt.Errorf("webdav: DAV:resource-id has %d href elements, expected exactly one", len(idProp.Hrefs))
	}
	id := idProp.Hrefs[0]
	if id == "" {
		return zero, fmt.Errorf("webdav: DAV:resource-id has an empty href")
	}
	idURL, err := url.Parse(id)
	if err != nil || !idURL.IsAbs() {
		return zero, fmt.Errorf("webdav: DAV:resource-id href %q is not an absolute URI", id)
	}

	var resType internal.ResourceType
	if err := resp.DecodeProp(&resType); err != nil {
		return zero, fmt.Errorf("webdav: missing or invalid DAV:resourcetype: %w", err)
	}

	ref := (*url.URL)(&resp.Hrefs[0])
	resolved := base.ResolveReference(ref)

	if !sameBindingOrigin(resolved, rootOrigin) {
		return zero, fmt.Errorf("webdav: response URL %q is outside the root origin %q", resolved, rootOrigin)
	}
	if resolved.User != nil || ref.User != nil {
		return zero, fmt.Errorf("webdav: response URL %q contains userinfo", resolved)
	}
	if resolved.RawQuery != "" || resolved.ForceQuery || ref.RawQuery != "" || ref.ForceQuery {
		return zero, fmt.Errorf("webdav: response URL %q contains a query", resolved)
	}
	if resolved.Fragment != "" || ref.Fragment != "" || resolved.RawFragment != "" {
		return zero, fmt.Errorf("webdav: response URL %q contains a fragment", resolved)
	}

	return bindingEntry{
		id:    id,
		url:   resolved,
		isDir: resType.Is(internal.CollectionName),
	}, nil
}

// bindingSegments splits an escaped URL path into escaped segments. Percent
// sequences stay inside their segment: %2F is a literal byte in a name, not a
// path separator. A single trailing slash (directory form) is ignored.
func bindingSegments(p string) []string {
	if p == "" {
		return nil
	}
	if !strings.HasPrefix(p, "/") {
		p = "/" + p
	}
	segs := strings.Split(p[1:], "/")
	if len(segs) > 0 && segs[len(segs)-1] == "" {
		segs = segs[:len(segs)-1]
	}
	return segs
}

// bindingSamePath compares two escaped paths segment by segment. This
// accepts the trailing-slash difference between a directory URL and the
// request URL that named it, without ever treating %2F as a separator.
func bindingSamePath(a, b string) bool {
	as, bs := bindingSegments(a), bindingSegments(b)
	if len(as) != len(bs) {
		return false
	}
	for i := range as {
		if as[i] != bs[i] {
			return false
		}
	}
	return true
}

func bindingDirectMember(basePath, memberPath string) bool {
	base, member := bindingSegments(basePath), bindingSegments(memberPath)
	if len(member) != len(base)+1 {
		return false
	}
	for i := range base {
		if base[i] != member[i] {
			return false
		}
	}
	return true
}

func sameBindingOrigin(a, b *url.URL) bool {
	if !strings.EqualFold(a.Scheme, b.Scheme) {
		return false
	}
	if !strings.EqualFold(a.Hostname(), b.Hostname()) {
		return false
	}
	return bindingEffectivePort(a) == bindingEffectivePort(b)
}

func bindingEffectivePort(u *url.URL) string {
	if p := u.Port(); p != "" {
		return p
	}
	switch strings.ToLower(u.Scheme) {
	case "http":
		return "80"
	case "https":
		return "443"
	}
	return ""
}
