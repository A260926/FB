package soup

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"net/url"
	"strings"

	"golang.org/x/net/html"
)

// FramePage is one document in a tree of pages embedded with iframes. Document
// exposes the page's own DOM through the usual Root helpers (Find, Attrs,
// Text, ...), while Frames holds one entry per iframe in Document order,
// including frames that could not be expanded because they close a cycle.
type FramePage struct {
	// URL is the identity of the page: the final response URL once redirects
	// are resolved, about:blank for a frame without a usable source, or
	// about:srcdoc for a page taken from an iframe's srcdoc attribute.
	URL string
	// BaseURL is the base address used to resolve the page's relative URLs.
	BaseURL string
	// Document is the parsed DOM. Its Pointer is nil for a cyclic frame.
	Document Root
	// Frames contains the child frame occupying each iframe position.
	Frames []FramePage
	// Cycle reports that this frame points to one of its ancestors.
	Cycle bool
}

// maxFrameRedirects bounds the redirect chain followed for one page.
const maxFrameRedirects = 10

// frameAncestor is one entry of the current ancestor chain. Both the requested
// and final identities are kept so a cycle can be recognized before or after a
// redirect.
type frameAncestor struct {
	requested string
	final     string
}

// frameCollector carries the per-call state while walking the page tree.
// bodies holds complete response bodies keyed by every address known to serve
// them, and finals maps an address to the final address it resolved to. The
// cache lives for one call only and is never shared between calls.
type frameCollector struct {
	ctx    context.Context
	client *http.Client
	bodies map[string][]byte
	finals map[string]string
}

// CollectFramesWithClient fetches url and, following iframes, returns the page
// as a tree that keeps each embedded frame in its position. Requests use the
// package Headers and Cookies and the supplied context; a nil client uses the
// package default client. Only HTTP(S) pages are fetched. Every page in the
// returned tree was collected successfully; otherwise a nil tree and an error
// naming the failing address and embedding path (for example
// frames[0].frames[1]) is returned.
func CollectFramesWithClient(ctx context.Context, url string, client *http.Client) (*FramePage, error) {
	if client == nil {
		client = defaultClient
	}
	if ctx == nil {
		ctx = context.Background()
	}
	c := &frameCollector{
		ctx:    ctx,
		client: redirectlessClient(client),
		bodies: make(map[string][]byte),
		finals: make(map[string]string),
	}
	page, err := c.collectRoot(url)
	if err != nil {
		return nil, fmt.Errorf("collect %s: %w", url, err)
	}
	return page, nil
}

// redirectlessClient returns a shallow copy of client that hands redirect
// responses back instead of following them, so the collector can consult its
// download cache and the ancestor chain before each hop. Transport, Jar,
// Timeout and the other fields are preserved.
func redirectlessClient(client *http.Client) *http.Client {
	clone := *client
	clone.CheckRedirect = func(*http.Request, []*http.Request) error {
		return http.ErrUseLastResponse
	}
	return &clone
}

// collectRoot collects the initially requested page and its frame tree.
func (c *frameCollector) collectRoot(address string) (*FramePage, error) {
	parsed, err := parseHTTPURL(address)
	if err != nil {
		return nil, &frameError{addr: address, err: err}
	}
	body, final, cycle, err := c.fetch(parsed.String(), nil)
	if err != nil {
		return nil, err
	}
	if cycle != nil {
		// An ancestor list for the root is empty, so this cannot happen.
		return nil, &frameError{addr: parsed.String(), err: errors.New("unexpected cycle at root")}
	}
	page := &FramePage{}
	if err := c.fillPage(page, body, nil, frameAncestor{requested: parsed.String(), final: final}, ""); err != nil {
		return nil, err
	}
	return page, nil
}

// fetch returns the body and final address for requested, downloading each
// address at most once per call. Redirects are followed manually so a hop can
// reuse a cached body or stop at an ancestor without issuing a request. When
// the chain reaches an ancestor, the third result names it and no body is
// returned. ancestors is the chain above the position being collected.
func (c *frameCollector) fetch(requested string, ancestors []frameAncestor) ([]byte, string, *frameAncestor, error) {
	if body, final, ok := c.cachedBody(requested); ok {
		if ancestor, hit := cycleAncestor(ancestors, requested, final); hit {
			return nil, "", &ancestor, nil
		}
		return body, final, nil, nil
	}
	current := requested
	var redirects []string
	seen := make(map[string]bool)
	for {
		if body, final, ok := c.cachedBody(current); ok {
			if ancestor, hit := cycleAncestor(ancestors, current, final); hit {
				c.recordFinal(redirects, final)
				return nil, "", &ancestor, nil
			}
			c.recordFinal(redirects, final)
			return body, final, nil, nil
		}
		// An address already known to be an ancestor is never requested.
		if ancestor, hit := cycleAncestor(ancestors, current, current); hit {
			c.recordFinal(redirects, ancestor.final)
			return nil, "", &ancestor, nil
		}
		if seen[current] {
			return nil, "", nil, &frameError{addr: current, err: errors.New("redirect loop")}
		}
		if len(redirects) >= maxFrameRedirects {
			return nil, "", nil, &frameError{addr: current, err: errors.New("too many redirects")}
		}
		seen[current] = true
		if err := c.ctx.Err(); err != nil {
			return nil, "", nil, &frameError{addr: current, err: err}
		}
		resp, body, err := getBody(c.ctx, current, c.client)
		if err != nil {
			return nil, "", nil, &frameError{addr: current, err: err}
		}
		if isRedirect(resp.StatusCode) && resp.Header.Get("Location") != "" {
			next, err := resolveHTTP(current, resp.Header.Get("Location"))
			if err != nil {
				return nil, "", nil, &frameError{addr: current, err: fmt.Errorf("invalid redirect location: %w", err)}
			}
			redirects = append(redirects, current)
			current = next
			continue
		}
		if resp.StatusCode < 200 || resp.StatusCode >= 300 {
			return nil, "", nil, &frameError{addr: current, err: fmt.Errorf("unexpected HTTP status %d", resp.StatusCode)}
		}
		final := current
		if resp.Request != nil && resp.Request.URL != nil {
			final = stripFragment(resp.Request.URL.String())
		}
		if final != current {
			// A transport may follow redirects on its own. If it landed on an
			// ancestor, report a cycle instead of replacing that ancestor.
			if ancestor, hit := cycleAncestor(ancestors, current, final); hit {
				c.recordFinal(redirects, final)
				return nil, "", &ancestor, nil
			}
		}
		c.bodies[current] = body
		c.finals[current] = final
		if final != current {
			if _, exists := c.bodies[final]; !exists {
				c.bodies[final] = body
			}
			c.finals[final] = final
		}
		c.recordFinal(redirects, final)
		return body, final, nil, nil
	}
}

// cachedBody returns a body already downloaded for addr, either directly or
// through an address already known to resolve to it.
func (c *frameCollector) cachedBody(addr string) ([]byte, string, bool) {
	if body, ok := c.bodies[addr]; ok {
		return body, c.finals[addr], true
	}
	if final, ok := c.finals[addr]; ok {
		if body, ok := c.bodies[final]; ok {
			return body, final, true
		}
	}
	return nil, "", false
}

// recordFinal remembers that every redirecting address resolves to final.
func (c *frameCollector) recordFinal(redirects []string, final string) {
	for _, hop := range redirects {
		c.finals[hop] = final
	}
}

func isRedirect(status int) bool {
	switch status {
	case http.StatusMovedPermanently, http.StatusFound, http.StatusSeeOther,
		http.StatusTemporaryRedirect, http.StatusPermanentRedirect:
		return true
	}
	return false
}

// fillPage parses a downloaded body into page, sets its bases, and expands its
// frames. ancestors is the chain above this page; identity is this page's own
// chain entry and path is its embedding path.
func (c *frameCollector) fillPage(page *FramePage, body []byte, ancestors []frameAncestor, identity frameAncestor, path string) error {
	doc := parseDocument(body)
	page.URL = identity.final
	page.Document = rootOf(doc)
	page.BaseURL = baseAddress(doc, page.URL)
	chain := make([]frameAncestor, 0, len(ancestors)+1)
	chain = append(chain, ancestors...)
	chain = append(chain, identity)
	return c.expand(page, doc, chain, path)
}

// expand walks doc in document order, replacing every iframe with a collected
// child page. ancestors includes the current document as its last entry; path
// is the embedding path of the current document.
func (c *frameCollector) expand(page *FramePage, doc *html.Node, ancestors []frameAncestor, path string) error {
	var iframes []*html.Node
	collectIframes(doc, &iframes)
	for i, node := range iframes {
		// Cancellation is observed before each frame position, even when the
		// frame needs no request.
		if err := c.ctx.Err(); err != nil {
			return &frameError{path: childPath(path, i), err: err}
		}
		child, err := c.collectFrame(page, node, childPath(path, i), ancestors)
		if err != nil {
			return err
		}
		page.Frames = append(page.Frames, *child)
	}
	return nil
}

// collectFrame builds the page occupying one iframe position. path is the
// embedding path used in error messages; ancestors is the chain of pages above
// this position.
func (c *frameCollector) collectFrame(parent *FramePage, node *html.Node, path string, ancestors []frameAncestor) (*FramePage, error) {
	attrs := attributesOf(node)
	// srcdoc takes precedence over src even when empty.
	if srcdoc, ok := attrs["srcdoc"]; ok {
		return c.srcdocFrame(srcdoc, parent, path, ancestors)
	}
	src := strings.TrimSpace(attrs["src"])
	if src == "" || isAboutBlank(src) {
		return blankFrame(parent), nil
	}
	requested, err := resolveHTTP(parent.BaseURL, src)
	if err != nil {
		return nil, &frameError{path: path, addr: src, err: fmt.Errorf("invalid frame address: %w", err)}
	}
	body, final, cycleHit, err := c.fetch(requested, ancestors)
	if err != nil {
		return nil, prefixPath(path, err)
	}
	if cycleHit != nil {
		return cyclicFrame(cycleHit.final), nil
	}
	child := &FramePage{}
	if err := c.fillPage(child, body, ancestors, frameAncestor{requested: requested, final: final}, path); err != nil {
		return nil, err
	}
	return child, nil
}

// srcdocFrame builds a page whose source is an iframe's srcdoc attribute. The
// HTML parser already decoded the attribute value, so it is parsed as is.
// Every srcdoc is expanded on its own: identical text under different parent
// bases resolves its relative addresses differently.
func (c *frameCollector) srcdocFrame(srcdoc string, parent *FramePage, path string, ancestors []frameAncestor) (*FramePage, error) {
	page := &FramePage{
		URL:     "about:srcdoc",
		BaseURL: parent.BaseURL,
	}
	doc := parseDocument([]byte(srcdoc))
	page.Document = rootOf(doc)
	page.BaseURL = baseAddress(doc, page.BaseURL)
	if err := c.expand(page, doc, append(ancestors, frameAncestor{requested: page.URL, final: page.URL}), path); err != nil {
		return nil, err
	}
	return page, nil
}

// blankFrame returns an empty page for a frame without a usable source.
func blankFrame(parent *FramePage) *FramePage {
	return &FramePage{
		URL:      "about:blank",
		BaseURL:  parent.BaseURL,
		Document: HTMLParse(""),
	}
}

// cyclicFrame returns the placeholder for an edge back to an ancestor. It
// still occupies its position in Frames but has no document and no children.
func cyclicFrame(final string) *FramePage {
	return &FramePage{
		URL:     final,
		BaseURL: final,
		Cycle:   true,
	}
}

// cycleAncestor reports the nearest ancestor matching either identity of the
// page being collected.
func cycleAncestor(ancestors []frameAncestor, requested, final string) (frameAncestor, bool) {
	for i := len(ancestors) - 1; i >= 0; i-- {
		a := ancestors[i]
		if a.requested == requested || a.final == final || a.final == requested || a.requested == final {
			return a, true
		}
	}
	return frameAncestor{}, false
}

// isAboutBlank reports whether src names the about:blank document, ignoring
// case, surrounding whitespace and any fragment.
func isAboutBlank(src string) bool {
	u, err := url.Parse(src)
	if err != nil {
		return false
	}
	if !strings.EqualFold(u.Scheme, "about") {
		return false
	}
	target := u.Opaque
	if target == "" {
		target = strings.TrimPrefix(u.Path, "/")
	}
	return strings.EqualFold(target, "blank")
}

// childPath builds the embedding path of the index-th iframe.
func childPath(parent string, index int) string {
	elem := fmt.Sprintf("frames[%d]", index)
	if parent == "" {
		return elem
	}
	return parent + "." + elem
}

// parseHTTPURL validates that raw is an absolute HTTP(S) address and returns
// it without a fragment. Fragments take no part in requests or identities.
func parseHTTPURL(raw string) (*url.URL, error) {
	u, err := url.Parse(strings.TrimSpace(raw))
	if err != nil {
		return nil, err
	}
	return httpURLWithoutFragment(u)
}

func httpURLWithoutFragment(u *url.URL) (*url.URL, error) {
	switch strings.ToLower(u.Scheme) {
	case "http", "https":
	default:
		return nil, fmt.Errorf("unsupported URL scheme %q", u.Scheme)
	}
	if u.Host == "" {
		return nil, errors.New("URL has no host")
	}
	withoutFragment := *u
	withoutFragment.Fragment = ""
	withoutFragment.RawFragment = ""
	return &withoutFragment, nil
}

// resolveHTTP resolves reference against base, requiring an HTTP(S) result.
func resolveHTTP(base, reference string) (string, error) {
	b, err := url.Parse(base)
	if err != nil {
		return "", err
	}
	ref, err := url.Parse(reference)
	if err != nil {
		return "", err
	}
	resolved, err := httpURLWithoutFragment(b.ResolveReference(ref))
	if err != nil {
		return "", err
	}
	return resolved.String(), nil
}

// stripFragment removes any fragment from raw when it parses as a URL.
func stripFragment(raw string) string {
	u, err := url.Parse(raw)
	if err != nil {
		return raw
	}
	u.Fragment = ""
	u.RawFragment = ""
	return u.String()
}

// baseAddress returns fallback unless doc contains a base[href] that resolves
// to an HTTP(S) address against it. Only the first such base counts; a base
// whose href is not an HTTP(S) address after resolution is skipped.
func baseAddress(doc *html.Node, fallback string) string {
	if resolved, ok := firstValidBase(doc, fallback); ok {
		return resolved
	}
	return fallback
}

// parseDocument parses HTML into a DOM. Entities in markup are decoded by the
// parser itself and must not be decoded again.
func parseDocument(body []byte) *html.Node {
	doc, err := html.Parse(strings.NewReader(string(body)))
	if err != nil {
		return HTMLParse("").Pointer
	}
	return doc
}

// rootOf turns a parsed document into a Root pointing at its root element,
// using the same traversal as HTMLParse.
func rootOf(doc *html.Node) Root {
	n := doc
	for n != nil {
		switch n.Type {
		case html.ElementNode:
			return Root{Pointer: n, NodeValue: n.Data}
		case html.DocumentNode:
			n = n.FirstChild
		case html.DoctypeNode, html.CommentNode:
			n = n.NextSibling
		default:
			n = n.NextSibling
		}
	}
	return Root{Error: newError(ErrUnableToParse, "unable to parse the HTML")}
}

// collectIframes appends the iframes below n in document order. It does not
// descend into template subtrees, whose content is inert, and it stops at an
// iframe, treating its children as fallback content rather than markup.
func collectIframes(n *html.Node, out *[]*html.Node) {
	for child := n.FirstChild; child != nil; child = child.NextSibling {
		if child.Type == html.ElementNode {
			if child.Data == "iframe" {
				*out = append(*out, child)
				continue
			}
			if child.Data == "template" {
				continue
			}
		}
		collectIframes(child, out)
	}
}

// firstValidBase returns the resolved address of the first base[href] in
// document order that resolves to an HTTP(S) address against fallback. Bases
// inside templates do not count and later bases never replace the first valid
// one.
func firstValidBase(doc *html.Node, fallback string) (string, bool) {
	var resolved string
	found := false
	var walk func(*html.Node)
	walk = func(n *html.Node) {
		if found {
			return
		}
		for child := n.FirstChild; child != nil; child = child.NextSibling {
			if child.Type != html.ElementNode {
				continue
			}
			if child.Data == "template" {
				continue
			}
			if child.Data == "base" {
				if href, ok := attributesOf(child)["href"]; ok {
					if base, err := resolveHTTP(fallback, href); err == nil {
						resolved, found = base, true
						return
					}
				}
			}
			walk(child)
		}
	}
	walk(doc)
	return resolved, found
}

// attributesOf returns the first value seen for each attribute name.
func attributesOf(n *html.Node) map[string]string {
	attrs := make(map[string]string, len(n.Attr))
	for _, attr := range n.Attr {
		if _, exists := attrs[attr.Key]; !exists {
			attrs[attr.Key] = attr.Val
		}
	}
	return attrs
}

// frameError records the embedding path and address that led to a failure
// while preserving the underlying error for errors.Is and errors.As.
type frameError struct {
	path string
	addr string
	err  error
}

func (e *frameError) Error() string {
	msg := e.err.Error()
	if e.addr != "" {
		msg = e.addr + ": " + msg
	}
	if e.path != "" {
		msg = e.path + ": " + msg
	}
	return msg
}

func (e *frameError) Unwrap() error { return e.err }

// prefixPath attaches an embedding path to an error that does not have one.
func prefixPath(path string, err error) error {
	var fe *frameError
	if errors.As(err, &fe) && fe.path == "" {
		return &frameError{path: path, addr: fe.addr, err: fe.err}
	}
	return err
}
