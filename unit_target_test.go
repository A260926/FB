package webdav

import (
	"context"
	"encoding/xml"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"reflect"
	"strings"
	"testing"

	"github.com/emersion/go-webdav/internal"
	"github.com/emersion/go-webdav/support/unitcheck"
)

// --- test doubles -----------------------------------------------------------

type bindingCtxKey struct{}

func bindingTestContext() context.Context {
	return context.WithValue(context.Background(), bindingCtxKey{}, true)
}

type bindingTestReply struct {
	status   int    // defaults to 207
	body     string // response body
	finalURL string // optional override for Response.Request.URL
}

type bindingTestCall struct {
	method string
	url    string
	depth  string
	props  []xml.Name
	ctxOK  bool
}

type bindingTestServer struct {
	t            *testing.T
	replies      map[string]*bindingTestReply
	calls        []bindingTestCall
	closedBodies int
	unexpected   []string
}

func (s *bindingTestServer) Do(req *http.Request) (*http.Response, error) {
	body, err := io.ReadAll(req.Body)
	if err != nil {
		s.t.Errorf("reading request body: %v", err)
	}
	req.Body.Close()

	var props []xml.Name
	var doc internal.PropFind
	if err := xml.Unmarshal(body, &doc); err == nil && doc.Prop != nil {
		for i := range doc.Prop.Raw {
			if name, ok := doc.Prop.Raw[i].XMLName(); ok {
				props = append(props, name)
			}
		}
	}

	s.calls = append(s.calls, bindingTestCall{
		method: req.Method,
		url:    req.URL.String(),
		depth:  req.Header.Get("Depth"),
		props:  props,
		ctxOK:  req.Context().Value(bindingCtxKey{}) == true,
	})

	key := req.URL.String()
	reply, ok := s.replies[key]
	if !ok {
		s.unexpected = append(s.unexpected, key)
		return nil, fmt.Errorf("unexpected request URL %q", key)
	}

	status := reply.status
	if status == 0 {
		status = http.StatusMultiStatus
	}

	final := req.URL
	if reply.finalURL != "" {
		u, err := url.Parse(reply.finalURL)
		if err != nil {
			return nil, err
		}
		final = u
	}
	reqCopy := req.Clone(req.Context())
	reqCopy.URL = final

	return &http.Response{
		StatusCode: status,
		Header:     make(http.Header),
		Body: &bindingTrackingBody{
			Reader: strings.NewReader(reply.body),
			onClose: func() {
				s.closedBodies++
			},
		},
		Request: reqCopy,
	}, nil
}

type bindingTrackingBody struct {
	io.Reader
	closed  bool
	onClose func()
}

func (b *bindingTrackingBody) Close() error {
	if !b.closed {
		b.closed = true
		if b.onClose != nil {
			b.onClose()
		}
	}
	return nil
}

type bindingTestMember struct {
	href string
	id   string
	dir  bool
}

func bindingTestMultistatus(self bindingTestMember, members []bindingTestMember) string {
	var buf strings.Builder
	buf.WriteString(`<?xml version="1.0" encoding="utf-8"?>`)
	buf.WriteString(`<multistatus xmlns="DAV:">`)
	writeResponse := func(m bindingTestMember) {
		buf.WriteString(`<response><href>`)
		buf.WriteString(m.href)
		buf.WriteString(`</href><propstat><prop><resource-id><href>`)
		buf.WriteString(m.id)
		buf.WriteString(`</href></resource-id><resourcetype>`)
		if m.dir {
			buf.WriteString(`<collection/>`)
		}
		buf.WriteString(`</resourcetype></prop><status>HTTP/1.1 200 OK</status></propstat></response>`)
	}
	writeResponse(self)
	for _, m := range members {
		writeResponse(m)
	}
	buf.WriteString(`</multistatus>`)
	return buf.String()
}

func newBindingTestClient(t *testing.T, endpoint string, replies map[string]*bindingTestReply) (*Client, *bindingTestServer) {
	t.Helper()
	srv := &bindingTestServer{t: t, replies: replies}
	c, err := NewClient(srv, endpoint)
	if err != nil {
		t.Fatalf("NewClient() = %v", err)
	}
	return c, srv
}

// --- shared assertions ------------------------------------------------------

var bindingRequestedProps = []xml.Name{
	{Space: "DAV:", Local: "resource-id"},
	{Space: "DAV:", Local: "resourcetype"},
}

func checkBindingCalls(c *unitcheck.C, t *testing.T, srv *bindingTestServer, wantURLs []string) {
	t.Helper()
	c.Check(t, len(srv.calls) == len(wantURLs), "made %d requests, want %d", len(srv.calls), len(wantURLs))
	for i := range srv.calls {
		if i >= len(wantURLs) {
			break
		}
		call := srv.calls[i]
		c.Check(t, call.method == "PROPFIND", "request %d method = %q, want PROPFIND", i, call.method)
		c.Check(t, call.depth == "1", "request %d Depth = %q, want 1", i, call.depth)
		c.Check(t, reflect.DeepEqual(call.props, bindingRequestedProps),
			"request %d requested properties = %v, want %v", i, call.props, bindingRequestedProps)
		c.Check(t, call.url == wantURLs[i], "request %d URL = %q, want %q", i, call.url, wantURLs[i])
		c.Check(t, call.ctxOK, "request %d did not carry the caller's context", i)
	}
	c.Check(t, len(srv.unexpected) == 0, "unexpected request URLs: %v", srv.unexpected)
	c.Check(t, srv.closedBodies == len(srv.calls), "closed %d response bodies for %d requests", srv.closedBodies, len(srv.calls))
}

func checkBindingGraph(c *unitcheck.C, t *testing.T, g *BindingGraph, want *BindingGraph) {
	t.Helper()
	if g == nil {
		c.Check(t, false, "DiscoverBindings returned a nil graph")
		return
	}
	c.Check(t, g.RootID == want.RootID, "RootID = %q, want %q", g.RootID, want.RootID)
	c.Check(t, reflect.DeepEqual(g.Nodes, want.Nodes), "Nodes = %v, want %v", g.Nodes, want.Nodes)
	c.Check(t, reflect.DeepEqual(g.Edges, want.Edges), "Edges = %v, want %v", g.Edges, want.Edges)

	ids := make(map[string]bool)
	duplicates := 0
	for _, n := range g.Nodes {
		if ids[n.ID] {
			duplicates++
		}
		ids[n.ID] = true
	}
	c.Check(t, duplicates == 0, "graph contains %d duplicate node IDs", duplicates)
}

func checkBindingFailure(c *unitcheck.C, t *testing.T, g *BindingGraph, err error, srv *bindingTestServer, wantCalls int) {
	t.Helper()
	c.Check(t, err != nil, "DiscoverBindings succeeded, want an error")
	c.Check(t, g == nil, "DiscoverBindings returned a partial graph alongside error %v", err)
	c.Check(t, len(srv.calls) == wantCalls, "made %d requests before failing, want %d", len(srv.calls), wantCalls)
	c.Check(t, len(srv.unexpected) == 0, "unexpected request URLs: %v", srv.unexpected)
	c.Check(t, srv.closedBodies == len(srv.calls), "closed %d response bodies for %d requests", srv.closedBodies, len(srv.calls))
}

func mustBindings(c *unitcheck.C, t *testing.T, c0 *Client, name string) *BindingGraph {
	t.Helper()
	g, err := c0.DiscoverBindings(bindingTestContext(), name)
	if err != nil {
		c.Check(t, false, "DiscoverBindings(%q) = %v", name, err)
		return nil
	}
	return g
}

// --- identity ---------------------------------------------------------------

func TestBindingIdentity(t *testing.T) {
	unitcheck.Run(t, func(c *unitcheck.C) {
		t.Run("DiamondAndAncestorBackEdge", func(t *testing.T) {
			root := bindingTestMember{href: "/root/", id: "urn:uuid:root", dir: true}
			left := bindingTestMember{href: "/root/left/", id: "urn:uuid:shared", dir: true}
			right := bindingTestMember{href: "/root/right/", id: "urn:uuid:shared", dir: true}
			file := bindingTestMember{href: "/root/file.txt", id: "urn:uuid:file"}
			inner := bindingTestMember{href: "/root/left/inner.txt", id: "urn:uuid:inner"}
			back := bindingTestMember{href: "/root/left/back/", id: "urn:uuid:root", dir: true}

			client, srv := newBindingTestClient(t, "https://dav.example/root/", map[string]*bindingTestReply{
				"https://dav.example/root/": {
					body: bindingTestMultistatus(root, []bindingTestMember{right, file, left}),
				},
				"https://dav.example/root/left/": {
					body: bindingTestMultistatus(left, []bindingTestMember{back, inner}),
				},
			})

			g := mustBindings(c, t, client, "")
			want := &BindingGraph{
				RootID: "urn:uuid:root",
				Nodes: []BindingNode{
					{ID: "urn:uuid:root", URL: "https://dav.example/root/", IsDir: true},
					{ID: "urn:uuid:file", URL: "https://dav.example/root/file.txt"},
					{ID: "urn:uuid:shared", URL: "https://dav.example/root/left/", IsDir: true},
					{ID: "urn:uuid:inner", URL: "https://dav.example/root/left/inner.txt"},
				},
				Edges: []BindingEdge{
					{ParentID: "urn:uuid:root", URL: "https://dav.example/root/file.txt", TargetID: "urn:uuid:file"},
					{ParentID: "urn:uuid:root", URL: "https://dav.example/root/left/", TargetID: "urn:uuid:shared"},
					{ParentID: "urn:uuid:root", URL: "https://dav.example/root/right/", TargetID: "urn:uuid:shared"},
					{ParentID: "urn:uuid:shared", URL: "https://dav.example/root/left/back/", TargetID: "urn:uuid:root"},
					{ParentID: "urn:uuid:shared", URL: "https://dav.example/root/left/inner.txt", TargetID: "urn:uuid:inner"},
				},
			}
			checkBindingGraph(c, t, g, want)
			checkBindingCalls(c, t, srv, []string{
				"https://dav.example/root/",
				"https://dav.example/root/left/",
			})

			// The right alias must not overwrite the first discovered node URL,
			// and the ancestor back-edge must still be present.
			if g != nil {
				c.Check(t, g.Nodes[2].URL == "https://dav.example/root/left/",
					"shared node URL = %q, want the first discovered alias", g.Nodes[2].URL)
				c.Check(t, len(g.Edges) == 5, "edges = %d, want 5", len(g.Edges))
				c.Check(t, g.Edges[2].URL == "https://dav.example/root/right/",
					"right alias edge URL = %q", g.Edges[2].URL)
			}
		})

		t.Run("ResponseOrderInvariant", func(t *testing.T) {
			root := bindingTestMember{href: "/root/", id: "urn:uuid:root", dir: true}
			alpha := bindingTestMember{href: "/root/alpha/", id: "urn:uuid:alpha", dir: true}
			zeta := bindingTestMember{href: "/root/zeta/", id: "urn:uuid:zeta", dir: true}
			file := bindingTestMember{href: "/root/zeta/z.txt", id: "urn:uuid:zfile"}

			orders := [][2]bindingTestMember{{zeta, alpha}, {alpha, zeta}}
			var graphs []*BindingGraph
			for _, order := range orders {
				client, srv := newBindingTestClient(t, "https://dav.example/root/", map[string]*bindingTestReply{
					"https://dav.example/root/": {
						body: bindingTestMultistatus(root, []bindingTestMember{order[0], order[1]}),
					},
					"https://dav.example/root/alpha/": {
						body: bindingTestMultistatus(alpha, nil),
					},
					"https://dav.example/root/zeta/": {
						body: bindingTestMultistatus(zeta, []bindingTestMember{file}),
					},
				})
				g := mustBindings(c, t, client, "")
				checkBindingCalls(c, t, srv, []string{
					"https://dav.example/root/",
					"https://dav.example/root/alpha/",
					"https://dav.example/root/zeta/",
				})
				graphs = append(graphs, g)
			}
			if len(graphs) == 2 && graphs[0] != nil && graphs[1] != nil {
				c.Check(t, reflect.DeepEqual(graphs[0], graphs[1]),
					"response ordering changed the graph: %v vs %v", graphs[0], graphs[1])
				c.Check(t, graphs[0].Nodes[1].ID == "urn:uuid:alpha",
					"first node after root = %q, want the alphabetically first member", graphs[0].Nodes[1].ID)
			}
		})

		t.Run("IdenticalDuplicateMerged", func(t *testing.T) {
			root := bindingTestMember{href: "/root/", id: "urn:uuid:root", dir: true}
			dup := bindingTestMember{href: "/root/only.txt", id: "urn:uuid:only"}
			client, srv := newBindingTestClient(t, "https://dav.example/root/", map[string]*bindingTestReply{
				"https://dav.example/root/": {
					body: bindingTestMultistatus(root, []bindingTestMember{dup, dup}),
				},
			})
			g := mustBindings(c, t, client, "")
			want := &BindingGraph{
				RootID: "urn:uuid:root",
				Nodes: []BindingNode{
					{ID: "urn:uuid:root", URL: "https://dav.example/root/", IsDir: true},
					{ID: "urn:uuid:only", URL: "https://dav.example/root/only.txt"},
				},
				Edges: []BindingEdge{
					{ParentID: "urn:uuid:root", URL: "https://dav.example/root/only.txt", TargetID: "urn:uuid:only"},
				},
			}
			checkBindingGraph(c, t, g, want)
			checkBindingCalls(c, t, srv, []string{"https://dav.example/root/"})
		})

		t.Run("TrailingSlashAndFinalURL", func(t *testing.T) {
			// The request URL has no trailing slash and the server redirects
			// to another path; the root node URL must be the final request URL.
			root := bindingTestMember{href: "https://dav.example/actual/", id: "urn:uuid:root", dir: true}
			child := bindingTestMember{href: "child", id: "urn:uuid:child"}
			client, srv := newBindingTestClient(t, "https://dav.example/requested", map[string]*bindingTestReply{
				"https://dav.example/requested": {
					body:     bindingTestMultistatus(root, []bindingTestMember{child}),
					finalURL: "https://dav.example/actual/",
				},
			})
			g := mustBindings(c, t, client, "")
			want := &BindingGraph{
				RootID: "urn:uuid:root",
				Nodes: []BindingNode{
					{ID: "urn:uuid:root", URL: "https://dav.example/actual/", IsDir: true},
					{ID: "urn:uuid:child", URL: "https://dav.example/actual/child"},
				},
				Edges: []BindingEdge{
					{ParentID: "urn:uuid:root", URL: "https://dav.example/actual/child", TargetID: "urn:uuid:child"},
				},
			}
			checkBindingGraph(c, t, g, want)
			checkBindingCalls(c, t, srv, []string{"https://dav.example/requested"})
			if g != nil {
				c.Check(t, g.Nodes[0].URL == "https://dav.example/actual/",
					"root node URL = %q, want the final request URL", g.Nodes[0].URL)
			}
		})

		t.Run("CallsDoNotShareState", func(t *testing.T) {
			root := bindingTestMember{href: "/root/", id: "urn:uuid:root", dir: true}
			dir := bindingTestMember{href: "/root/d/", id: "urn:uuid:d", dir: true}
			leaf := bindingTestMember{href: "/root/d/leaf", id: "urn:uuid:leaf"}
			client, srv := newBindingTestClient(t, "https://dav.example/root/", map[string]*bindingTestReply{
				"https://dav.example/root/": {
					body: bindingTestMultistatus(root, []bindingTestMember{dir}),
				},
				"https://dav.example/root/d/": {
					body: bindingTestMultistatus(dir, []bindingTestMember{leaf}),
				},
			})
			g1 := mustBindings(c, t, client, "")
			g2 := mustBindings(c, t, client, "")
			checkBindingCalls(c, t, srv, []string{
				"https://dav.example/root/", "https://dav.example/root/d/",
				"https://dav.example/root/", "https://dav.example/root/d/",
			})
			if g1 != nil && g2 != nil {
				c.Check(t, reflect.DeepEqual(g1, g2), "two calls produced different graphs: %v vs %v", g1, g2)
			}
		})
	})
}

// --- href handling ----------------------------------------------------------

func TestBindingHref(t *testing.T) {
	unitcheck.Run(t, func(c *unitcheck.C) {
		t.Run("MixedAbsoluteRelativeEscapedSequence", func(t *testing.T) {
			root := bindingTestMember{href: "/root/", id: "urn:uuid:root", dir: true}
			// Escaped slash inside a single segment: %2F must not act as a
			// path separator. The href is absolute here.
			absDir := bindingTestMember{href: "https://dav.example/root/a%2Fb/", id: "urn:uuid:abs", dir: true}
			// Relative href; its node is a directory that expands.
			relDir := bindingTestMember{href: "rel/", id: "urn:uuid:rel", dir: true}
			absLeaf := bindingTestMember{href: "https://dav.example/root/a%2Fb/leaf.txt", id: "urn:uuid:leaf"}
			// Relative member of the relative directory.
			relLeaf := bindingTestMember{href: "x.txt", id: "urn:uuid:relleaf"}

			client, srv := newBindingTestClient(t, "https://dav.example/root/", map[string]*bindingTestReply{
				"https://dav.example/root/": {
					// Deliberately out of order: "rel/" sorts after "a%2Fb/".
					body: bindingTestMultistatus(root, []bindingTestMember{relDir, absDir}),
				},
				"https://dav.example/root/a%2Fb/": {
					body: bindingTestMultistatus(absDir, []bindingTestMember{absLeaf}),
				},
				"https://dav.example/root/rel/": {
					body: bindingTestMultistatus(
						bindingTestMember{href: "/root/rel/", id: "urn:uuid:rel", dir: true},
						[]bindingTestMember{relLeaf},
					),
				},
			})

			g := mustBindings(c, t, client, "")
			want := &BindingGraph{
				RootID: "urn:uuid:root",
				Nodes: []BindingNode{
					{ID: "urn:uuid:root", URL: "https://dav.example/root/", IsDir: true},
					{ID: "urn:uuid:abs", URL: "https://dav.example/root/a%2Fb/", IsDir: true},
					{ID: "urn:uuid:rel", URL: "https://dav.example/root/rel/", IsDir: true},
					{ID: "urn:uuid:leaf", URL: "https://dav.example/root/a%2Fb/leaf.txt"},
					{ID: "urn:uuid:relleaf", URL: "https://dav.example/root/rel/x.txt"},
				},
				Edges: []BindingEdge{
					{ParentID: "urn:uuid:root", URL: "https://dav.example/root/a%2Fb/", TargetID: "urn:uuid:abs"},
					{ParentID: "urn:uuid:root", URL: "https://dav.example/root/rel/", TargetID: "urn:uuid:rel"},
					{ParentID: "urn:uuid:abs", URL: "https://dav.example/root/a%2Fb/leaf.txt", TargetID: "urn:uuid:leaf"},
					{ParentID: "urn:uuid:rel", URL: "https://dav.example/root/rel/x.txt", TargetID: "urn:uuid:relleaf"},
				},
			}
			checkBindingGraph(c, t, g, want)
			checkBindingCalls(c, t, srv, []string{
				"https://dav.example/root/",
				"https://dav.example/root/a%2Fb/",
				"https://dav.example/root/rel/",
			})
			if g != nil {
				c.Check(t, strings.Contains(g.Nodes[1].URL, "%2F"),
					"escaped slash was lost in node URL %q", g.Nodes[1].URL)
				// No request may be inferred from a decoded path.
				for _, call := range srv.calls {
					c.Check(t, call.url != "https://dav.example/root/a/b/",
						"escaped slash was decoded into a path separator in %q", call.url)
				}
			}
		})

		t.Run("RelativeSelfAndMemberHrefs", func(t *testing.T) {
			// Self href is relative to the final request URL, member hrefs
			// are relative too.
			root := bindingTestMember{href: ".", id: "urn:uuid:root", dir: true}
			child := bindingTestMember{href: "one.txt", id: "urn:uuid:one"}
			client, srv := newBindingTestClient(t, "https://dav.example/root/", map[string]*bindingTestReply{
				"https://dav.example/root/": {
					body: bindingTestMultistatus(root, []bindingTestMember{child}),
				},
			})
			g := mustBindings(c, t, client, "")
			want := &BindingGraph{
				RootID: "urn:uuid:root",
				Nodes: []BindingNode{
					{ID: "urn:uuid:root", URL: "https://dav.example/root/", IsDir: true},
					{ID: "urn:uuid:one", URL: "https://dav.example/root/one.txt"},
				},
				Edges: []BindingEdge{
					{ParentID: "urn:uuid:root", URL: "https://dav.example/root/one.txt", TargetID: "urn:uuid:one"},
				},
			}
			checkBindingGraph(c, t, g, want)
			checkBindingCalls(c, t, srv, []string{"https://dav.example/root/"})
		})

		t.Run("SelfTrailingSlashDifferenceAccepted", func(t *testing.T) {
			root := bindingTestMember{href: "/root", id: "urn:uuid:root", dir: true}
			file := bindingTestMember{href: "/root/a.txt", id: "urn:uuid:a"}
			client, srv := newBindingTestClient(t, "https://dav.example/root/", map[string]*bindingTestReply{
				"https://dav.example/root/": {
					body: bindingTestMultistatus(root, []bindingTestMember{file}),
				},
			})
			g := mustBindings(c, t, client, "")
			want := &BindingGraph{
				RootID: "urn:uuid:root",
				Nodes: []BindingNode{
					{ID: "urn:uuid:root", URL: "https://dav.example/root/", IsDir: true},
					{ID: "urn:uuid:a", URL: "https://dav.example/root/a.txt"},
				},
				Edges: []BindingEdge{
					{ParentID: "urn:uuid:root", URL: "https://dav.example/root/a.txt", TargetID: "urn:uuid:a"},
				},
			}
			checkBindingGraph(c, t, g, want)
			checkBindingCalls(c, t, srv, []string{"https://dav.example/root/"})
		})
	})
}

// --- failures ---------------------------------------------------------------

func TestBindingFailure(t *testing.T) {
	unitcheck.Run(t, func(c *unitcheck.C) {
		root := bindingTestMember{href: "/root/", id: "urn:uuid:root", dir: true}

		t.Run("MissingResourceID", func(t *testing.T) {
			body := `<multistatus xmlns="DAV:"><response><href>/root/</href><propstat><prop>` +
				`<resourcetype><collection/></resourcetype>` +
				`</prop><status>HTTP/1.1 200 OK</status></propstat></response></multistatus>`
			client, srv := newBindingTestClient(t, "https://dav.example/root/", map[string]*bindingTestReply{
				"https://dav.example/root/": {body: body},
			})
			g, err := client.DiscoverBindings(bindingTestContext(), "")
			checkBindingFailure(c, t, g, err, srv, 1)
		})

		t.Run("MissingResourceType", func(t *testing.T) {
			body := `<multistatus xmlns="DAV:"><response><href>/root/</href><propstat><prop>` +
				`<resource-id><href>urn:uuid:root</href></resource-id>` +
				`</prop><status>HTTP/1.1 200 OK</status></propstat></response></multistatus>`
			client, srv := newBindingTestClient(t, "https://dav.example/root/", map[string]*bindingTestReply{
				"https://dav.example/root/": {body: body},
			})
			g, err := client.DiscoverBindings(bindingTestContext(), "")
			checkBindingFailure(c, t, g, err, srv, 1)
		})

		t.Run("ConflictingDuplicateHref", func(t *testing.T) {
			a := bindingTestMember{href: "/root/a.txt", id: "urn:uuid:a"}
			b := bindingTestMember{href: "/root/a.txt", id: "urn:uuid:b"}
			client, srv := newBindingTestClient(t, "https://dav.example/root/", map[string]*bindingTestReply{
				"https://dav.example/root/": {
					body: bindingTestMultistatus(root, []bindingTestMember{a, b}),
				},
			})
			g, err := client.DiscoverBindings(bindingTestContext(), "")
			checkBindingFailure(c, t, g, err, srv, 1)
		})

		t.Run("SameIDFileAndDirInOnePage", func(t *testing.T) {
			dir := bindingTestMember{href: "/root/a/", id: "urn:uuid:x", dir: true}
			file := bindingTestMember{href: "/root/a.txt", id: "urn:uuid:x"}
			client, srv := newBindingTestClient(t, "https://dav.example/root/", map[string]*bindingTestReply{
				"https://dav.example/root/": {
					body: bindingTestMultistatus(root, []bindingTestMember{dir, file}),
				},
			})
			g, err := client.DiscoverBindings(bindingTestContext(), "")
			checkBindingFailure(c, t, g, err, srv, 1)
		})

		t.Run("SameIDFileAndDirAcrossPages", func(t *testing.T) {
			dir := bindingTestMember{href: "/root/d/", id: "urn:uuid:d", dir: true}
			alias := bindingTestMember{href: "/root/d/alias.txt", id: "urn:uuid:d"}
			client, srv := newBindingTestClient(t, "https://dav.example/root/", map[string]*bindingTestReply{
				"https://dav.example/root/": {
					body: bindingTestMultistatus(root, []bindingTestMember{dir}),
				},
				"https://dav.example/root/d/": {
					body: bindingTestMultistatus(
						bindingTestMember{href: "/root/d/", id: "urn:uuid:d", dir: true},
						[]bindingTestMember{alias},
					),
				},
			})
			g, err := client.DiscoverBindings(bindingTestContext(), "")
			checkBindingFailure(c, t, g, err, srv, 2)
		})

		t.Run("NonSuccessPropstat", func(t *testing.T) {
			body := `<multistatus xmlns="DAV:"><response><href>/root/</href>` +
				`<propstat><prop><resource-id><href>urn:uuid:root</href></resource-id>` +
				`<resourcetype><collection/></resourcetype></prop>` +
				`<status>HTTP/1.1 404 Not Found</status></propstat></response></multistatus>`
			client, srv := newBindingTestClient(t, "https://dav.example/root/", map[string]*bindingTestReply{
				"https://dav.example/root/": {body: body},
			})
			g, err := client.DiscoverBindings(bindingTestContext(), "")
			checkBindingFailure(c, t, g, err, srv, 1)
		})

		t.Run("NotMultiStatusHTTPStatus", func(t *testing.T) {
			body := bindingTestMultistatus(root, nil)
			client, srv := newBindingTestClient(t, "https://dav.example/root/", map[string]*bindingTestReply{
				"https://dav.example/root/": {status: http.StatusOK, body: body},
			})
			g, err := client.DiscoverBindings(bindingTestContext(), "")
			checkBindingFailure(c, t, g, err, srv, 1)
		})

		t.Run("OutOfBoundsCrossOrigin", func(t *testing.T) {
			file := bindingTestMember{href: "https://other.example/root/x", id: "urn:uuid:x"}
			client, srv := newBindingTestClient(t, "https://dav.example/root/", map[string]*bindingTestReply{
				"https://dav.example/root/": {
					body: bindingTestMultistatus(root, []bindingTestMember{file}),
				},
			})
			g, err := client.DiscoverBindings(bindingTestContext(), "")
			checkBindingFailure(c, t, g, err, srv, 1)
		})

		t.Run("OutOfBoundsDeeperPath", func(t *testing.T) {
			deep := bindingTestMember{href: "/root/sub/deep.txt", id: "urn:uuid:deep"}
			client, srv := newBindingTestClient(t, "https://dav.example/root/", map[string]*bindingTestReply{
				"https://dav.example/root/": {
					body: bindingTestMultistatus(root, []bindingTestMember{deep}),
				},
			})
			g, err := client.DiscoverBindings(bindingTestContext(), "")
			checkBindingFailure(c, t, g, err, srv, 1)
		})

		t.Run("OutOfBoundsQueryUserinfoFragment", func(t *testing.T) {
			cases := map[string]string{
				"query":    "/root/x?q=1",
				"userinfo": "https://user@dav.example/root/x",
				"fragment": "/root/x#frag",
			}
			for name, href := range cases {
				t.Run(name, func(t *testing.T) {
					file := bindingTestMember{href: href, id: "urn:uuid:x"}
					client, srv := newBindingTestClient(t, "https://dav.example/root/", map[string]*bindingTestReply{
						"https://dav.example/root/": {
							body: bindingTestMultistatus(root, []bindingTestMember{file}),
						},
					})
					g, err := client.DiscoverBindings(bindingTestContext(), "")
					checkBindingFailure(c, t, g, err, srv, 1)
				})
			}
		})

		t.Run("MalformedXML", func(t *testing.T) {
			body := `<multistatus xmlns="DAV:"><response><href>/root/</href>` +
				`<propstat><prop><resource-id><href>urn:uuid:root</href></resource-id>`
			client, srv := newBindingTestClient(t, "https://dav.example/root/", map[string]*bindingTestReply{
				"https://dav.example/root/": {body: body},
			})
			g, err := client.DiscoverBindings(bindingTestContext(), "")
			checkBindingFailure(c, t, g, err, srv, 1)
		})

		t.Run("LaterPageIdentityChanged", func(t *testing.T) {
			dir := bindingTestMember{href: "/root/d/", id: "urn:uuid:d", dir: true}
			changed := bindingTestMember{href: "/root/d/", id: "urn:uuid:other", dir: true}
			client, srv := newBindingTestClient(t, "https://dav.example/root/", map[string]*bindingTestReply{
				"https://dav.example/root/": {
					body: bindingTestMultistatus(root, []bindingTestMember{dir}),
				},
				"https://dav.example/root/d/": {
					body: bindingTestMultistatus(changed, nil),
				},
			})
			g, err := client.DiscoverBindings(bindingTestContext(), "")
			checkBindingFailure(c, t, g, err, srv, 2)
			// A failed page must not be followed by another request.
			c.Check(t, len(srv.calls) == 2, "sent %d requests after the failing page", len(srv.calls))
		})

		t.Run("RootSelfMissing", func(t *testing.T) {
			file := bindingTestMember{href: "/root/a.txt", id: "urn:uuid:a"}
			client, srv := newBindingTestClient(t, "https://dav.example/root/", map[string]*bindingTestReply{
				"https://dav.example/root/": {
					body: bindingTestMultistatus(file, nil),
				},
			})
			g, err := client.DiscoverBindings(bindingTestContext(), "")
			checkBindingFailure(c, t, g, err, srv, 1)
		})

		t.Run("MultipleSelfResponses", func(t *testing.T) {
			body := `<multistatus xmlns="DAV:">` +
				`<response><href>/root/</href><propstat><prop>` +
				`<resource-id><href>urn:uuid:root</href></resource-id>` +
				`<resourcetype><collection/></resourcetype>` +
				`</prop><status>HTTP/1.1 200 OK</status></propstat></response>` +
				`<response><href>/root</href><propstat><prop>` +
				`<resource-id><href>urn:uuid:root</href></resource-id>` +
				`<resourcetype><collection/></resourcetype>` +
				`</prop><status>HTTP/1.1 200 OK</status></propstat></response>` +
				`</multistatus>`
			client, srv := newBindingTestClient(t, "https://dav.example/root/", map[string]*bindingTestReply{
				"https://dav.example/root/": {body: body},
			})
			g, err := client.DiscoverBindings(bindingTestContext(), "")
			checkBindingFailure(c, t, g, err, srv, 1)
		})

		t.Run("ResourceIDMultipleHrefs", func(t *testing.T) {
			body := `<multistatus xmlns="DAV:"><response><href>/root/</href><propstat><prop>` +
				`<resource-id><href>urn:uuid:root</href><href>urn:uuid:other</href></resource-id>` +
				`<resourcetype><collection/></resourcetype>` +
				`</prop><status>HTTP/1.1 200 OK</status></propstat></response></multistatus>`
			client, srv := newBindingTestClient(t, "https://dav.example/root/", map[string]*bindingTestReply{
				"https://dav.example/root/": {body: body},
			})
			g, err := client.DiscoverBindings(bindingTestContext(), "")
			checkBindingFailure(c, t, g, err, srv, 1)
		})

		t.Run("ResourceIDNotAbsolute", func(t *testing.T) {
			file := bindingTestMember{href: "/root/a.txt", id: "not-a-uri"}
			client, srv := newBindingTestClient(t, "https://dav.example/root/", map[string]*bindingTestReply{
				"https://dav.example/root/": {
					body: bindingTestMultistatus(root, []bindingTestMember{file}),
				},
			})
			g, err := client.DiscoverBindings(bindingTestContext(), "")
			checkBindingFailure(c, t, g, err, srv, 1)
		})

		t.Run("DuplicateResponseHref", func(t *testing.T) {
			body := `<multistatus xmlns="DAV:"><response><href>/root/</href><href>/root/a</href>` +
				`<propstat><prop><resource-id><href>urn:uuid:root</href></resource-id>` +
				`<resourcetype><collection/></resourcetype>` +
				`</prop><status>HTTP/1.1 200 OK</status></propstat></response></multistatus>`
			client, srv := newBindingTestClient(t, "https://dav.example/root/", map[string]*bindingTestReply{
				"https://dav.example/root/": {body: body},
			})
			g, err := client.DiscoverBindings(bindingTestContext(), "")
			checkBindingFailure(c, t, g, err, srv, 1)
		})
	})
}
