package gocrawl

import (
	"bytes"
	"net/url"
	"strings"

	"github.com/PuerkitoBio/purell"
)

const (
	robotsTxtPath = "/robots.txt"
)

// U is a convenience type definition, it is a map[*url.URL]interface{}
// that can be used to enqueue URLs with state information.
type U map[*url.URL]interface{}

// S is a convenience type definition, it is a map[string]interface{}
// that can be used to enqueue URLs (the string) with state information.
type S map[string]interface{}

// URLContext contains all information related to an URL to process.
type URLContext struct {
	HeadBeforeGet bool
	State         interface{}

	// Internal fields, available through getters
	url                 *url.URL
	normalizedURL       *url.URL
	sourceURL           *url.URL
	normalizedSourceURL *url.URL
	isSitemap           bool
}

// URL returns the URL.
func (uc *URLContext) URL() *url.URL {
	return uc.url
}

// NormalizedURL returns the normalized URL (using Options.URLNormalizationFlags)
// of the URL.
func (uc *URLContext) NormalizedURL() *url.URL {
	return uc.normalizedURL
}

// SourceURL returns the source URL, if any (the URL that enqueued this
// URL).
func (uc *URLContext) SourceURL() *url.URL {
	return uc.sourceURL
}

// NormalizedSourceURL returns the normalized form of the source URL,
// if any (using Options.URLNormalizationFlags).
func (uc *URLContext) NormalizedSourceURL() *url.URL {
	return uc.normalizedSourceURL
}

// IsRobotsURL indicates if the URL is a robots.txt URL.
func (uc *URLContext) IsRobotsURL() bool {
	return isRobotsURL(uc.normalizedURL)
}

// IsSitemapURL indicates if the URL is a sitemap control request,
// discovered from a robots.txt Sitemap declaration or from a sitemap
// index. Such URLs are only enqueued when Options.DiscoverSitemaps
// is set.
func (uc *URLContext) IsSitemapURL() bool {
	return uc.isSitemap
}

// cloneForRedirect returns a new URLContext with the given
// destination URL with the same sourceURL and normalizedSourceURL.
func (uc *URLContext) cloneForRedirect(dst *url.URL, normFlags purell.NormalizationFlags) *URLContext {
	var src, normalizedSrc *url.URL
	if uc.sourceURL != nil {
		src = &url.URL{}
		*src = *uc.sourceURL
	}
	if src == nil && uc.url != nil {
		// if the current context doesn't have a source URL, use its URL as
		// source (e.g. for a seed URL that triggers a redirect)
		src = &url.URL{}
		*src = *uc.url
	}

	if uc.normalizedSourceURL != nil {
		normalizedSrc = &url.URL{}
		*normalizedSrc = *uc.normalizedSourceURL
	}
	if normalizedSrc == nil {
		normalizedSrc = &url.URL{}
		*normalizedSrc = *uc.normalizedURL
	}

	rawDst := &url.URL{}
	*rawDst = *dst
	purell.NormalizeURL(dst, normFlags)
	return &URLContext{
		HeadBeforeGet:       uc.HeadBeforeGet,
		State:               uc.State,
		url:                 rawDst,
		normalizedURL:       dst,
		sourceURL:           src,
		normalizedSourceURL: normalizedSrc,
		isSitemap:           uc.isSitemap,
	}
}

// Implement in a private func, because called from HttpClient also (without
// an URLContext).
func isRobotsURL(u *url.URL) bool {
	if u == nil {
		return false
	}
	return strings.ToLower(u.Path) == robotsTxtPath
}

func toStringArrayContextURL(list []*URLContext) string {
	var buf bytes.Buffer

	for _, item := range list {
		if buf.Len() > 0 {
			buf.WriteString(", ")
		}
		if nurl := item.NormalizedURL(); nurl != nil {
			buf.WriteString(nurl.String())
		}
	}

	return buf.String()
}

func (uc *URLContext) getRobotsURLCtx() (*URLContext, error) {
	robURL, err := uc.normalizedURL.Parse(robotsTxtPath)
	if err != nil {
		return nil, err
	}
	return &URLContext{
		HeadBeforeGet:       false, // Never request HEAD before GET for robots.txt
		State:               nil,   // Always nil state
		url:                 robURL,
		normalizedURL:       robURL, // Normalized is same as raw
		sourceURL:           uc.sourceURL,
		normalizedSourceURL: uc.normalizedSourceURL, // Source and normalized source is same as for current context
	}, nil
}

func (c *Crawler) toURLContexts(raw interface{}, src *url.URL) []*URLContext {
	var res []*URLContext

	mapString := func(v S) {
		res = make([]*URLContext, 0, len(v))
		for s, st := range v {
			ctx, err := c.stringToURLContext(s, src)
			if err != nil {
				c.Options.Extender.Error(newCrawlError(nil, err, CekParseURL))
				c.logFunc(LogError, "ERROR parsing URL %s", s)
			} else {
				ctx.State = st
				res = append(res, ctx)
			}
		}
	}

	mapURL := func(v U) {
		res = make([]*URLContext, 0, len(v))
		for u, st := range v {
			ctx := c.urlToURLContext(u, src)
			ctx.State = st
			res = append(res, ctx)
		}
	}

	switch v := raw.(type) {
	case *URLContext:
		res = []*URLContext{v}

	case []*URLContext:
		// Already-built contexts, as produced internally (e.g. sitemap
		// control requests and the URLs they discovered)
		res = v

	case string:
		// Convert a single string URL to an URLContext
		ctx, err := c.stringToURLContext(v, src)
		if err != nil {
			c.Options.Extender.Error(newCrawlError(nil, err, CekParseURL))
			c.logFunc(LogError, "ERROR parsing URL %s", v)
		} else {
			res = []*URLContext{ctx}
		}

	case []string:
		// Convert all strings to URLContexts
		res = make([]*URLContext, 0, len(v))
		for _, s := range v {
			ctx, err := c.stringToURLContext(s, src)
			if err != nil {
				c.Options.Extender.Error(newCrawlError(nil, err, CekParseURL))
				c.logFunc(LogError, "ERROR parsing URL %s", s)
			} else {
				res = append(res, ctx)
			}
		}

	case *url.URL:
		res = []*URLContext{c.urlToURLContext(v, src)}

	case []*url.URL:
		res = make([]*URLContext, 0, len(v))
		for _, u := range v {
			res = append(res, c.urlToURLContext(u, src))
		}

	case map[string]interface{}:
		mapString(S(v))

	case S:
		mapString(v)

	case map[*url.URL]interface{}:
		mapURL(U(v))

	case U:
		mapURL(v)

	default:
		if raw != nil {
			panic("unsupported URL type passed as empty interface")
		}
	}
	return res
}

func (c *Crawler) stringToURLContext(str string, src *url.URL) (*URLContext, error) {
	u, err := url.Parse(str)
	if err != nil {
		return nil, err
	}
	return c.urlToURLContext(u, src), nil
}

func (c *Crawler) urlToURLContext(u, src *url.URL) *URLContext {
	var rawSrc *url.URL

	rawU := *u
	purell.NormalizeURL(u, c.Options.URLNormalizationFlags)
	if src != nil {
		rawSrc = &url.URL{}
		*rawSrc = *src
		purell.NormalizeURL(src, c.Options.URLNormalizationFlags)
	}

	return &URLContext{
		HeadBeforeGet:       c.Options.HeadBeforeGet,
		State:               nil,
		url:                 &rawU,
		normalizedURL:       u,
		sourceURL:           rawSrc,
		normalizedSourceURL: src,
	}
}
