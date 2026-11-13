// Package robotstxt implements the robots.txt Exclusion Protocol
// as specified in http://www.robotstxt.org/wc/robots.html
// with various extensions.
package robotstxt

// Comments explaining the logic are taken from either the Google's spec:
// https://developers.google.com/webmasters/control-crawl-index/docs/robots_txt

import (
	"bytes"
	"errors"
	"io"
	"net/http"
	"regexp"
	"strconv"
	"strings"
	"time"
)

type RobotsData struct {
	Host     string
	Sitemaps []string

	// private
	allowAll    bool
	disallowAll bool
	groups      map[string]*Group
}

type Group struct {
	rules      []*rule
	Agent      string
	CrawlDelay time.Duration

	disallowAll bool
}

type rule struct {
	path    string
	allow   bool
	pattern *regexp.Regexp
}

type ParseError struct {
	Errs []error
}

func newParseError(errs []error) *ParseError {
	return &ParseError{errs}
}

func (e ParseError) Error() string {
	var b bytes.Buffer

	b.WriteString("Parse error(s): " + "\n")
	for _, er := range e.Errs {
		b.WriteString(er.Error() + "\n")
	}
	return b.String()
}

// Unwrap returns the underlying errors, compatible with errors.Is
// and errors.As from the standard library (Go 1.20+).
func (e ParseError) Unwrap() []error {
	return e.Errs
}

var allowAll = &RobotsData{allowAll: true}
var disallowAll = &RobotsData{disallowAll: true}
var emptyGroup = &Group{}
var emptyDisallowGroup = &Group{disallowAll: true}

func FromStatusAndBytes(statusCode int, body []byte) (*RobotsData, error) {
	switch {
	case statusCode >= 200 && statusCode < 300:
		return FromBytes(body)

	// From https://developers.google.com/webmasters/control-crawl-index/docs/robots_txt
	//
	// Google treats all 4xx errors in the same way and assumes that no valid
	// robots.txt file exists. It is assumed that there are no restrictions.
	// This is a "full allow" for crawling. Note: this includes 401
	// "Unauthorized" and 403 "Forbidden" HTTP result codes.
	case statusCode >= 400 && statusCode < 500:
		return allowAll, nil

	// From Google's spec:
	// Server errors (5xx) are seen as temporary errors that result in a "full
	// disallow" of crawling.
	case statusCode >= 500 && statusCode < 600:
		return disallowAll, nil
	}

	return nil, errors.New("Unexpected status: " + strconv.Itoa(statusCode))
}

func FromStatusAndString(statusCode int, body string) (*RobotsData, error) {
	return FromStatusAndBytes(statusCode, []byte(body))
}

// FromResponse parses robots data from an HTTP response. The response body
// is consumed through the streaming parser for successful (2xx) responses;
// bodies of 4xx/5xx and other unsupported statuses are still read to the
// end (or until a read error) but neither parsed nor fully buffered.
//
// FromResponse never closes res.Body: ownership of the body stays with the
// caller. A nil response yields (nil, nil). A body read failure is returned
// unchanged, so errors.Is can recover the original error, and takes
// precedence over any syntax error in the partially read body.
func FromResponse(res *http.Response) (*RobotsData, error) {
	if res == nil {
		// Edge case, if res is nil, return nil data
		return nil, nil
	}

	switch {
	case res.StatusCode >= 200 && res.StatusCode < 300:
		return FromReader(res.Body)

	// From https://developers.google.com/webmasters/control-crawl-index/docs/robots_txt
	//
	// Google treats all 4xx errors in the same way and assumes that no valid
	// robots.txt file exists. It is assumed that there are no restrictions.
	// This is a "full allow" for crawling. Note: this includes 401
	// "Unauthorized" and 403 "Forbidden" HTTP result codes.
	case res.StatusCode >= 400 && res.StatusCode < 500:
		if e := drainReader(res.Body); e != nil {
			return nil, e
		}
		return allowAll, nil

	// From Google's spec:
	// Server errors (5xx) are seen as temporary errors that result in a "full
	// disallow" of crawling.
	case res.StatusCode >= 500 && res.StatusCode < 600:
		if e := drainReader(res.Body); e != nil {
			return nil, e
		}
		return disallowAll, nil
	}

	// Unsupported status codes still require draining the body before the
	// status error is reported, preserving the historical read-error
	// precedence.
	if e := drainReader(res.Body); e != nil {
		return nil, e
	}
	return nil, errors.New("Unexpected status: " + strconv.Itoa(res.StatusCode))
}

// FromReader parses robots.txt data incrementally from r. Input is read and
// tokenized as a stream: the whole body is never buffered, and working
// memory grows only with the longest token, not with body size, comment
// length or the number of tokens seen.
//
// FromReader does not close r. A nil reader returns ErrNilReader. A
// non-EOF failure of r is returned unchanged (errors.Is can recover it)
// and takes precedence over any syntax error; the bytes delivered together
// with the failing Read are parsed first, but a truncated body can never be
// accepted as a complete policy.
func FromReader(r io.Reader) (*RobotsData, error) {
	if r == nil {
		return nil, ErrNilReader
	}
	return parseStream(r)
}

func FromBytes(body []byte) (r *RobotsData, err error) {
	return parseStream(bytes.NewReader(body))
}

func FromString(body string) (r *RobotsData, err error) {
	return FromBytes([]byte(body))
}

// parseStream is the shared streaming core used by FromReader, FromBytes
// and FromString. It drives the scanner and parser without materializing
// the token stream.
func parseStream(r io.Reader) (*RobotsData, error) {
	data, err, _ := parseStreamStats(r)
	return data, err
}

// parseStreamStats is parseStream plus the scanner memory high-water marks,
// used by the package tests to prove the streaming memory bounds.
func parseStreamStats(r io.Reader) (*RobotsData, error, *streamStats) {
	sc := newStreamScanner("bytes", true, r)
	sc.begin()

	p := newParser(sc, &sc.stats)
	groups, host, sitemaps, errs, readErr := p.parseAll()

	// A failure of the underlying Reader always wins: a truncated body must
	// never be treated as a complete policy, even if valid tokens arrived in
	// the same Read call.
	if readErr != nil {
		return nil, readErr, &sc.stats
	}
	if len(errs) > 0 {
		return nil, newParseError(errs), &sc.stats
	}

	// No tokens at all (empty body, whitespace/comments only) means an
	// unrestricted policy, matching the historical FromBytes shortcut.
	if sc.emittedTokens() == 0 {
		return allowAll, nil, &sc.stats
	}

	return &RobotsData{
		Host:     host,
		Sitemaps: sitemaps,
		groups:   groups,
	}, nil, &sc.stats
}

func (r *RobotsData) TestAgent(path, agent string) bool {
	if r.allowAll {
		return true
	}
	if r.disallowAll {
		return false
	}

	// Find a group of rules that applies to this agent
	// From Google's spec:
	// The user-agent is non-case-sensitive.
	g := r.FindGroup(agent)
	return g.Test(path)
}

// FindGroup searches block of declarations for specified user-agent.
// From Google's spec:
// Only one group of group-member records is valid for a particular crawler.
// The crawler must determine the correct group of records by finding the group
// with the most specific user-agent that still matches. All other groups of
// records are ignored by the crawler. The user-agent is non-case-sensitive.
// The order of the groups within the robots.txt file is irrelevant.
func (r *RobotsData) FindGroup(agent string) (ret *Group) {
	var prefixLen int

	agent = strings.ToLower(agent)
	if ret = r.groups["*"]; ret != nil {
		// Weakest match possible
		prefixLen = 1
	}
	for a, g := range r.groups {
		if a != "*" && strings.HasPrefix(agent, a) {
			if l := len(a); l > prefixLen {
				prefixLen = l
				ret = g
			}
		}
	}

	if ret == nil {
		if r.disallowAll {
			return emptyDisallowGroup
		} else {
			return emptyGroup
		}
	}
	return
}

func (g *Group) Test(path string) bool {
	if g.disallowAll {
		return false
	}
	if r := g.findRule(path); r != nil {
		return r.allow
	}

	// From Google's spec:
	// By default, there are no restrictions for crawling for the designated crawlers.
	return true
}

// From Google's spec:
// The path value is used as a basis to determine whether or not a rule applies
// to a specific URL on a site. With the exception of wildcards, the path is
// used to match the beginning of a URL (and any valid URLs that start with the
// same path).
//
// At a group-member level, in particular for allow and disallow directives,
// the most specific rule based on the length of the [path] entry will trump
// the less specific (shorter) rule. The order of precedence for rules with
// wildcards is undefined.
func (g *Group) findRule(path string) (ret *rule) {
	var prefixLen int

	for _, r := range g.rules {
		if r.pattern != nil {
			if r.pattern.MatchString(path) {
				// Consider this a match equal to the length of the pattern.
				// From Google's spec:
				// The order of precedence for rules with wildcards is undefined.
				if l := len(r.pattern.String()); l > prefixLen {
					prefixLen = l
					ret = r
				}
			}
		} else if r.path == "/" && prefixLen == 0 {
			// Weakest match possible
			prefixLen = 1
			ret = r
		} else if strings.HasPrefix(path, r.path) {
			if l := len(r.path); l > prefixLen {
				prefixLen = l
				ret = r
			}
		}
	}
	return
}
