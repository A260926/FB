package grab

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"strconv"
	"strings"
	"sync/atomic"
)

// reconnectSupported reports whether the Request is eligible for mid-transfer
// reconnects. Only bodyless GET requests using the identity content encoding
// qualify: grab cannot re-issue another method, and a compressed or chunked
// representation cannot be validated byte-for-byte against a resumed prefix.
func reconnectSupported(req *Request) bool {
	if req == nil || req.HTTPRequest == nil || req.MaxReconnects == 0 {
		return false
	}
	if req.HTTPRequest.Method != http.MethodGet || req.HTTPRequest.Body != nil ||
		req.HTTPRequest.GetBody != nil {
		return false
	}
	for _, te := range req.HTTPRequest.TransferEncoding {
		if !strings.EqualFold(te, "identity") {
			return false
		}
	}
	if te := req.HTTPRequest.Header.Get("Transfer-Encoding"); te != "" &&
		!strings.EqualFold(te, "identity") {
		return false
	}
	if ce := req.HTTPRequest.Header.Get("Content-Encoding"); ce != "" &&
		!strings.EqualFold(ce, "identity") {
		return false
	}
	if ae := req.HTTPRequest.Header.Get("Accept-Encoding"); ae != "" &&
		!strings.EqualFold(ae, "identity") {
		return false
	}
	return true
}

// strongETag returns the ETag header value only when it denotes a strong
// validator. Weak ETags (W/"...") may change without a change in the bytes and
// therefore cannot protect a resumed prefix.
func strongETag(h http.Header) (string, bool) {
	etag := h.Get("ETag")
	if etag == "" || strings.HasPrefix(etag, "W/") || strings.HasPrefix(etag, `w/`) {
		return "", false
	}
	return etag, true
}

// captureRepresentation records the metadata of the representation currently
// being written so that reconnect responses can be checked against it. It
// returns false when reconnects cannot be offered for this response: the
// transfer length must be known and the server must provide a strong ETag.
func (c *Client) captureRepresentation(resp *Response) bool {
	if !reconnectSupported(resp.Request) {
		return false
	}
	hresp := resp.HTTPResponse
	if hresp == nil || hresp.ContentLength < 0 {
		return false
	}
	if ce := hresp.Header.Get("Content-Encoding"); ce != "" &&
		!strings.EqualFold(ce, "identity") {
		return false
	}
	etag, ok := strongETag(hresp.Header)
	if !ok {
		return false
	}
	resp.repKnown = true
	resp.repTotal = hresp.ContentLength + resp.bytesResumed
	resp.repETag = etag
	resp.reconnectsLeft = resp.Request.MaxReconnects
	return true
}

// reconnectReadError reports whether err originates in reading the response
// body and therefore may justify a reconnect attempt. Errors from the
// destination writer, the rate limiter, hooks or the context are deliberately
// excluded even when their concrete type happens to match.
func reconnectReadError(err error, lengthKnown bool) bool {
	if err == nil {
		return false
	}
	// A canceled or expired context is a caller decision, never a dropped
	// connection to recover from.
	if errors.Is(err, context.Canceled) || errors.Is(err, context.DeadlineExceeded) {
		return false
	}
	// Writer and rate limiter failures are wrapped with their own sentinels so
	// an identical underlying error type cannot masquerade as a read failure.
	if errors.Is(err, errTransferWrite) || errors.Is(err, errTransferLimit) {
		return false
	}
	if errors.Is(err, io.ErrUnexpectedEOF) {
		return true
	}
	var netErr net.Error
	if errors.As(err, &netErr) {
		return true
	}
	// A bare io.EOF before the advertised number of bytes means the body was
	// cut short, but only when the expected length is known.
	if lengthKnown && errors.Is(err, io.EOF) {
		return true
	}
	return false
}

// written returns the number of bytes truly written to the destination for the
// current representation, including any prefix that was resumed from a local
// file before this Client.Do call began.
func (c *Client) written(resp *Response) int64 {
	resp.transferMu.RLock()
	defer resp.transferMu.RUnlock()
	return resp.bytesResumed + resp.transfer.N()
}

// reconnect issues a Range GET for the bytes still missing from the current
// representation and, after strict validation, swaps the response and transfer
// over to it. It consumes one reconnect allowance. A returned error ends the
// transfer.
func (c *Client) reconnect(resp *Response, readErr error) (stateFunc, error) {
	if resp.reconnectsLeft <= 0 {
		// The allowance is exhausted: keep the prefix already on disk and
		// surface the read error that interrupted the last body.
		return nil, readErr
	}
	resp.reconnectsLeft--

	k := c.written(resp)
	if k >= resp.repTotal {
		// Nothing is missing, so there is nothing to resume.
		return nil, readErr
	}

	// Build a fresh request from the final URL that served the body so far.
	// The caller's http.Request is never mutated: headers are cloned and the
	// Range/If-Range/Accept-Encoding headers live only on this copy.
	hreq := c.newReconnectRequest(resp, k)
	hresp, err := c.doHTTPRequest(hreq)
	if err != nil {
		return nil, err
	}

	switch hresp.StatusCode {
	case http.StatusPartialContent:
		return c.handlePartial(resp, hresp, k)
	case http.StatusOK:
		return c.handleRestart(resp, hresp)
	default:
		hresp.Body.Close()
		return nil, fmt.Errorf("%w: server returned %d on reconnect",
			ErrBadReconnect, hresp.StatusCode)
	}
}

// newReconnectRequest constructs the GET used to fetch the missing suffix.
func (c *Client) newReconnectRequest(resp *Response, k int64) *http.Request {
	src := resp.Request.HTTPRequest
	// Clone deep-copies the URL and Header map, so the internal Range,
	// If-Range and Accept-Encoding headers cannot leak into the caller's
	// request object.
	hreq := src.Clone(resp.ctx)
	// Use the final address that actually served the body, following any
	// redirects, rather than re-resolving the original URL. No HEAD request
	// is made during a reconnect.
	if final := resp.HTTPResponse.Request; final != nil {
		hreq.URL = final.URL
		hreq.Host = final.Host
	}
	hreq.Method = http.MethodGet
	hreq.Body = nil
	hreq.GetBody = nil
	hreq.ContentLength = 0
	hreq.Header.Set("Range", fmt.Sprintf("bytes=%d-", k))
	hreq.Header.Set("If-Range", resp.repETag)
	hreq.Header.Set("Accept-Encoding", "identity")
	hreq.Header.Del("Transfer-Encoding")
	hreq.Header.Del("Content-Encoding")
	return hreq
}

// handlePartial validates a 206 response against the current representation
// and appends its body to the destination.
func (c *Client) handlePartial(resp *Response, hresp *http.Response, k int64) (stateFunc, error) {
	start, end, total, err := parseContentRange(hresp.Header.Get("Content-Range"))
	if err != nil {
		return rejectReconnect(hresp, "%v", err)
	}
	if start != k || end != resp.repTotal-1 || total != resp.repTotal {
		return rejectReconnect(hresp,
			"Content-Range %q does not cover bytes %d-%d of %d",
			hresp.Header.Get("Content-Range"), k, resp.repTotal-1, resp.repTotal)
	}
	etag, ok := strongETag(hresp.Header)
	if !ok || etag != resp.repETag {
		return rejectReconnect(hresp, "ETag %q does not match current representation %q",
			hresp.Header.Get("ETag"), resp.repETag)
	}
	if hresp.ContentLength < 0 {
		return rejectReconnect(hresp, "reconnect response has no Content-Length")
	}
	if hresp.ContentLength != resp.repTotal-k {
		return rejectReconnect(hresp, "Content-Length %d does not match remaining %d bytes",
			hresp.ContentLength, resp.repTotal-k)
	}
	if ce := hresp.Header.Get("Content-Encoding"); ce != "" &&
		!strings.EqualFold(ce, "identity") {
		return rejectReconnect(hresp, "unexpected Content-Encoding %q", ce)
	}

	// The old body and its transfer are finished; replace them without
	// touching the destination, which still holds the validated prefix.
	resp.closeResponseBody()
	if err := c.swapTransfer(resp, hresp, false); err != nil {
		hresp.Body.Close()
		return nil, err
	}
	resp.HTTPResponse = hresp
	return c.copyFile, nil
}

// rejectReconnect closes a response that failed validation and wraps the
// explanation in ErrBadReconnect.
func rejectReconnect(hresp *http.Response, format string, args ...any) (stateFunc, error) {
	hresp.Body.Close()
	return nil, fmt.Errorf("%w: %s", ErrBadReconnect, fmt.Sprintf(format, args...))
}

// handleRestart validates a 200 response as a brand-new representation and,
// if accepted, rewinds the destination to zero before downloading it again.
func (c *Client) handleRestart(resp *Response, hresp *http.Response) (stateFunc, error) {
	if hresp.ContentLength < 0 {
		return rejectReconnect(hresp, "restart response has no Content-Length")
	}
	etag, ok := strongETag(hresp.Header)
	if !ok {
		return rejectReconnect(hresp, "restart response is missing a strong ETag")
	}
	if ce := hresp.Header.Get("Content-Encoding"); ce != "" &&
		!strings.EqualFold(ce, "identity") {
		return rejectReconnect(hresp, "unexpected Content-Encoding %q", ce)
	}
	newTotal := hresp.ContentLength
	if resp.Request.Size > 0 && resp.Request.Size != newTotal {
		return rejectReconnect(hresp, "restart length %d does not match Request.Size %d",
			newTotal, resp.Request.Size)
	}

	// The filename is already determined for this Response and a server
	// suggested name must never open a second file.
	if resp.Filename == "" {
		return rejectReconnect(hresp, "restart response arrived before a filename was determined")
	}

	// Re-run the BeforeCopy hook before destroying the old prefix, mirroring a
	// fresh download. A rejection leaves the existing bytes untouched.
	if f := resp.Request.BeforeCopy; f != nil {
		if err := f(resp); err != nil {
			hresp.Body.Close()
			return nil, err
		}
	}

	resp.closeResponseBody()

	// Reset the destination and all counters to the new representation.
	if resp.Request.NoStore {
		resp.storeBuffer.Reset()
	} else if t, ok := resp.writer.(truncater); ok {
		if err := t.Truncate(0); err != nil {
			hresp.Body.Close()
			return nil, err
		}
	}
	resp.repTotal = newTotal
	resp.repETag = etag
	resp.repKnown = true

	if err := c.swapTransfer(resp, hresp, true); err != nil {
		hresp.Body.Close()
		return nil, err
	}
	resp.HTTPResponse = hresp
	atomic.StoreInt64(&resp.sizeUnsafe, newTotal)
	return c.copyFile, nil
}

// swapTransfer replaces the finished transfer with one reading from hresp and
// writing to the same destination. A restart rewinds the destination and the
// progress counters; a continuation keeps the resumed prefix.
func (c *Client) swapTransfer(resp *Response, hresp *http.Response, restart bool) error {
	if !resp.Request.NoStore {
		if f, ok := resp.writer.(interface {
			Seek(int64, int) (int64, error)
		}); ok {
			whence := io.SeekEnd
			if restart {
				whence = io.SeekStart
			}
			if _, err := f.Seek(0, whence); err != nil {
				return err
			}
		}
	}
	if resp.bufferSize < 1 {
		resp.bufferSize = 32 * 1024
	}
	b := make([]byte, resp.bufferSize)
	t := newTransfer(
		resp.Request.Context(),
		resp.Request.RateLimiter,
		resp.writer,
		hresp.Body,
		b)

	resp.transferMu.Lock()
	oldN := int64(0)
	if resp.transfer != nil {
		oldN = resp.transfer.N()
	}
	if restart {
		// A 200 restart replaces the representation: every counter starts at
		// zero and only the new bytes are counted.
		resp.bytesResumed = 0
		resp.DidResume = false
	} else {
		// A 206 continuation keeps the same representation. Fold the bytes the
		// finished transfer wrote into the resumed prefix so the progress
		// methods never reset or double count.
		resp.bytesResumed += oldN
	}
	resp.transfer = t
	resp.transferMu.Unlock()
	return nil
}

// parseContentRange parses a "bytes start-end/total" Content-Range header and
// rejects absent or open-ended values.
func parseContentRange(value string) (start, end, total int64, err error) {
	const prefix = "bytes "
	if !strings.HasPrefix(value, prefix) {
		return 0, 0, 0, fmt.Errorf("malformed Content-Range %q", value)
	}
	part := strings.TrimPrefix(value, prefix)
	pos := strings.IndexByte(part, '/')
	if pos < 0 {
		return 0, 0, 0, fmt.Errorf("malformed Content-Range %q", value)
	}
	span, totalStr := part[:pos], part[pos+1:]
	if totalStr == "" || totalStr == "*" {
		return 0, 0, 0, fmt.Errorf("Content-Range %q has no total length", value)
	}
	dash := strings.IndexByte(span, '-')
	if dash < 0 {
		return 0, 0, 0, fmt.Errorf("malformed Content-Range %q", value)
	}
	start, err = strconv.ParseInt(span[:dash], 10, 64)
	if err != nil {
		return 0, 0, 0, fmt.Errorf("malformed Content-Range %q", value)
	}
	end, err = strconv.ParseInt(span[dash+1:], 10, 64)
	if err != nil {
		return 0, 0, 0, fmt.Errorf("malformed Content-Range %q", value)
	}
	total, err = strconv.ParseInt(totalStr, 10, 64)
	if err != nil {
		return 0, 0, 0, fmt.Errorf("malformed Content-Range %q", value)
	}
	if start < 0 || end < start || total <= end {
		return 0, 0, 0, fmt.Errorf("inconsistent Content-Range %q", value)
	}
	return start, end, total, nil
}
