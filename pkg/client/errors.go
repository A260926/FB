package client

import (
	"errors"
	"fmt"
	"net/http"
	"strconv"
	"time"
)

// Sentinel errors. Use errors.Is for matching.
var (
	// ErrNotFound is returned when the server responds with HTTP 404.
	ErrNotFound = errors.New("stac: not found")
	// ErrUnauthorized is returned for HTTP 401.
	ErrUnauthorized = errors.New("stac: unauthorized")
	// ErrForbidden is returned for HTTP 403.
	ErrForbidden = errors.New("stac: forbidden")
	// ErrRateLimited is returned for HTTP 429. The wrapping *HTTPError
	// surfaces RetryAfter when the server provides a Retry-After header.
	ErrRateLimited = errors.New("stac: rate limited")
	// ErrServer is returned for HTTP 5xx.
	ErrServer = errors.New("stac: server error")
	// ErrPaginationCycle is returned when the iterator detects that the
	// server's next links revisit a page already requested.
	ErrPaginationCycle = errors.New("stac: pagination cycle detected")
	// ErrPaginationLimit is returned when the configured maximum number
	// of pages has been reached.
	ErrPaginationLimit = errors.New("stac: pagination page limit reached")
	// ErrUnsupportedForGET is returned by SearchSimple when a parameter
	// cannot be expressed in a GET /search request.
	ErrUnsupportedForGET = errors.New("stac: parameter not supported by GET /search; use Search (POST) instead")
	// ErrResponseTooLarge is returned when a response body exceeds the
	// configured maximum size.
	ErrResponseTooLarge = errors.New("stac: response body exceeds maximum size")
	// ErrUnexpectedContentType is returned when a JSON endpoint returns a
	// non-JSON Content-Type.
	ErrUnexpectedContentType = errors.New("stac: unexpected Content-Type")
)

// APIError represents a STAC API JSON error response body.
type APIError struct {
	Code        int    `json:"code"`
	Description string `json:"description"`
	Type        string `json:"type,omitempty"`
}

// Error implements the error interface.
func (e *APIError) Error() string {
	if e == nil {
		return ""
	}
	if e.Type != "" {
		return fmt.Sprintf("stac api: [%d %s] %s", e.Code, e.Type, e.Description)
	}
	return fmt.Sprintf("stac api: [%d] %s", e.Code, e.Description)
}

// HTTPError is returned for non-2xx HTTP responses. It implements error,
// supports errors.Is against the sentinel errors above, and surfaces the
// decoded API body via Unwrap.
type HTTPError struct {
	Status     int
	Method     string
	URL        string
	Body       []byte // truncated to client.MaxErrorBodyBytes
	RetryAfter time.Duration
	API        *APIError
}

// Error implements the error interface.
func (e *HTTPError) Error() string {
	switch {
	case e.API != nil:
		return fmt.Sprintf("%s %s: %d %s: %s",
			e.Method, e.URL, e.Status, http.StatusText(e.Status), e.API.Error())
	case len(e.Body) > 0:
		return fmt.Sprintf("%s %s: %d %s: %s",
			e.Method, e.URL, e.Status, http.StatusText(e.Status), string(e.Body))
	default:
		return fmt.Sprintf("%s %s: %d %s",
			e.Method, e.URL, e.Status, http.StatusText(e.Status))
	}
}

// Unwrap returns the wrapped APIError when one was decoded.
func (e *HTTPError) Unwrap() error { return e.API }

// Is allows errors.Is matching against the sentinel errors.
func (e *HTTPError) Is(target error) bool {
	switch target {
	case ErrNotFound:
		return e.Status == http.StatusNotFound
	case ErrUnauthorized:
		return e.Status == http.StatusUnauthorized
	case ErrForbidden:
		return e.Status == http.StatusForbidden
	case ErrRateLimited:
		return e.Status == http.StatusTooManyRequests
	case ErrServer:
		return e.Status >= 500 && e.Status < 600
	}
	return false
}

// parseRetryAfter parses the Retry-After header value as either delta-seconds
// or an HTTP-date and returns a time.Duration. It returns 0 if absent or
// malformed.
func parseRetryAfter(v string) time.Duration {
	if v == "" {
		return 0
	}
	if secs, err := strconv.Atoi(v); err == nil && secs >= 0 {
		return time.Duration(secs) * time.Second
	}
	if t, err := http.ParseTime(v); err == nil {
		if d := time.Until(t); d > 0 {
			return d
		}
	}
	return 0
}
