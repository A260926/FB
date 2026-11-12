package grab

import (
	"context"
	"errors"
	"fmt"
	"io"
	"mime"
	"mime/multipart"
	"net/http"
	"os"
	"sort"
	"strconv"
	"strings"
)

// sparseBufferSize is the single fixed-size transfer buffer used by a
// FetchRanges call. Memory use is independent of the requested offsets and of
// the resource length: received bytes are spooled to a temporary file.
const sparseBufferSize = 32 * 1024

// ErrRangeRepresentationChanged is returned by FetchRanges when the remote
// representation cannot be proven identical across the requests of one call.
// It is returned for an HTTP 412 response, a strong ETag that changes or goes
// missing on a subsequent request, or a resource total length that changes.
//
// The call is not retried and the destination is never modified.
var ErrRangeRepresentationChanged = errors.New("grab: remote representation changed during range fetch")

// ByteRange is a half-open byte range [Start, End) of a resource: Start is the
// offset of the first requested byte and End is one past the last requested
// byte.
type ByteRange struct {
	Start int64
	End   int64
}

// RangeOptions configures a Client.FetchRanges call.
type RangeOptions struct {
	// BatchSize is the maximum number of ranges carried in one HTTP request.
	// It must be positive.
	BatchSize int

	// TempDir is the directory in which received bytes are spooled before the
	// whole plan has been validated and the destination is published to. It
	// defaults to os.TempDir when empty. Only files created by the call are
	// used and removed; any pre-existing content is left untouched.
	TempDir string
}

// RangeResult is the outcome of a successful Client.FetchRanges call.
type RangeResult struct {
	// Size is the total length of the remote representation in bytes. It is
	// -1 when the plan was empty, in which case no request is sent.
	Size int64

	// ETag is the strong ETag of the fetched representation. It is empty when
	// the server did not provide a strong validator, which is only permitted
	// for a single-batch call.
	ETag string

	// Ranges is the normalized plan: sorted by Start with duplicate,
	// overlapping and adjacent ranges merged. The caller's input slice is
	// never modified.
	Ranges []ByteRange
}

// A spoolChunk records where validated bytes live in the temporary spool file
// and where they must be published in the destination resource.
type spoolChunk struct {
	resOff   int64 // offset within the resource and destination
	spoolOff int64 // offset within the spool file
	length   int64
}

// A respPart is one parsed Content-Range body from a 206 response.
type respPart struct {
	start    int64 // inclusive resource offset
	end      int64 // exclusive resource offset
	total    int64 // complete resource length stated by Content-Range
	spoolOff int64 // body position in the spool file
}

// FetchRanges synchronously fetches the given byte ranges of the resource
// described by template and writes them to dst at their original offsets.
//
// The plan is normalized first: ranges are copied and sorted, and duplicate,
// overlapping and adjacent ranges are merged. The normalized plan is returned
// in RangeResult.Ranges; the ranges argument is never modified. Ranges are
// requested in batches of at most RangeOptions.BatchSize ranges per HTTP
// request.
//
// Nothing is written to dst until every response and every range has been
// validated: received bytes are spooled in RangeOptions.TempDir and only
// published to dst once the whole plan is covered exactly once. Positions
// outside the plan keep their previous contents; dst is neither truncated nor
// closed. Network, protocol, cancellation and representation-change failures
// leave dst completely untouched.
//
// The existing Do and Request behavior is unaffected by this method.
func (c *Client) FetchRanges(
	ctx context.Context,
	template *http.Request,
	dst io.WriterAt,
	ranges []ByteRange,
	opts RangeOptions,
) (RangeResult, error) {
	result := RangeResult{Size: -1}

	if ctx == nil {
		return result, errors.New("grab: nil context")
	}
	if template == nil {
		return result, errors.New("grab: nil template request")
	}
	if dst == nil {
		return result, errors.New("grab: nil destination writer")
	}
	if c.HTTPClient == nil {
		return result, errors.New("grab: Client has no HTTPClient")
	}
	if template.Method != "" && template.Method != http.MethodGet {
		return result, fmt.Errorf("grab: FetchRanges requires a GET request, got %q", template.Method)
	}
	if template.Body != nil || template.GetBody != nil {
		return result, errors.New("grab: FetchRanges request must not carry a body")
	}
	if opts.BatchSize <= 0 {
		return result, errors.New("grab: RangeOptions.BatchSize must be positive")
	}
	for _, h := range []string{"Range", "If-Range", "If-Match"} {
		if template.Header.Get(h) != "" {
			return result, fmt.Errorf("grab: template request must not pre-set the %s header", h)
		}
	}

	plan, err := normalizeRanges(ranges)
	if err != nil {
		return result, err
	}
	result.Ranges = plan
	if len(plan) == 0 {
		// No work: send nothing, write nothing, leave Size at -1.
		return result, nil
	}

	tempDir := opts.TempDir
	if tempDir == "" {
		tempDir = os.TempDir()
	}
	spool, err := os.CreateTemp(tempDir, "grab-sparse-*.tmp")
	if err != nil {
		return result, err
	}
	spoolName := spool.Name()
	// The spool is private to this call and is always removed; pre-existing
	// contents of TempDir are never touched.
	defer os.Remove(spoolName)
	defer spool.Close()

	buf := make([]byte, sparseBufferSize)
	batchCount := (len(plan) + opts.BatchSize - 1) / opts.BatchSize
	var (
		etag     string
		size     int64 = -1
		chunks   []spoolChunk
		spoolPos int64
	)

	for lo := 0; lo < len(plan); lo += opts.BatchSize {
		if err := ctx.Err(); err != nil {
			return result, err
		}
		hi := lo + opts.BatchSize
		if hi > len(plan) {
			hi = len(plan)
		}
		batch := plan[lo:hi]

		hreq, err := buildRangeRequest(ctx, template, batch, etag)
		if err != nil {
			return result, err
		}
		hresp, err := c.doHTTPRequest(hreq)
		if err != nil {
			return result, err
		}

		var (
			parts     []respPart
			respSize  int64
			respETag  string
			strongOK  bool
			fullFetch bool
			fullLen   int64
		)
		perr := func() error {
			switch hresp.StatusCode {
			case http.StatusPreconditionFailed:
				return ErrRangeRepresentationChanged
			case http.StatusOK:
				if lo != 0 {
					return fmt.Errorf("grab: server returned 200 for batch %d instead of a partial response", lo/opts.BatchSize+1)
				}
				if enc := hresp.Header.Get("Content-Encoding"); enc != "" && !strings.EqualFold(enc, "identity") {
					return fmt.Errorf("grab: unsupported Content-Encoding %q", enc)
				}
				n, err := spoolAll(spool, hresp.Body, buf, &spoolPos)
				if err != nil {
					return err
				}
				if hresp.ContentLength >= 0 && hresp.ContentLength != n {
					return fmt.Errorf("%w: declared %d bytes but read %d", ErrBadLength, hresp.ContentLength, n)
				}
				fullFetch = true
				fullLen = n
				respSize = n
				// No further request follows a full representation, so an
				// ETag validator is not required.
				respETag, _ = strongETag(hresp.Header)
				return nil
			case http.StatusPartialContent:
				parts, respSize, err = readPartialResponse(hresp, spool, buf, &spoolPos)
				if err != nil {
					return err
				}
				if err := validateParts(parts, batch, respSize); err != nil {
					return err
				}
				respETag, strongOK = strongETag(hresp.Header)
				return nil
			default:
				return StatusCodeError(hresp.StatusCode)
			}
		}()
		cerr := hresp.Body.Close()
		if perr != nil {
			return result, perr
		}
		if cerr != nil {
			return result, cerr
		}

		if fullFetch {
			// The server ignored the Range header and sent the whole entity.
			// Extract every planned range from the spooled representation and
			// stop: no further batches are sent.
			if plan[len(plan)-1].End > respSize {
				return result, fmt.Errorf("%w: planned range extends past resource length %d", ErrBadLength, respSize)
			}
			base := spoolPos - fullLen
			for _, r := range plan {
				chunks = append(chunks, spoolChunk{
					resOff:   r.Start,
					spoolOff: base + r.Start,
					length:   r.End - r.Start,
				})
			}
			size = respSize
			etag = respETag
			break
		}

		if lo == 0 {
			size = respSize
			if batchCount > 1 && !strongOK {
				return result, fmt.Errorf("%w: first 206 response did not provide a strong ETag", ErrRangeRepresentationChanged)
			}
			etag = respETag
		} else {
			if !strongOK || respETag != etag {
				return result, fmt.Errorf("%w: expected strong ETag %q, got %q", ErrRangeRepresentationChanged, etag, respETag)
			}
			if respSize != size {
				return result, fmt.Errorf("%w: resource length changed from %d to %d", ErrRangeRepresentationChanged, size, respSize)
			}
		}
		if plan[len(plan)-1].End > size {
			return result, fmt.Errorf("%w: planned range extends past resource length %d", ErrBadLength, size)
		}

		// Record only the intersections with this batch's requested ranges.
		// Bytes from gaps inside a part that merges nearby ranges are never
		// published.
		for _, p := range parts {
			for _, r := range batch {
				s := p.start
				if r.Start > s {
					s = r.Start
				}
				e := p.end
				if r.End < e {
					e = r.End
				}
				if s < e {
					chunks = append(chunks, spoolChunk{
						resOff:   s,
						spoolOff: p.spoolOff + (s - p.start),
						length:   e - s,
					})
				}
			}
		}
	}

	if err := ctx.Err(); err != nil {
		return result, err
	}

	// All checks passed: publish at the original resource offsets. A failure
	// here is reported as-is without rolling back bytes already published.
	if err := publishChunks(dst, spool, chunks, buf); err != nil {
		return result, err
	}

	result.Size = size
	result.ETag = etag
	return result, nil
}

// normalizeRanges returns a sorted copy of in with duplicate, overlapping and
// adjacent ranges merged.
func normalizeRanges(in []ByteRange) ([]ByteRange, error) {
	rs := make([]ByteRange, len(in))
	copy(rs, in)
	for _, r := range rs {
		if r.Start < 0 {
			return nil, fmt.Errorf("grab: range start must not be negative: %d", r.Start)
		}
		if r.End <= r.Start {
			return nil, fmt.Errorf("grab: invalid range [%d,%d): end must be greater than start", r.Start, r.End)
		}
	}
	sort.Slice(rs, func(i, j int) bool {
		if rs[i].Start != rs[j].Start {
			return rs[i].Start < rs[j].Start
		}
		return rs[i].End < rs[j].End
	})

	merged := make([]ByteRange, 0, len(rs))
	for _, r := range rs {
		if n := len(merged); n > 0 && r.Start <= merged[n-1].End {
			if r.End > merged[n-1].End {
				merged[n-1].End = r.End
			}
			continue
		}
		merged = append(merged, r)
	}
	return merged, nil
}

// buildRangeRequest returns a deep copy of template carrying the Range header
// for batch. Neither template nor the Client-shared header map is modified.
func buildRangeRequest(ctx context.Context, template *http.Request, batch []ByteRange, ifMatch string) (*http.Request, error) {
	req := template.Clone(ctx)
	req.Method = http.MethodGet
	req.Body = nil
	req.GetBody = nil
	req.ContentLength = 0

	var sb strings.Builder
	sb.WriteString("bytes=")
	for i, r := range batch {
		if i > 0 {
			sb.WriteByte(',')
		}
		fmt.Fprintf(&sb, "%d-%d", r.Start, r.End-1)
	}
	req.Header.Set("Range", sb.String())
	// We must see the raw entity bytes to match them to resource offsets.
	req.Header.Set("Accept-Encoding", "identity")
	if ifMatch != "" {
		req.Header.Set("If-Match", ifMatch)
	}
	return req, nil
}

// readPartialResponse parses a 206 response into validated parts, spooling
// every part body. It accepts a single Content-Range body or a
// multipart/byteranges body; parts may arrive in any order.
func readPartialResponse(hresp *http.Response, spool *os.File, buf []byte, spoolPos *int64) ([]respPart, int64, error) {
	if enc := hresp.Header.Get("Content-Encoding"); enc != "" && !strings.EqualFold(enc, "identity") {
		return nil, 0, fmt.Errorf("grab: unsupported Content-Encoding %q", enc)
	}

	mediaType, params, _ := mime.ParseMediaType(hresp.Header.Get("Content-Type"))
	if mediaType == "multipart/byteranges" {
		boundary := params["boundary"]
		if boundary == "" {
			return nil, 0, errors.New("grab: multipart/byteranges response is missing a boundary")
		}
		var (
			parts []respPart
			total int64 = -1
		)
		reader := multipart.NewReader(hresp.Body, boundary)
		for {
			part, err := reader.NextPart()
			if err == io.EOF {
				break
			}
			if err != nil {
				return nil, 0, fmt.Errorf("grab: malformed multipart/byteranges body: %w", err)
			}
			if enc := part.Header.Get("Content-Encoding"); enc != "" && !strings.EqualFold(enc, "identity") {
				return nil, 0, fmt.Errorf("grab: unsupported Content-Encoding %q on range part", enc)
			}
			p, err := spoolPart(part.Header.Get("Content-Range"), part, spool, buf, spoolPos)
			if err != nil {
				return nil, 0, err
			}
			if total == -1 {
				total = p.total
			} else if p.total != total {
				return nil, 0, fmt.Errorf("%w: parts state resource lengths %d and %d", ErrBadLength, total, p.total)
			}
			parts = append(parts, p)
		}
		if len(parts) == 0 {
			return nil, 0, errors.New("grab: multipart/byteranges response contained no parts")
		}
		return parts, total, nil
	}

	// A single partial body. The outer Content-Length must not be mistaken for
	// the resource length: only Content-Range tells us that.
	p, err := spoolPart(hresp.Header.Get("Content-Range"), hresp.Body, spool, buf, spoolPos)
	if err != nil {
		return nil, 0, err
	}
	return []respPart{p}, p.total, nil
}

// spoolPart parses a Content-Range header, copies exactly its declared span
// from body into the spool and verifies that the body ends immediately after.
func spoolPart(contentRange string, body io.Reader, spool *os.File, buf []byte, spoolPos *int64) (respPart, error) {
	start, end, total, err := parseContentRange(contentRange)
	if err != nil {
		return respPart{}, err
	}
	span := end - start + 1
	off := *spoolPos
	n, err := copySpool(spool, body, span, buf, off)
	*spoolPos = off + n
	if err != nil {
		return respPart{}, err
	}
	if n != span {
		return respPart{}, fmt.Errorf("%w: truncated range part: want %d bytes, got %d", ErrBadLength, span, n)
	}
	if err := expectEOF(body); err != nil {
		return respPart{}, err
	}
	return respPart{start: start, end: end + 1, total: total, spoolOff: off}, nil
}

// parseContentRange parses "bytes start-end/total" as defined by RFC 9110.
// Unsatisfied ranges ("bytes */total") and unknown total lengths are rejected.
func parseContentRange(v string) (start, end, total int64, err error) {
	v = strings.TrimSpace(v)
	const prefix = "bytes "
	if !strings.HasPrefix(v, prefix) {
		return 0, 0, 0, fmt.Errorf("grab: invalid or missing Content-Range %q", v)
	}
	rest := v[len(prefix):]
	slash := strings.IndexByte(rest, '/')
	if slash < 0 {
		return 0, 0, 0, fmt.Errorf("grab: malformed Content-Range %q", v)
	}
	rangeSpec, totalSpec := rest[:slash], rest[slash+1:]
	if rangeSpec == "*" || totalSpec == "*" {
		return 0, 0, 0, fmt.Errorf("grab: Content-Range %q must state a concrete range and total length", v)
	}
	dash := strings.IndexByte(rangeSpec, '-')
	if dash < 0 {
		return 0, 0, 0, fmt.Errorf("grab: malformed Content-Range %q", v)
	}
	if start, err = strconv.ParseInt(rangeSpec[:dash], 10, 64); err != nil {
		return 0, 0, 0, fmt.Errorf("grab: malformed Content-Range %q: %w", v, err)
	}
	if end, err = strconv.ParseInt(rangeSpec[dash+1:], 10, 64); err != nil {
		return 0, 0, 0, fmt.Errorf("grab: malformed Content-Range %q: %w", v, err)
	}
	if total, err = strconv.ParseInt(totalSpec, 10, 64); err != nil {
		return 0, 0, 0, fmt.Errorf("grab: malformed Content-Range %q: %w", v, err)
	}
	if start < 0 || end < start || total <= end {
		return 0, 0, 0, fmt.Errorf("grab: Content-Range %q is out of bounds", v)
	}
	return start, end, total, nil
}

// validateParts checks that parts are in bounds, pairwise disjoint, relevant
// to the batch, and that every requested range is covered exactly once.
func validateParts(parts []respPart, batch []ByteRange, total int64) error {
	sorted := make([]respPart, len(parts))
	copy(sorted, parts)
	sort.Slice(sorted, func(i, j int) bool { return sorted[i].start < sorted[j].start })

	for i, p := range sorted {
		if p.total != total || p.start < 0 || p.end <= p.start || p.end > total {
			return fmt.Errorf("grab: range part [%d,%d) is out of bounds for resource length %d", p.start, p.end, total)
		}
		if i > 0 && p.start < sorted[i-1].end {
			return fmt.Errorf("grab: overlapping range parts [%d,%d) and [%d,%d)", sorted[i-1].start, sorted[i-1].end, p.start, p.end)
		}
	}

	for _, p := range sorted {
		relevant := false
		for _, r := range batch {
			if p.start < r.End && r.Start < p.end {
				relevant = true
				break
			}
		}
		if !relevant {
			return fmt.Errorf("grab: range part [%d,%d) does not intersect any requested range", p.start, p.end)
		}
	}

	for _, r := range batch {
		cursor := r.Start
		for _, p := range sorted {
			if p.end <= cursor {
				continue
			}
			if p.start > cursor {
				return fmt.Errorf("%w: gap at offset %d in response for range [%d,%d)", ErrBadLength, cursor, r.Start, r.End)
			}
			cursor = p.end
			if cursor >= r.End {
				break
			}
		}
		if cursor < r.End {
			return fmt.Errorf("%w: truncated coverage for range [%d,%d), ended at %d", ErrBadLength, r.Start, r.End, cursor)
		}
	}
	return nil
}

// strongETag returns the ETag header value when it is a strong validator
// (RFC 9110 §8.8.3). Weak validators and malformed values are treated as
// absent.
func strongETag(h http.Header) (string, bool) {
	v := strings.TrimSpace(h.Get("ETag"))
	if len(v) < 2 || !strings.HasPrefix(v, "\"") || !strings.HasSuffix(v, "\"") {
		return "", false
	}
	return v, true
}

// copySpool copies exactly n bytes from src into spool at offset off using the
// shared fixed buffer. It returns the number copied before an error or EOF.
func copySpool(spool *os.File, src io.Reader, n int64, buf []byte, off int64) (int64, error) {
	var copied int64
	for copied < n {
		want := len(buf)
		if remaining := n - copied; int64(want) > remaining {
			want = int(remaining)
		}
		m, rerr := src.Read(buf[:want])
		if m > 0 {
			w, werr := spool.WriteAt(buf[:m], off+copied)
			copied += int64(w)
			if w != m {
				if werr == nil {
					werr = io.ErrShortWrite
				}
				return copied, werr
			}
		}
		if rerr != nil {
			if rerr == io.EOF {
				if copied < n {
					return copied, fmt.Errorf("%w: truncated range part: want %d bytes, got %d", ErrBadLength, n, copied)
				}
				break
			}
			return copied, rerr
		}
	}
	return copied, nil
}

// spoolAll copies src until EOF, returning the number of bytes spooled. It is
// used for the 200 full-representation fallback, whose length may only be
// known after reading to EOF.
func spoolAll(spool *os.File, src io.Reader, buf []byte, spoolPos *int64) (int64, error) {
	var total int64
	base := *spoolPos
	for {
		n, err := src.Read(buf)
		if n > 0 {
			w, werr := spool.WriteAt(buf[:n], base+total)
			total += int64(w)
			if w != n {
				if werr == nil {
					werr = io.ErrShortWrite
				}
				*spoolPos = base + total
				return total, werr
			}
		}
		if err != nil {
			*spoolPos = base + total
			if err == io.EOF {
				return total, nil
			}
			return total, err
		}
	}
}

// expectEOF fails if any further byte can be read from r.
func expectEOF(r io.Reader) error {
	var b [1]byte
	for {
		n, err := r.Read(b[:])
		if n > 0 {
			return fmt.Errorf("%w: range part contains more bytes than its declared span", ErrBadLength)
		}
		if err != nil {
			if err == io.EOF {
				return nil
			}
			return err
		}
	}
}

// publishChunks writes the validated bytes from the spool to dst at their
// resource offsets, in offset order.
func publishChunks(dst io.WriterAt, spool *os.File, chunks []spoolChunk, buf []byte) error {
	sorted := make([]spoolChunk, len(chunks))
	copy(sorted, chunks)
	sort.Slice(sorted, func(i, j int) bool { return sorted[i].resOff < sorted[j].resOff })

	for _, ch := range sorted {
		var pos int64
		for pos < ch.length {
			want := len(buf)
			if remaining := ch.length - pos; int64(want) > remaining {
				want = int(remaining)
			}
			n, rerr := spool.ReadAt(buf[:want], ch.spoolOff+pos)
			if n > 0 {
				w, werr := dst.WriteAt(buf[:n], ch.resOff+pos)
				if w < n {
					if werr == nil {
						werr = io.ErrShortWrite
					}
					return werr
				}
				pos += int64(w)
			}
			if rerr != nil && rerr != io.EOF {
				return rerr
			}
		}
	}
	return nil
}
