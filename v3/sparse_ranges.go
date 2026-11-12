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

// ErrRangeRepresentationChanged indicates that the remote resource changed
// identity while a ranged fetch was in progress: a precondition failed
// (412 Precondition Failed), the strong entity tag was lost or altered, or
// the total length of the representation changed between batches. No bytes
// are published to the destination when this error is returned.
var ErrRangeRepresentationChanged = errors.New("remote representation changed during ranged fetch")

// ByteRange identifies a half-open interval [Start, End) of a remote
// resource. Start must be non-negative and End must be greater than Start.
type ByteRange struct {
	Start int64
	End   int64
}

// RangeOptions configures Client.FetchRanges.
type RangeOptions struct {
	// BatchSize is the maximum number of byte ranges requested per HTTP
	// request. It must be positive.
	BatchSize int

	// TempDir is the directory in which fetched bytes are staged before they
	// are published to the destination. If empty, the operating system's
	// default temporary directory is used. Staging files created by a call
	// are always removed before the call returns; pre-existing contents of
	// TempDir are left untouched.
	TempDir string
}

// RangeResult describes the outcome of a successful Client.FetchRanges call.
type RangeResult struct {
	// Size is the total length in bytes of the remote representation, or -1
	// if the fetch plan was empty and no request was sent.
	Size int64

	// ETag is the strong entity tag of the fetched representation, if the
	// server provided one.
	ETag string

	// Ranges is the normalized fetch plan: the requested ranges, sorted and
	// with duplicates, overlaps and adjacencies merged.
	Ranges []ByteRange
}

// FetchRanges downloads selected byte ranges of a remote resource and writes
// them to dst at their original resource offsets, leaving every other
// position of dst unchanged. It is intended for catalog-style captures -
// such as reading the header and trailing index of a large archive - where
// downloading the whole resource would be wasteful. dst is never truncated
// or closed.
//
// The template request supplies the URL and any additional headers; it must
// be a GET request without a body and must not preset the Range, If-Range or
// If-Match headers. Invalid plans and templates are rejected before any I/O.
// The template and the Client configuration are never modified. Every
// request is sent with ctx and forces "Accept-Encoding: identity"; the
// Client's HTTPClient and UserAgent apply as they do for Client.Do.
//
// The requested ranges are validated, copied, sorted and merged before any
// request is sent; the caller's slice is never modified. An empty plan sends
// no request, writes nothing and reports Size -1. A non-empty plan is
// fetched in batches of at most opts.BatchSize ranges per request. Fetched
// bytes are staged on disk (see RangeOptions.TempDir) using only fixed-size
// memory buffers, and are published to dst only after every batch has been
// received and validated. A transfer, protocol, cancellation or
// representation-change failure therefore leaves dst completely untouched,
// while a failure reported by dst itself during publishing (including a
// short write) is returned without rolling back the bytes already
// published.
//
// When a plan spans multiple batches, the first partial response must carry
// a strong ETag. Later requests repeat it in If-Match, and any change or
// loss of the ETag or total size, or a 412 response, fails the call with
// ErrRangeRepresentationChanged; no retry is attempted. If the server
// answers the first request with 200 OK, the full representation is consumed
// to EOF and every planned range is extracted from it; no further requests
// are sent. A 200 response to a later batch is rejected.
//
// FetchRanges is safe for concurrent use; concurrent calls share no staging
// or progress state.
func (c *Client) FetchRanges(ctx context.Context, template *http.Request, dst io.WriterAt, ranges []ByteRange, opts RangeOptions) (RangeResult, error) {
	if ctx == nil {
		return RangeResult{}, errors.New("grab: FetchRanges requires a non-nil context")
	}
	if template == nil {
		return RangeResult{}, errors.New("grab: FetchRanges requires a non-nil request template")
	}
	if dst == nil {
		return RangeResult{}, errors.New("grab: FetchRanges requires a non-nil destination")
	}
	plan, err := normalizeRanges(ranges)
	if err != nil {
		return RangeResult{}, err
	}
	if err := validateRangeTemplate(template, opts); err != nil {
		return RangeResult{}, err
	}
	if len(plan) == 0 {
		return RangeResult{Size: -1, Ranges: plan}, nil
	}

	f := &rangeFetch{
		client:   c,
		ctx:      ctx,
		template: template,
		dst:      dst,
		plan:     plan,
		opts:     opts,
		size:     -1,
		multi:    len(plan) > opts.BatchSize,
	}
	defer f.cleanup()

	if err := f.run(); err != nil {
		return RangeResult{}, err
	}
	if err := f.publish(); err != nil {
		return RangeResult{}, err
	}
	return RangeResult{Size: f.size, ETag: f.etag, Ranges: plan}, nil
}

// normalizeRanges validates, copies, sorts and merges the requested ranges.
// Duplicate, overlapping and adjacent ranges are coalesced. The caller's
// slice is never modified.
func normalizeRanges(ranges []ByteRange) ([]ByteRange, error) {
	plan := make([]ByteRange, 0, len(ranges))
	for _, r := range ranges {
		if r.Start < 0 {
			return nil, fmt.Errorf("grab: range [%d,%d) has a negative start", r.Start, r.End)
		}
		if r.End <= r.Start {
			return nil, fmt.Errorf("grab: range [%d,%d) has an end not greater than its start", r.Start, r.End)
		}
		plan = append(plan, r)
	}
	sort.Slice(plan, func(i, j int) bool {
		if plan[i].Start != plan[j].Start {
			return plan[i].Start < plan[j].Start
		}
		return plan[i].End < plan[j].End
	})
	merged := plan[:0]
	for _, r := range plan {
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

// validateRangeTemplate rejects templates that FetchRanges cannot use,
// before any I/O takes place.
func validateRangeTemplate(template *http.Request, opts RangeOptions) error {
	if opts.BatchSize < 1 {
		return fmt.Errorf("grab: RangeOptions.BatchSize must be positive, got %d", opts.BatchSize)
	}
	if template.Method != "" && template.Method != http.MethodGet {
		return fmt.Errorf("grab: FetchRanges requires a GET request, got %q", template.Method)
	}
	if template.Body != nil || template.GetBody != nil {
		return errors.New("grab: FetchRanges requires a request without a body")
	}
	for k := range template.Header {
		switch http.CanonicalHeaderKey(k) {
		case "Range", "If-Range", "If-Match":
			return fmt.Errorf("grab: request template must not preset the %s header", k)
		}
	}
	return nil
}

// rangeFetch holds the per-call state of one FetchRanges invocation. Two
// concurrent calls never share a rangeFetch.
type rangeFetch struct {
	client   *Client
	ctx      context.Context
	template *http.Request
	dst      io.WriterAt
	plan     []ByteRange
	opts     RangeOptions
	multi    bool // plan spans more than one batch

	size int64  // total representation length, -1 while unknown
	etag string // strong entity tag, "" while unknown

	buf       []byte // fixed-size copy buffer
	stageDir  string
	stageFile *os.File
	stageLen  int64
	segments  []stagedSegment
}

// stagedSegment describes bytes held in the staging file that belong at
// dstOff in the destination.
type stagedSegment struct {
	dstOff   int64
	stageOff int64
	length   int64
}

// run fetches every batch of the plan, staging the bytes on disk. It returns
// before anything is written to the destination.
func (f *rangeFetch) run() error {
	dir, err := os.MkdirTemp(f.opts.TempDir, "grab-ranges-")
	if err != nil {
		return fmt.Errorf("grab: cannot create staging directory: %w", err)
	}
	f.stageDir = dir
	file, err := os.CreateTemp(dir, "data-*")
	if err != nil {
		return fmt.Errorf("grab: cannot create staging file: %w", err)
	}
	f.stageFile = file
	f.buf = make([]byte, 32*1024)

	for start := 0; start < len(f.plan); start += f.opts.BatchSize {
		end := start + f.opts.BatchSize
		if end > len(f.plan) {
			end = len(f.plan)
		}
		full, err := f.fetchBatch(f.plan[start:end], start == 0)
		if err != nil {
			return err
		}
		if full {
			// The server answered the first batch with the complete
			// representation; every planned range was extracted from it.
			break
		}
	}
	return nil
}

// cleanup closes the staging file and removes the staging directory created
// by run. It is called exactly once per FetchRanges call, on success and on
// failure alike.
func (f *rangeFetch) cleanup() {
	if f.stageFile != nil {
		f.stageFile.Close()
		f.stageFile = nil
	}
	if f.stageDir != "" {
		os.RemoveAll(f.stageDir)
		f.stageDir = ""
	}
}

// fetchBatch sends one ranged request for the given batch of the plan and
// stages the response. It reports full=true when the server returned the
// complete representation (only legal for the first batch), in which case no
// further batches may be requested.
func (f *rangeFetch) fetchBatch(batch []ByteRange, first bool) (full bool, err error) {
	if err := f.ctx.Err(); err != nil {
		return false, err
	}

	req := f.template.Clone(f.ctx)
	req.Header.Set("Range", rangeHeaderValue(batch))
	req.Header.Set("Accept-Encoding", "identity")
	if f.etag != "" {
		req.Header.Set("If-Match", f.etag)
	}

	resp, err := f.client.doHTTPRequest(req)
	if err != nil {
		return false, err
	}
	if resp.Body == nil {
		resp.Body = http.NoBody
	}
	defer resp.Body.Close()

	if enc := resp.Header.Get("Content-Encoding"); enc != "" && !strings.EqualFold(enc, "identity") {
		return false, fmt.Errorf("grab: unsupported content encoding %q", enc)
	}

	switch resp.StatusCode {
	case http.StatusOK:
		if !first {
			return false, fmt.Errorf("grab: server answered a later range batch with %d %s",
				resp.StatusCode, http.StatusText(resp.StatusCode))
		}
		return true, f.fetchFull(resp)
	case http.StatusPartialContent:
		return false, f.fetchPartial(resp, batch, first)
	case http.StatusPreconditionFailed:
		return false, fmt.Errorf("grab: server rejected If-Match precondition: %w", ErrRangeRepresentationChanged)
	default:
		return false, StatusCodeError(resp.StatusCode)
	}
}

// fetchFull consumes a 200 OK response to the first batch as the complete
// representation, staging the intersection of every planned range with it.
// The body is read to EOF to determine and verify the representation length.
func (f *rangeFetch) fetchFull(resp *http.Response) error {
	if et := strongETag(resp.Header.Get("ETag")); et != "" {
		f.etag = et
	}
	var pos int64
	for _, r := range f.plan {
		if err := f.skip(resp.Body, r.Start-pos); err != nil {
			return fmt.Errorf("grab: reading full representation: %w", err)
		}
		pos = r.Start
		if err := f.stage(resp.Body, r.Start, r.End-r.Start); err != nil {
			return fmt.Errorf("grab: extracting range [%d,%d) from full representation: %w",
				r.Start, r.End, err)
		}
		pos = r.End
	}
	rest, err := f.countToEOF(resp.Body)
	if err != nil {
		return fmt.Errorf("grab: reading full representation: %w", err)
	}
	total := pos + rest
	if resp.ContentLength >= 0 && resp.ContentLength != total {
		return fmt.Errorf("grab: full representation is %d bytes but Content-Length promised %d",
			total, resp.ContentLength)
	}
	f.size = total
	return nil
}

// fetchPartial validates and stages a 206 Partial Content response, either a
// single Content-Range body or a multipart/byteranges body. Every byte of
// every requested range in the batch must be covered exactly once; parts may
// arrive out of order and may coalesce requested ranges into larger spans
// whose gaps are skipped.
func (f *rangeFetch) fetchPartial(resp *http.Response, batch []ByteRange, first bool) error {
	et := strongETag(resp.Header.Get("ETag"))
	if first {
		if f.multi && et == "" {
			return fmt.Errorf("grab: multi-batch fetch requires a strong ETag on the first response: %w",
				ErrRangeRepresentationChanged)
		}
		f.etag = et
	} else if et == "" || et != f.etag {
		return fmt.Errorf("grab: ETag changed between batches: %w", ErrRangeRepresentationChanged)
	}

	covered := make([]int64, len(batch))
	var received []inclusiveRange

	process := func(p responsePart) error {
		if f.size == -1 {
			f.size = p.total
			for _, r := range f.plan {
				if r.End > f.size {
					return fmt.Errorf("grab: planned range [%d,%d) exceeds resource size %d",
						r.Start, r.End, f.size)
				}
			}
		} else if p.total != f.size {
			return fmt.Errorf("grab: total size changed from %d to %d: %w",
				f.size, p.total, ErrRangeRepresentationChanged)
		}
		if p.start < batch[0].Start || p.end >= batch[len(batch)-1].End {
			return fmt.Errorf("grab: response range %d-%d lies outside the requested batch",
				p.start, p.end)
		}
		for _, q := range received {
			if p.start <= q.end && q.start <= p.end {
				return fmt.Errorf("grab: response range %d-%d overlaps response range %d-%d",
					p.start, p.end, q.start, q.end)
			}
		}
		received = append(received, inclusiveRange{start: p.start, end: p.end})

		pos := p.start
		intersected := false
		for i, r := range batch {
			is := max(p.start, r.Start)
			ie := min(p.end+1, r.End)
			if is >= ie {
				continue
			}
			intersected = true
			if err := f.skip(p.body, is-pos); err != nil {
				return fmt.Errorf("grab: reading range %d-%d: %w", p.start, p.end, err)
			}
			if err := f.stage(p.body, is, ie-is); err != nil {
				return fmt.Errorf("grab: reading range %d-%d: %w", p.start, p.end, err)
			}
			covered[i] += ie - is
			pos = ie
		}
		if !intersected {
			return fmt.Errorf("grab: response range %d-%d does not intersect the requested batch",
				p.start, p.end)
		}
		if err := f.skip(p.body, p.end+1-pos); err != nil {
			return fmt.Errorf("grab: reading range %d-%d: %w", p.start, p.end, err)
		}
		extra, err := f.countToEOF(p.body)
		if err != nil {
			return fmt.Errorf("grab: reading range %d-%d: %w", p.start, p.end, err)
		}
		if extra != 0 {
			return fmt.Errorf("grab: range %d-%d carries %d more bytes than declared",
				p.start, p.end, extra)
		}
		return nil
	}

	if ct := resp.Header.Get("Content-Type"); strings.HasPrefix(strings.ToLower(ct), "multipart/") {
		mt, params, err := mime.ParseMediaType(ct)
		if err != nil || !strings.EqualFold(mt, "multipart/byteranges") {
			return fmt.Errorf("grab: unsupported multipart content type %q", ct)
		}
		boundary := params["boundary"]
		if boundary == "" {
			return errors.New("grab: multipart response is missing its boundary")
		}
		mr := multipart.NewReader(resp.Body, boundary)
		for {
			pr, err := mr.NextPart()
			if err == io.EOF {
				break
			}
			if err != nil {
				return fmt.Errorf("grab: reading multipart response: %w", err)
			}
			crs := pr.Header.Values("Content-Range")
			if len(crs) != 1 {
				pr.Close()
				return fmt.Errorf("grab: multipart section carries %d Content-Range headers, want 1",
					len(crs))
			}
			start, end, total, err := parseContentRange(crs[0])
			if err != nil {
				pr.Close()
				return err
			}
			err = process(responsePart{start: start, end: end, total: total, body: pr})
			pr.Close()
			if err != nil {
				return err
			}
		}
	} else {
		crs := resp.Header.Values("Content-Range")
		if len(crs) != 1 {
			return fmt.Errorf("grab: 206 response carries %d Content-Range headers, want 1", len(crs))
		}
		start, end, total, err := parseContentRange(crs[0])
		if err != nil {
			return err
		}
		if span := end - start + 1; resp.ContentLength >= 0 && resp.ContentLength != span {
			return fmt.Errorf("grab: Content-Length %d does not match content range span %d",
				resp.ContentLength, span)
		}
		if err := process(responsePart{start: start, end: end, total: total, body: resp.Body}); err != nil {
			return err
		}
	}

	for i, r := range batch {
		if want := r.End - r.Start; covered[i] != want {
			return fmt.Errorf("grab: requested range [%d,%d) was covered %d of %d bytes",
				r.Start, r.End, covered[i], want)
		}
	}
	return nil
}

// publish writes every staged segment to the destination at its original
// resource offset. It runs only after all batches have been received and
// validated. A destination failure is returned without rolling back the
// bytes already published.
func (f *rangeFetch) publish() error {
	if err := f.ctx.Err(); err != nil {
		return err
	}
	segments := f.segments
	sort.Slice(segments, func(i, j int) bool { return segments[i].dstOff < segments[j].dstOff })
	for _, seg := range segments {
		stageOff := seg.stageOff
		dstOff := seg.dstOff
		rem := seg.length
		for rem > 0 {
			m := int64(len(f.buf))
			if rem < m {
				m = rem
			}
			if _, err := f.stageFile.ReadAt(f.buf[:m], stageOff); err != nil {
				return fmt.Errorf("grab: reading staged data: %w", err)
			}
			nw, err := f.dst.WriteAt(f.buf[:m], dstOff)
			if err != nil {
				return fmt.Errorf("grab: publishing to destination: %w", err)
			}
			if nw != int(m) {
				return fmt.Errorf("grab: publishing to destination: %w", io.ErrShortWrite)
			}
			stageOff += m
			dstOff += m
			rem -= m
		}
	}
	return nil
}

// stage copies exactly n bytes from src into the staging file and records a
// segment placing them at dstOff in the destination.
func (f *rangeFetch) stage(src io.Reader, dstOff, n int64) error {
	seg := stagedSegment{dstOff: dstOff, stageOff: f.stageLen, length: n}
	written, err := copyN(f.stageFile, src, n, f.buf)
	f.stageLen += written
	if err != nil {
		return err
	}
	f.segments = append(f.segments, seg)
	return nil
}

// skip reads and discards exactly n bytes from src.
func (f *rangeFetch) skip(src io.Reader, n int64) error {
	_, err := copyN(io.Discard, src, n, f.buf)
	return err
}

// countToEOF drains src and returns the number of bytes read.
func (f *rangeFetch) countToEOF(src io.Reader) (int64, error) {
	var total int64
	for {
		nr, err := src.Read(f.buf)
		total += int64(nr)
		if err == io.EOF {
			return total, nil
		}
		if err != nil {
			return total, err
		}
		if nr == 0 {
			return total, io.ErrNoProgress
		}
	}
}

// copyN copies exactly n bytes from src to dst through buf. A source that
// ends before delivering n bytes is reported as io.ErrUnexpectedEOF; an EOF
// accompanying the final bytes of a read is a normal completion.
func copyN(dst io.Writer, src io.Reader, n int64, buf []byte) (int64, error) {
	var written int64
	for written < n {
		m := int64(len(buf))
		if rem := n - written; rem < m {
			m = rem
		}
		nr, rerr := src.Read(buf[:m])
		if nr > 0 {
			nw, werr := dst.Write(buf[:nr])
			written += int64(nw)
			if werr != nil {
				return written, werr
			}
			if nw != nr {
				return written, io.ErrShortWrite
			}
		}
		if written >= n {
			return written, nil
		}
		if rerr != nil {
			if rerr == io.EOF {
				return written, io.ErrUnexpectedEOF
			}
			return written, rerr
		}
		if nr == 0 {
			return written, io.ErrNoProgress
		}
	}
	return written, nil
}

// responsePart is one received span of a 206 response: either the whole
// single-range body or one section of a multipart/byteranges body.
type responsePart struct {
	start, end int64 // inclusive, as declared by Content-Range
	total      int64 // declared total representation length
	body       io.Reader
}

// inclusiveRange is a closed interval used for response overlap detection.
type inclusiveRange struct {
	start, end int64
}

// rangeHeaderValue formats a batch of ranges for the Range request header,
// translating half-open ends to the inclusive ends used on the wire.
func rangeHeaderValue(batch []ByteRange) string {
	var sb strings.Builder
	sb.WriteString("bytes=")
	for i, r := range batch {
		if i > 0 {
			sb.WriteByte(',')
		}
		fmt.Fprintf(&sb, "%d-%d", r.Start, r.End-1)
	}
	return sb.String()
}

// parseContentRange parses a "bytes start-end/total" Content-Range value.
// The total length must be explicit; "*" is rejected.
func parseContentRange(v string) (start, end, total int64, err error) {
	bad := func() (int64, int64, int64, error) {
		return 0, 0, 0, fmt.Errorf("grab: invalid Content-Range %q", v)
	}
	v = strings.TrimSpace(v)
	if len(v) < len("bytes ") || !strings.EqualFold(v[:len("bytes ")], "bytes ") {
		return bad()
	}
	v = v[len("bytes "):]
	slash := strings.IndexByte(v, '/')
	if slash < 0 {
		return bad()
	}
	span, totalStr := v[:slash], v[slash+1:]
	if totalStr == "" || totalStr == "*" {
		// The total representation length must be explicit.
		return bad()
	}
	if total, err = strconv.ParseInt(totalStr, 10, 64); err != nil || total < 1 {
		return bad()
	}
	dash := strings.IndexByte(span, '-')
	if dash < 0 {
		return bad()
	}
	if start, err = strconv.ParseInt(span[:dash], 10, 64); err != nil || start < 0 {
		return bad()
	}
	if end, err = strconv.ParseInt(span[dash+1:], 10, 64); err != nil || end < start || end >= total {
		return bad()
	}
	return start, end, total, nil
}

// strongETag returns v if it is a strong entity tag, otherwise "".
func strongETag(v string) string {
	v = strings.TrimSpace(v)
	if len(v) >= 2 && v[0] == '"' && v[len(v)-1] == '"' {
		return v
	}
	return ""
}
