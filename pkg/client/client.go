package client

import (
	"fmt"
	"net/http"
	"net/url"
	"strings"
	"time"

	"github.com/robert-malhotra/go-stac-client/pkg/stac"
)

// Version is the library version, surfaced in the default User-Agent.
const Version = "1.0.0"

// Defaults applied when constructing a Client.
const (
	defaultHTTPTimeout    = 30 * time.Second
	defaultMaxBodyBytes   = 64 * 1024 * 1024 // 64 MiB
	defaultMaxPages       = 10000
	defaultMaxErrorBytes  = 16 * 1024
	defaultMaxResponseLog = 1024
)

// MaxErrorBodyBytes caps how many bytes of an error response body are
// captured into HTTPError.Body. Exposed for tests and override.
var MaxErrorBodyBytes int64 = defaultMaxErrorBytes

// Middleware manipulates an outgoing *http.Request before it is executed.
// The context is provided for cancellation and to support auth implementations
// that may need to perform async operations such as token refresh.
type Middleware func(*http.Request) error

// NextHandler determines the next-page URL from a list of STAC links.
// Return nil if there's no next page, or an error if parsing fails.
//
// NextHandler is consulted by DefaultNextHandler-aware decoders. The built-in
// LinkDecoder honors it when set on the Client; pass WithNextHandler to override.
type NextHandler func([]*stac.Link) (*url.URL, error)

// ClientOption configures the Client.
type ClientOption func(*Client)

// Client represents a STAC API client.
//
// A Client is safe for concurrent use after construction. Applying additional
// ClientOptions to a Client after it has been used by another goroutine is
// not safe.
type Client struct {
	baseURL       *url.URL
	httpClient    *http.Client
	timeout       time.Duration
	nextHandler   NextHandler
	middleware    []Middleware
	userAgent     string
	maxPages      int
	maxBodyBytes  int64
	allowedHosts  map[string]struct{} // optional allowlist for paginated next URLs
	allowAllHosts bool
}

// WithHTTPClient sets a custom HTTP client. The supplied client is used as-is;
// the STAC client never mutates it.
func WithHTTPClient(client *http.Client) ClientOption {
	return func(c *Client) {
		if client != nil {
			c.httpClient = client
		}
	}
}

// WithTimeout sets the per-request HTTP timeout. The timeout is applied via
// context.WithTimeout on each request and never mutates the underlying
// http.Client. A zero value disables the per-request timeout.
func WithTimeout(d time.Duration) ClientOption {
	return func(c *Client) { c.timeout = d }
}

// WithNextHandler configures a custom NextHandler used when the built-in
// LinkDecoder finds no rel="next" link via the default lookup. Most callers
// will not need this; it exists to support non-standard pagination link rels.
func WithNextHandler(h NextHandler) ClientOption {
	return func(c *Client) { c.nextHandler = h }
}

// WithMiddleware registers one or more request-middleware functions. They run
// in the order registered, before the request is dispatched.
func WithMiddleware(mw ...Middleware) ClientOption {
	return func(c *Client) { c.middleware = append(c.middleware, mw...) }
}

// WithUserAgent overrides the default User-Agent.
func WithUserAgent(ua string) ClientOption {
	return func(c *Client) { c.userAgent = ua }
}

// WithMaxPages caps the number of pages an iterator will fetch. Zero or
// negative disables the limit (not recommended for untrusted servers).
func WithMaxPages(n int) ClientOption {
	return func(c *Client) { c.maxPages = n }
}

// WithMaxBodyBytes caps the size of any single response body decoded by the
// client. Bodies larger than this return ErrResponseTooLarge.
// Zero or negative disables the limit.
func WithMaxBodyBytes(n int64) ClientOption {
	return func(c *Client) { c.maxBodyBytes = n }
}

// WithAllowedHosts restricts the hosts the client will fetch from when
// following pagination next-links. The base URL host is always allowed.
//
// Without this option, the client follows any host the server returns.
// Use it to mitigate SSRF or credential-leak risks across hosts.
func WithAllowedHosts(hosts ...string) ClientOption {
	return func(c *Client) {
		if c.allowedHosts == nil {
			c.allowedHosts = map[string]struct{}{}
		}
		for _, h := range hosts {
			c.allowedHosts[strings.ToLower(h)] = struct{}{}
		}
	}
}

// WithBearerToken installs a middleware that sends the given bearer token in
// the Authorization header on requests to the base URL's origin (scheme+host+port).
// Requests to other origins (e.g., a CDN-fronted next link) do not receive the token.
func WithBearerToken(token string) ClientOption {
	return func(c *Client) {
		base := originOf(c.baseURL)
		c.middleware = append(c.middleware, func(req *http.Request) error {
			if originOf(req.URL) == base {
				req.Header.Set("Authorization", "Bearer "+token)
			}
			return nil
		})
	}
}

// WithAPIKey installs a middleware that sends the given header on requests to
// the base URL's origin. Requests to other origins do not receive the header.
func WithAPIKey(header, value string) ClientOption {
	return func(c *Client) {
		if header == "" {
			return
		}
		base := originOf(c.baseURL)
		c.middleware = append(c.middleware, func(req *http.Request) error {
			if originOf(req.URL) == base {
				req.Header.Set(header, value)
			}
			return nil
		})
	}
}

// originOf returns scheme://host[:port] in lowercase, with default ports
// elided so https://example.com and https://example.com:443 compare equal.
func originOf(u *url.URL) string {
	if u == nil {
		return ""
	}
	scheme := strings.ToLower(u.Scheme)
	host := strings.ToLower(u.Host)
	switch {
	case scheme == "http" && strings.HasSuffix(host, ":80"):
		host = strings.TrimSuffix(host, ":80")
	case scheme == "https" && strings.HasSuffix(host, ":443"):
		host = strings.TrimSuffix(host, ":443")
	}
	return scheme + "://" + host
}

// NewClient creates a new STAC client rooted at baseURL.
//
// baseURL must be an absolute http or https URL. A trailing slash is added to
// its path if missing so that relative paths ("collections", "search", …)
// resolve under the API root.
func NewClient(baseURL string, opts ...ClientOption) (*Client, error) {
	if baseURL == "" {
		return nil, fmt.Errorf("stac: baseURL is required")
	}
	u, err := url.Parse(baseURL)
	if err != nil {
		return nil, fmt.Errorf("stac: invalid baseURL %q: %w", baseURL, err)
	}
	if !u.IsAbs() {
		return nil, fmt.Errorf("stac: baseURL must be absolute, got %q", baseURL)
	}
	if u.Scheme != "http" && u.Scheme != "https" {
		return nil, fmt.Errorf("stac: baseURL scheme must be http or https, got %q", u.Scheme)
	}
	if u.Path == "" {
		u.Path = "/"
	}
	if !strings.HasSuffix(u.Path, "/") {
		u.Path += "/"
	}
	if u.RawPath != "" && !strings.HasSuffix(u.RawPath, "/") {
		u.RawPath += "/"
	}

	c := &Client{
		baseURL:      u,
		httpClient:   &http.Client{},
		timeout:      defaultHTTPTimeout,
		nextHandler:  DefaultNextHandler,
		userAgent:    fmt.Sprintf("go-stac-client/%s (+https://github.com/robert-malhotra/go-stac-client)", Version),
		maxPages:     defaultMaxPages,
		maxBodyBytes: defaultMaxBodyBytes,
	}
	for _, o := range opts {
		o(c)
	}
	return c, nil
}

// BaseURL returns a copy of the client's base URL.
func (c *Client) BaseURL() *url.URL {
	u := *c.baseURL
	return &u
}

// hostAllowed reports whether the given URL host is permitted as a next-page
// destination. The base URL host is always allowed; matching is by Host
// (host:port) so that two services on different ports of the same machine
// are correctly distinguished.
func (c *Client) hostAllowed(u *url.URL) bool {
	if u == nil {
		return false
	}
	if c.allowedHosts == nil {
		return true
	}
	host := strings.ToLower(u.Host)
	if host == strings.ToLower(c.baseURL.Host) {
		return true
	}
	_, ok := c.allowedHosts[host]
	return ok
}

// DefaultNextHandler looks for the first link with rel="next" and returns its
// Href parsed as a URL. The returned URL may be relative or absolute.
func DefaultNextHandler(links []*stac.Link) (*url.URL, error) {
	nl := findLinkByRel(links, "next")
	if nl == nil {
		return nil, nil
	}
	if nl.Href == "" {
		return nil, fmt.Errorf("stac: 'next' link has empty href")
	}
	parsed, err := url.Parse(nl.Href)
	if err != nil {
		return nil, fmt.Errorf("stac: invalid 'next' link URL %q: %w", nl.Href, err)
	}
	return parsed, nil
}

func findLinkByRel(links []*stac.Link, rel string) *stac.Link {
	for _, l := range links {
		if l != nil && l.Rel == rel {
			return l
		}
	}
	return nil
}
