package robotstxt

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// --- Controllable Readers -------------------------------------------------

// chunkReader delivers its data in bounded Reads. chunk <= 0 means all
// remaining bytes in one Read. The final bytes can be delivered together
// with io.EOF or a custom error to exercise the (n>0, err) contract.
type chunkReader struct {
	data        []byte
	off         int
	chunk       int
	lastErr     error // returned at end of data; nil means io.EOF
	errWithData bool  // deliver lastErr/io.EOF together with the final bytes
}

func (r *chunkReader) Read(p []byte) (int, error) {
	if r.off >= len(r.data) {
		if r.lastErr != nil {
			return 0, r.lastErr
		}
		return 0, io.EOF
	}
	n := len(r.data) - r.off
	if r.chunk > 0 && n > r.chunk {
		n = r.chunk
	}
	if len(p) < n {
		n = len(p)
	}
	copy(p, r.data[r.off:r.off+n])
	r.off += n

	if r.off >= len(r.data) {
		switch {
		case r.errWithData && r.lastErr != nil:
			return n, r.lastErr
		case r.errWithData:
			return n, io.EOF
		case r.lastErr != nil:
			// Final data arrives cleanly; next Read surfaces the error.
			return n, nil
		}
	}
	return n, nil
}

// splitReader makes one Read of size first (possibly 0), then serves one
// byte per Read, so every byte position can be a split point.
type splitReader struct {
	data      []byte
	off       int
	first     int
	firstDone bool
}

func (r *splitReader) Read(p []byte) (int, error) {
	if r.off >= len(r.data) {
		return 0, io.EOF
	}
	if !r.firstDone {
		r.firstDone = true
		n := r.first
		if n > len(r.data) {
			n = len(r.data)
		}
		if len(p) < n {
			n = len(p)
		}
		copy(p, r.data[:n])
		r.off = n
		return n, nil
	}
	p[0] = r.data[r.off]
	r.off++
	return 1, nil
}

// errorAfterReader serves prefix cleanly, then fails with err on every
// following Read. It can also hand over a few final bytes together with
// the error.
type errorAfterReader struct {
	data         []byte
	err          error
	errAt        int // byte offset at which the error starts
	bytesWithErr int // bytes delivered together with the first error Read
	off          int
	done         bool
}

func (r *errorAfterReader) Read(p []byte) (int, error) {
	if r.off < r.errAt {
		n := r.errAt - r.off
		if len(p) < n {
			n = len(p)
		}
		copy(p, r.data[r.off:r.off+n])
		r.off += n
		return n, nil
	}
	if !r.done && r.bytesWithErr > 0 && r.off < len(r.data) {
		n := len(r.data) - r.off
		if n > r.bytesWithErr {
			n = r.bytesWithErr
		}
		if len(p) < n {
			n = len(p)
		}
		copy(p, r.data[r.off:r.off+n])
		r.off += n
		r.done = true
		return n, r.err
	}
	r.done = true
	return 0, r.err
}

// noProgressReader first serves head, then keeps returning (0, nil).
type noProgressReader struct {
	head []byte
	off  int
}

func (r *noProgressReader) Read(p []byte) (int, error) {
	if r.off < len(r.head) {
		n := copy(p, r.head[r.off:])
		r.off += n
		return n, nil
	}
	return 0, nil
}

// trackingReadCloser records whether Close was called.
type trackingReadCloser struct {
	io.Reader
	closed bool
}

func (c *trackingReadCloser) Close() error {
	c.closed = true
	return nil
}

// generatedRobots synthesizes a very large robots.txt without ever holding
// more than one generated line in memory.
type generatedRobots struct {
	commentRounds int
	round         int
	line          []byte
	phase         int
	longTokenLen  int
	produced      int
}

func (g *generatedRobots) nextLine() []byte {
	switch g.phase {
	case 0:
		g.phase++
		return []byte("User-agent: *\nUser-agent: padbot\nDisallow: /a\n")
	case 1:
		if g.round < g.commentRounds {
			g.round++
			return []byte(fmt.Sprintf("# padding comment line %06d: drop me as you read\n", g.round))
		}
		g.phase++
		fallthrough
	case 2:
		g.phase++
		return []byte("Allow: /b\nSitemap: https://example.test/sitemap.xml\nCrawl-delay: 2\n")
	case 3:
		if g.longTokenLen > 0 {
			g.phase++
			return []byte("Disallow: /" + strings.Repeat("x", g.longTokenLen) + "\n")
		}
		g.phase++
		return nil
	default:
		return nil
	}
}

func (g *generatedRobots) Read(p []byte) (int, error) {
	for len(g.line) == 0 {
		g.line = g.nextLine()
		if g.line == nil {
			return 0, io.EOF
		}
	}
	n := copy(p, g.line)
	g.line = g.line[n:]
	g.produced += n
	return n, nil
}

// --- Comparison helpers ---------------------------------------------------

var probeAgents = []string{"", "*", "a", "b", "c", "bot", "Googlebot", "wall-e", "padbot"}
var probePaths = []string{
	"", "/", "/a", "/b", "/x", "/admin", "/account", "/fish", "/fish.html",
	"/news", "/private", "/私有/secret", "/私有/public/page",
	"/path/to/whatever", "/index.php?option=com_phorum",
}

func assertSamePolicy(t *testing.T, want, got *RobotsData) {
	t.Helper()

	if want == nil || got == nil {
		require.Equal(t, want == nil, got == nil)
		return
	}

	assert.Equal(t, want.allowAll, got.allowAll, "allowAll flag")
	assert.Equal(t, want.disallowAll, got.disallowAll, "disallowAll flag")
	assert.Equal(t, want.Host, got.Host, "Host")
	assert.Equal(t, want.Sitemaps, got.Sitemaps, "Sitemaps")

	require.Equal(t, len(want.groups), len(got.groups), "group count")
	for name, wg := range want.groups {
		gg, ok := got.groups[name]
		require.True(t, ok, "group %q missing", name)
		assert.Equal(t, wg.Agent, gg.Agent, "group %q Agent", name)
		assert.Equal(t, wg.CrawlDelay, gg.CrawlDelay, "group %q CrawlDelay", name)
		assert.Equal(t, wg.disallowAll, gg.disallowAll, "group %q disallowAll", name)
		require.Equal(t, len(wg.rules), len(gg.rules), "group %q rule count", name)
		for i, wr := range wg.rules {
			gr := gg.rules[i]
			assert.Equal(t, wr.path, gr.path, "group %q rule %d path", name, i)
			assert.Equal(t, wr.allow, gr.allow, "group %q rule %d allow", name, i)
			if wr.pattern == nil {
				assert.Nil(t, gr.pattern, "group %q rule %d pattern", name, i)
			} else {
				require.NotNil(t, gr.pattern, "group %q rule %d pattern", name, i)
				assert.Equal(t, wr.pattern.String(), gr.pattern.String(),
					"group %q rule %d pattern", name, i)
			}
		}
	}

	for _, agent := range probeAgents {
		wg := want.FindGroup(agent)
		gg := got.FindGroup(agent)
		require.NotNil(t, gg)
		assert.Equal(t, wg.Agent, gg.Agent, "FindGroup(%q).Agent", agent)
		assert.Equal(t, wg.CrawlDelay, gg.CrawlDelay, "FindGroup(%q).CrawlDelay", agent)
		for _, path := range probePaths {
			assert.Equal(t, want.TestAgent(path, agent), got.TestAgent(path, agent),
				"TestAgent path=%q agent=%q", path, agent)
			assert.Equal(t, wg.Test(path), gg.Test(path),
				"Group.Test path=%q agent=%q", path, agent)
		}
	}
}

func assertSameError(t *testing.T, want, got error) {
	t.Helper()
	if want == nil {
		require.NoError(t, got)
		return
	}
	require.Error(t, got)
	assert.Equal(t, want.Error(), got.Error())

	var wp, gp *ParseError
	wantIsParse := errors.As(want, &wp)
	gotIsParse := errors.As(got, &gp)
	assert.Equal(t, wantIsParse, gotIsParse, "ParseError type mismatch")
	if wantIsParse {
		require.Len(t, gp.Errs, len(wp.Errs))
		for i := range wp.Errs {
			assert.Equal(t, wp.Errs[i].Error(), gp.Errs[i].Error(), "wrapped error #%d", i)
		}
	}
}

func streamVariants(data []byte) map[string]io.Reader {
	variants := map[string]io.Reader{
		"one-shot":        &chunkReader{data: data, chunk: 0},
		"byte-at-a-time":  &chunkReader{data: data, chunk: 1},
		"two-at-a-time":   &chunkReader{data: data, chunk: 2},
		"seven-at-a-time": &chunkReader{data: data, chunk: 7},
		"data-plus-eof":   &chunkReader{data: data, chunk: 1, errWithData: true},
	}
	return variants
}

// --- Tests ----------------------------------------------------------------

var equivalenceSamples = map[string]string{
	"empty":              "",
	"whitespace-only":    "   \n\t\r\n  \v ",
	"comment-only":       "# just a comment\n# another one\r\n",
	"no-final-newline":   "User-agent: bot\nDisallow: /x",
	"empty-directives":   "Disallow:\nAllow:\nUser-agent: *\nDisallow: \n",
	"bom-crlf-unicode":   "\xef\xbb\xbfUser-agent: a\r\nUser-agent: b\r\nDisallow: /私有\r\nAllow: /私有/public\r\nCrawl-delay: 0.25\r\nSitemap: https://site.test/map.xml\r\n",
	"grouping":           streamGrouping,
	"matching":           robotsCaseMatching,
	"precedence":         robotsCasePrecedence,
	"sitemaps":           streamSitemaps,
	"crawl-delays":       streamDelays,
	"wildcard":           streamWildcard,
	"vanityfair":         robotsTextVanityfair,
	"simple-groups":      streamSimple,
	"robotstxt-org":      streamText005,
	"html-instead":       robotsTextJustHTML,
	"invalid-utf8":       "User-agent: H\xef\xbf\xbdm�h�kki\nDisallow: *",
	"parse-disallow":     "Disallow: /\nUser-agent: bot",
	"parse-crawl-syntax": "User-agent: bot\nCrawl-delay: bad-time-value",
	"parse-crawl-inf":    "User-agent: bot\nCrawl-delay: -inf",
	"colon-in-url":       "Sitemap: http://example.test/a:b/c\nUser-agent: *\nDisallow: /\n",
	"repeated-groups":    "user-agent: b\nDisallow: /one\nuser-agent: b\nDisallow: /two\n",
	"big-google":         robotsGoogle,
	"big-standard":       robotsText001,
}

const (
	streamGrouping = `user-agent: a
user-agent: b
disallow: /a
disallow: /b

user-agent: ignore
Disallow: /separator

user-agent: b
user-agent: c
disallow: /b
disallow: /c`

	streamSitemaps = `sitemap: http://test.com/a
user-agent: a
disallow: /c
sitemap: http://test.com/b
user-agent: b
disallow: /d
user-agent: e
sitemap: http://test.com/c
user-agent: f
disallow: /g`

	streamDelays = `useragent: a
# some comment : with colon
disallow: /c
user-agent: b
crawldelay: 3.5
disallow: /d
user-agent: e
sitemap: http://test.com/c
user-agent: f
disallow: /g
crawl-delay: 5`

	streamWildcard = `user-agent: *
Disallow: /path*l$`

	streamSimple = `user-agent: wall-e
disallow: /
user-agent: *
allow: /`

	streamText005 = `User-agent: Google
Disallow:
User-agent: *
Disallow: /`
)

func TestReaderStreamEquivalence(t *testing.T) {
	for name, input := range equivalenceSamples {
		wantData, wantErr := FromBytes([]byte(input))
		// FromString must agree as well.
		strData, strErr := FromString(input)
		assertSameError(t, wantErr, strErr)
		assertSamePolicy(t, wantData, strData)

		// Byte-at-a-time and other fixed chunkings, including (n, EOF).
		for vname, r := range streamVariants([]byte(input)) {
			t.Run(name+"/"+vname, func(t *testing.T) {
				gotData, gotErr := FromReader(r)
				assertSameError(t, wantErr, gotErr)
				assertSamePolicy(t, wantData, gotData)
			})
		}

		// Split at every single byte position; small samples only to keep
		// the sweep linear in total corpus size.
		if len(input) <= 512 {
			for k := 0; k <= len(input); k++ {
				t.Run(fmt.Sprintf("%s/split-%d", name, k), func(t *testing.T) {
					gotData, gotErr := FromReader(&splitReader{data: []byte(input), first: k})
					assertSameError(t, wantErr, gotErr)
					assertSamePolicy(t, wantData, gotData)
				})
			}
		} else {
			for _, k := range []int{0, 1, 2, 3, 7, 31, 32, 33, 64, 256, 4096} {
				if k > len(input) {
					continue
				}
				t.Run(fmt.Sprintf("%s/split-%d", name, k), func(t *testing.T) {
					gotData, gotErr := FromReader(&splitReader{data: []byte(input), first: k})
					assertSameError(t, wantErr, gotErr)
					assertSamePolicy(t, wantData, gotData)
				})
			}
		}
	}
}

func TestReaderStreamReaderError(t *testing.T) {
	sentinel := errors.New("sentinel read failure")
	wrapped := fmt.Errorf("wrapped: %w", sentinel)

	const complete = "User-agent: a\nDisallow: /x\nAllow: /x/y\n"
	const halfToken = "User-agent: a\nDisallow: /half-token-without-end"

	t.Run("nil-reader", func(t *testing.T) {
		data, err := FromReader(nil)
		require.Error(t, err)
		assert.Nil(t, data)
		assert.True(t, errors.Is(err, ErrNilReader))
		assert.NotPanics(t, func() { _, _ = FromReader(nil) })
	})

	t.Run("error-after-complete-groups", func(t *testing.T) {
		r := &errorAfterReader{data: []byte(complete + "User-agent: b\n"), err: sentinel, errAt: len(complete)}
		data, err := FromReader(r)
		assert.Nil(t, data)
		require.Error(t, err)
		assert.True(t, errors.Is(err, sentinel))
	})

	t.Run("error-bytes-with-data-half-token", func(t *testing.T) {
		// Error arrives together with the last valid bytes, mid-token.
		r := &errorAfterReader{
			data:         []byte(halfToken),
			err:          wrapped,
			errAt:        len(halfToken) - 4,
			bytesWithErr: 4,
		}
		data, err := FromReader(r)
		assert.Nil(t, data)
		require.Error(t, err)
		assert.True(t, errors.Is(err, sentinel))
		assert.True(t, errors.Is(err, wrapped))
	})

	t.Run("eof-with-data-succeeds", func(t *testing.T) {
		wantData, wantErr := FromBytes([]byte(complete))
		r := &chunkReader{data: []byte(complete), chunk: 3, errWithData: true}
		data, err := FromReader(r)
		assertSameError(t, wantErr, err)
		assertSamePolicy(t, wantData, data)
	})

	t.Run("read-error-beats-syntax-error", func(t *testing.T) {
		// "Disallow before User-agent" is a syntax error; the read error
		// must take precedence.
		bad := "Disallow: /\nUser-agent: bot\n"
		r := &errorAfterReader{data: []byte(bad), err: sentinel, errAt: len(bad)}
		_, syntaxErr := FromBytes([]byte(bad))
		require.Error(t, syntaxErr)
		var pe *ParseError
		require.True(t, errors.As(syntaxErr, &pe))

		data, err := FromReader(r)
		assert.Nil(t, data)
		require.Error(t, err)
		assert.True(t, errors.Is(err, sentinel))
		assert.False(t, errors.As(err, &pe), "read error must not be reported as ParseError")
	})

	t.Run("no-progress", func(t *testing.T) {
		data, err := FromReader(&noProgressReader{})
		assert.Nil(t, data)
		assert.True(t, errors.Is(err, io.ErrNoProgress))

		// Also after a few valid bytes.
		data, err = FromReader(&noProgressReader{head: []byte("User-agent: *\n")})
		assert.Nil(t, data)
		assert.True(t, errors.Is(err, io.ErrNoProgress))
	})

	t.Run("reader-is-not-closed", func(t *testing.T) {
		body := &trackingReadCloser{Reader: strings.NewReader(complete)}
		_, err := FromReader(body)
		require.NoError(t, err)
		assert.False(t, body.closed, "FromReader must not close the Reader")
	})

	t.Run("error-content-preserved", func(t *testing.T) {
		r := &errorAfterReader{data: []byte(complete), err: sentinel, errAt: 3}
		_, err := FromReader(r)
		require.EqualError(t, err, sentinel.Error())
	})
}

func TestReaderStreamBounded(t *testing.T) {
	// Longest legitimate token is fixed up front; irrelevant comment volume
	// is multiplied below and must not move any high-water mark.
	const longTokenLen = 128 * 1024

	type mark struct {
		maxRead          int
		maxRetained      int
		maxPendingTokens int
	}
	run := func(rounds int) mark {
		r := &generatedRobots{commentRounds: rounds, longTokenLen: longTokenLen}
		data, err, stats := parseStreamStats(r)
		require.NoError(t, err)
		require.NotNil(t, data)
		// Sanity: the directives embedded in the generated stream are live.
		assert.False(t, data.TestAgent("/a", "padbot"))
		assert.True(t, data.TestAgent("/b", "padbot"))
		assert.Equal(t, 2*time.Second, data.FindGroup("padbot").CrawlDelay)
		assert.Equal(t, []string{"https://example.test/sitemap.xml"}, data.Sitemaps)
		// The long token is accepted: there is no hidden line length limit.
		assert.NotEmpty(t, data.FindGroup("padbot").rules)
		return mark{
			maxRead:          stats.MaxReadSize,
			maxRetained:      stats.MaxRetained,
			maxPendingTokens: stats.MaxPendingTokens,
		}
	}

	small := run(100)
	medium := run(1000)
	large := run(10000) // 100x the small run of irrelevant input
	t.Logf("high-water marks small=%+v medium=%+v large=%+v (read buffer cap=%d, long token=%d)",
		small, medium, large, maxReadBuffer, longTokenLen)

	// Every underlying Read requests at most 32 KiB.
	for _, m := range []mark{small, medium, large} {
		if m.maxRead > maxReadBuffer {
			t.Errorf("max Read size %d exceeds %d", m.maxRead, maxReadBuffer)
		}
		if m.maxRead == 0 {
			t.Error("no Read was ever requested")
		}
	}

	// The parser only ever needs one token of look-ahead.
	if large.maxPendingTokens > 1 {
		t.Errorf("pending tokens high-water %d exceeds 1", large.maxPendingTokens)
	}

	// Fixed upper bound: one read buffer plus at most the in-progress token
	// and one pending token copy, with a small constant slack.
	bound := maxReadBuffer + 2*longTokenLen + 128
	for _, m := range []mark{small, medium, large} {
		if m.maxRetained > bound {
			t.Errorf("retained bytes %d exceed fixed bound %d", m.maxRetained, bound)
		}
	}

	// Irrelevant input growing 100x must not move the high-water marks.
	assert.Equal(t, small.maxRetained, large.maxRetained,
		"retained bytes must not grow with comment volume")
	assert.Equal(t, medium.maxRetained, large.maxRetained)
	assert.Equal(t, small.maxPendingTokens, large.maxPendingTokens)

	// Without the long token, retained memory stays near one read buffer
	// even for the huge comment stream.
	plain := &generatedRobots{commentRounds: 10000}
	_, err, stats := parseStreamStats(plain)
	require.NoError(t, err)
	t.Logf("plain stream high-water: read=%d retained=%d pendingTokens=%d",
		stats.MaxReadSize, stats.MaxRetained, stats.MaxPendingTokens)
	if stats.MaxRetained > maxReadBuffer+256 {
		t.Errorf("retained bytes %d exceed %d; comments must be discarded while reading",
			stats.MaxRetained, maxReadBuffer+256)
	}
	if stats.MaxReadSize > maxReadBuffer {
		t.Errorf("max Read size %d exceeds %d", stats.MaxReadSize, maxReadBuffer)
	}
}

func TestReaderStreamResponse(t *testing.T) {
	const body = "\xef\xbb\xbfUser-agent: a\r\nUser-agent: b\r\nDisallow: /私有\r\nAllow: /私有/public\r\n"

	newResponse := func(status int, r io.Reader) *http.Response {
		return &http.Response{
			StatusCode:    status,
			Proto:         "HTTP/1.1",
			ProtoMajor:    1,
			ProtoMinor:    1,
			Body:          &trackingReadCloser{Reader: r},
			ContentLength: -1,
		}
	}

	t.Run("200-streaming-parse", func(t *testing.T) {
		res := newResponse(200, &chunkReader{data: []byte(body), chunk: 1, errWithData: true})
		data, err := FromResponse(res)
		require.NoError(t, err)
		require.NotNil(t, data)

		ref, refErr := FromStatusAndBytes(200, []byte(body))
		require.NoError(t, refErr)
		assertSamePolicy(t, ref, data)

		assert.True(t, data.TestAgent("/私有/public/page", "b"))
		assert.False(t, data.TestAgent("/私有/secret", "a"))
		g := data.FindGroup("b")
		assert.False(t, g.Test("/私有/secret"))
		assert.True(t, g.Test("/私有/public/page"))
		assert.False(t, res.Body.(*trackingReadCloser).closed, "Body Close stays with the caller")
	})

	t.Run("status-matrix", func(t *testing.T) {
		cases := []struct {
			code    int
			allow   bool
			wantErr bool
		}{
			{200, true, false},
			{204, true, false},
			{400, true, false},
			{401, true, false},
			{403, true, false},
			{404, true, false},
			{500, false, false},
			{502, false, false},
			{503, false, false},
			{301, false, true},
			{600, false, true},
		}
		for _, c := range cases {
			t.Run(fmt.Sprint(c.code), func(t *testing.T) {
				res := newResponse(c.code, &chunkReader{data: []byte(body), chunk: 5})
				data, err := FromResponse(res)
				if c.wantErr {
					require.Error(t, err)
					assert.Nil(t, data)
					assert.Contains(t, err.Error(), "Unexpected status")
				} else {
					require.NoError(t, err)
					require.NotNil(t, data)
					assert.Equal(t, c.allow, data.TestAgent("/anything", "a"))
				}
				assert.False(t, res.Body.(*trackingReadCloser).closed)
			})
		}
	})

	t.Run("body-fully-drained-on-non-2xx", func(t *testing.T) {
		src := &generatedRobots{commentRounds: 5000}
		counting := &countingReader{r: src}
		res := newResponse(404, counting)
		data, err := FromResponse(res)
		require.NoError(t, err)
		assert.True(t, data.allowAll)
		assert.Equal(t, src.produced, counting.read, "4xx body must be read to the end")

		src = &generatedRobots{commentRounds: 5000}
		counting = &countingReader{r: src}
		res = newResponse(503, counting)
		data, err = FromResponse(res)
		require.NoError(t, err)
		assert.True(t, data.disallowAll)
		assert.Equal(t, src.produced, counting.read)
	})

	t.Run("read-error-precedence", func(t *testing.T) {
		sentinel := errors.New("body exploded")
		for _, code := range []int{200, 404, 500, 302} {
			t.Run(fmt.Sprint(code), func(t *testing.T) {
				r := &errorAfterReader{data: []byte(body), err: sentinel, errAt: 7}
				res := newResponse(code, r)
				data, err := FromResponse(res)
				assert.Nil(t, data)
				require.Error(t, err)
				assert.True(t, errors.Is(err, sentinel))
			})
		}
	})

	t.Run("nil-response", func(t *testing.T) {
		data, err := FromResponse(nil)
		assert.Nil(t, data)
		assert.NoError(t, err)
	})

	t.Run("from-status-and-bytes-unchanged", func(t *testing.T) {
		direct, err := FromStatusAndBytes(200, []byte(body))
		require.NoError(t, err)
		streamed, err := FromResponse(newResponse(200, bytes.NewReader([]byte(body))))
		require.NoError(t, err)
		assertSamePolicy(t, direct, streamed)

		allow, err := FromStatusAndBytes(404, []byte(body))
		require.NoError(t, err)
		assert.True(t, allow.allowAll)
		deny, err := FromStatusAndBytes(500, []byte(body))
		require.NoError(t, err)
		assert.True(t, deny.disallowAll)
	})
}

// countingReader counts how many bytes the consumer actually pulled.
type countingReader struct {
	r    io.Reader
	read int
}

func (c *countingReader) Read(p []byte) (int, error) {
	n, err := c.r.Read(p)
	c.read += n
	return n, err
}
