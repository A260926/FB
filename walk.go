package gofeed

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
)

// ErrPaginationCycle is returned by Parser.WalkURL when the next-page chain
// points back at a page that the same walk has already requested or reached
// through a redirect. The walk does not request or parse the repeating page
// again. The seen-set is per walk: the next WalkURL call starts fresh.
var ErrPaginationCycle = errors.New("gofeed: pagination cycle detected")

// WalkOptions configures a Parser.WalkURL run.
type WalkOptions struct {
	// Incremental switches the walk from a full archive crawl to an update
	// fetch: it stops at the first item whose non-empty GUID is present in
	// KnownIDs, delivering neither that item nor anything after it. When
	// false, KnownIDs is ignored and every page in the chain is walked.
	Incremental bool
	// KnownIDs holds GUIDs collected by earlier runs. It is only consulted in
	// incremental mode and is never modified by the walk. GUIDs are treated as
	// opaque strings: they are not trimmed or turned into URLs.
	KnownIDs map[string]struct{}
}

// WalkReport summarizes a completed or interrupted Parser.WalkURL run.
type WalkReport struct {
	// Pages counts pages whose response body was parsed successfully and
	// closed. A page that fails to fetch or parse is not counted, even when
	// items from earlier pages were already delivered.
	Pages int
	// Items counts items accepted by the emit callback.
	Items int
	// StoppedAt records the GUID that ended an incremental walk. It stays
	// empty when the chain ends naturally or the walk stops for another
	// reason (callback error, cycle, transport failure).
	StoppedAt string
}

// WalkURL fetches a feed and follows its NextPage chain, emitting items one at
// a time in the order each page gives them. Pages are fetched lazily: the next
// page is not requested until the current page has been parsed, emitted, and
// closed. It reuses the parser's format detection, translators and HTTP
// configuration (UserAgent, Client, AuthConfig, MaxByteSize); no conditional
// caching or automatic retry is applied.
//
// NextPage comes from the translated Feed, so a custom Translator can set or
// clear it and the walk never reaches around it to the source feed. A relative
// NextPage is resolved against the final URL of the page's response
// (resp.Request.URL), falling back to the requested URL when that is absent;
// the feed's self or home_page_url values are never used as the base. Only
// http and https continuations are accepted. Page identity for cycle
// detection drops the fragment but keeps query parameters, including their
// order.
//
// In incremental mode, the first item with a non-empty GUID in KnownIDs stops
// the walk and its GUID is reported in StoppedAt. A duplicate non-empty GUID
// already delivered earlier in the same walk is skipped instead of treated as
// the boundary; items with empty GUIDs are always delivered. Full mode
// ignores KnownIDs. The per-walk duplicate set and cycle set are discarded
// when the call returns; KnownIDs is left untouched.
//
// A callback error is returned unchanged with the counts accumulated so far.
// Items already emitted belong to the caller: later pages build new Item
// values and never mutate earlier ones.
func (f *Parser) WalkURL(ctx context.Context, start string, opts WalkOptions, emit func(*Item) error) (report WalkReport, err error) {
	if ctx == nil {
		return report, errors.New("gofeed: WalkURL requires a non-nil context")
	}
	if emit == nil {
		return report, errors.New("gofeed: WalkURL requires a non-nil emit callback")
	}
	startURL, err := url.Parse(start)
	if err != nil {
		return report, err
	}
	if !isHTTPURL(startURL) {
		return report, fmt.Errorf("gofeed: unsupported feed URL scheme %q", startURL.Scheme)
	}

	// seen holds page identities (fragment stripped) requested or finally
	// reached during this walk. It is deliberately local to the call.
	seen := make(map[string]struct{})
	delivered := make(map[string]struct{})
	pageURL := start

	for {
		requestID := pageIdentity(pageURL)
		if _, ok := seen[requestID]; ok {
			// The chain looped back to a page already requested: do not even
			// issue the request.
			return report, ErrPaginationCycle
		}
		seen[requestID] = struct{}{}

		resp, err := f.doPageRequest(ctx, pageURL)
		if err != nil {
			return report, err
		}

		// Redirects can make a fresh alias land on a page already visited.
		// Identify the final destination before parsing, so the repeated page
		// is neither parsed nor delivered. The response has been received, so
		// its body still has to be closed.
		finalURL := pageURL
		if resp.Request != nil && resp.Request.URL != nil {
			finalURL = resp.Request.URL.String()
		}
		finalID := pageIdentity(finalURL)
		if finalID != requestID {
			if _, ok := seen[finalID]; ok {
				_ = resp.Body.Close()
				return report, ErrPaginationCycle
			}
			seen[finalID] = struct{}{}
		}

		feed, err := f.parsePageResponse(resp)
		if err != nil {
			return report, err
		}
		report.Pages++

		for _, item := range feed.Items {
			guid := item.GUID
			if opts.Incremental && guid != "" {
				if _, known := opts.KnownIDs[guid]; known {
					// Historical boundary: this item and everything after it
					// stay undelivered, and the next page is never requested.
					report.StoppedAt = guid
					return report, nil
				}
			}
			if guid != "" {
				if _, dup := delivered[guid]; dup {
					// A repeat inside the walk is just a duplicate, never a
					// boundary: skip it but keep paging.
					continue
				}
				delivered[guid] = struct{}{}
			}
			if err := emit(item); err != nil {
				return report, err
			}
			report.Items++
		}

		if feed.NextPage == "" {
			return report, nil
		}

		pageURL, err = resolveNextPage(finalURL, feed.NextPage)
		if err != nil {
			return report, err
		}
	}
}

// doPageRequest builds and sends one authenticated GET for a feed page,
// applying the parser's User-Agent and Basic Auth settings. The caller owns
// resp.Body. It is shared by ParseURLWithContext and WalkURL so both use the
// same request construction and HTTP client.
func (f *Parser) doPageRequest(ctx context.Context, pageURL string) (*http.Response, error) {
	req, err := http.NewRequestWithContext(ctx, "GET", pageURL, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("User-Agent", f.UserAgent)

	if f.AuthConfig != nil && f.AuthConfig.Username != "" && f.AuthConfig.Password != "" {
		req.SetBasicAuth(f.AuthConfig.Username, f.AuthConfig.Password)
	}

	return f.httpClient().Do(req)
}

// parsePageResponse checks the response status, parses and translates the body
// through the normal detection/translator path (subject to MaxByteSize), and
// always closes the body before returning. A close error wins when the parse
// itself succeeded, matching ParseURLWithContext's error precedence.
func (f *Parser) parsePageResponse(resp *http.Response) (feed *Feed, err error) {
	defer func() {
		if ce := resp.Body.Close(); ce != nil && err == nil {
			err = ce
		}
	}()

	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return nil, HTTPError{
			StatusCode: resp.StatusCode,
			Status:     resp.Status,
		}
	}

	if f.MaxByteSize <= 0 {
		return f.Parse(resp.Body)
	}

	body := &limitedReader{r: resp.Body, left: f.MaxByteSize}
	feed, err = f.Parse(body)
	if body.err == ErrResponseTooLarge {
		return nil, ErrResponseTooLarge
	}
	if err != nil {
		return nil, err
	}
	// XML parsers stop at the root's closing tag. Drain the remainder through
	// the same limit before treating the page as successfully read.
	if _, err = io.Copy(io.Discard, body); err != nil {
		return nil, err
	}
	return feed, nil
}

// resolveNextPage resolves a translated NextPage value against the page's
// final response URL. The feed's self or home page URL is deliberately not
// used. Only http/https results are accepted.
func resolveNextPage(finalURL, next string) (string, error) {
	base, err := url.Parse(finalURL)
	if err != nil {
		return "", err
	}
	ref, err := url.Parse(next)
	if err != nil {
		return "", err
	}
	resolved := base.ResolveReference(ref)
	if !isHTTPURL(resolved) {
		return "", fmt.Errorf("gofeed: unsupported next page scheme %q", resolved.Scheme)
	}
	return resolved.String(), nil
}

// pageIdentity normalizes a URL for cycle comparison: fragments are dropped
// because they are not sent to servers, while the query string (including
// parameter order) and everything else are preserved.
func pageIdentity(raw string) string {
	u, err := url.Parse(raw)
	if err != nil {
		return raw
	}
	u.Fragment = ""
	return u.String()
}

func isHTTPURL(u *url.URL) bool {
	return u.Scheme == "http" || u.Scheme == "https"
}
