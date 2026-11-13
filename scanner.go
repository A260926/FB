package robotstxt

import (
	"bytes"
	"errors"
	"fmt"
	"go/token"
	"io"
	"os"
	"slices"
	"sync"
	"unicode/utf8"
)

const (
	tokEOL = "\n"

	// maxReadBuffer is the fixed size of the scanner read buffer: every
	// single Read request to the underlying Reader asks for at most this
	// many bytes.
	maxReadBuffer = 32 * 1024

	// maxEmptyReads bounds Readers that keep returning (0, nil) without an
	// error, so such a Reader eventually surfaces io.ErrNoProgress instead
	// of looping forever.
	maxEmptyReads = 100
)

var WhitespaceChars = []rune{' ', '\t', '\v'}

// ErrNilReader is returned by FromReader when its Reader argument is nil.
var ErrNilReader = errors.New("robotstxt: nil reader")

var drainBuffers = sync.Pool{New: func() any {
	b := make([]byte, maxReadBuffer)
	return &b
}}

// streamStats holds package-visible high-water marks of how much input a
// streaming parse keeps around. It is intended for tests and diagnostics.
type streamStats struct {
	sc *streamScanner

	// MaxReadSize is the largest buffer ever handed to Reader.Read.
	MaxReadSize int
	// MaxRetained is the high-water count of input bytes retained at any
	// point: unconsumed read buffer, the in-progress token and fully formed
	// tokens not yet consumed by the parser.
	MaxRetained int
	// MaxPendingTokens is the high-water number of produced tokens waiting
	// to be consumed by the parser.
	MaxPendingTokens int

	pendingBytes int
	pendingN     int
}

func (st *streamStats) setPending(n, b int) {
	st.pendingN = n
	st.pendingBytes = b
	st.sample()
}

func (st *streamStats) sample() {
	if st.sc == nil {
		return
	}
	if n := st.sc.retainedBytes() + st.pendingBytes; n > st.MaxRetained {
		st.MaxRetained = n
	}
	if st.pendingN > st.MaxPendingTokens {
		st.MaxPendingTokens = st.pendingN
	}
}

// streamScanner tokenizes a robots.txt stream straight from an io.Reader.
//
// Working memory is a fixed-size read buffer, the single token currently
// being assembled and at most one rune of look-ahead. Comments and blank
// lines are discarded as soon as they are read, without buffering.
type streamScanner struct {
	r io.Reader

	stats streamStats

	// Fixed-size read buffer; unconsumed bytes are buf[start:end].
	buf   [maxReadBuffer]byte
	start int
	end   int

	atEOF   bool
	readErr error
	emptyRd int
	quiet   bool
	ch      rune
	pos     token.Position

	keyTokenFound bool

	// token accumulates only the token currently being scanned.
	token      bytes.Buffer
	tokenCount int
	ErrorCount int
}

func newStreamScanner(srcname string, quiet bool, r io.Reader) *streamScanner {
	s := &streamScanner{
		r:     r,
		quiet: quiet,
		ch:    -1,
		pos:   token.Position{Filename: srcname},
	}
	s.stats.sc = s
	return s
}

// begin loads the first rune and skips a leading UTF-8 byte order mark.
// The BOM is recognized only at the very beginning of the input.
func (s *streamScanner) begin() {
	s.nextChar()
	if s.ch == 65279 {
		s.nextChar()
		s.pos.Column = 1
	}
}

func (s *streamScanner) GetPosition() token.Position {
	return s.pos
}

// err returns the non-EOF error that terminated the underlying Reader, if
// any. Bytes delivered together with that error are still parsed first.
func (s *streamScanner) err() error {
	return s.readErr
}

func (s *streamScanner) emittedTokens() int {
	return s.tokenCount
}

// retainedBytes reports input bytes currently held by the scanner: the
// unconsumed part of the read buffer plus the in-progress token.
func (s *streamScanner) retainedBytes() int {
	return s.end - s.start + s.token.Len()
}

// nextToken returns the next token. tokEOL represents a line boundary.
// ok is false at clean EOF or once a read error has drained the buffer;
// call err() to distinguish the two.
func (s *streamScanner) nextToken() (tok string, ok bool) {
	for {
		s.skipSpace()
		if s.ch == -1 {
			return "", false
		}

		// EOL
		if s.isEol() {
			s.keyTokenFound = false
			// skip subsequent newline chars
			for s.ch != -1 && s.isEol() {
				s.nextChar()
			}
			s.stats.sample()
			s.tokenCount++
			return tokEOL, true
		}

		// skip comments
		if s.ch == '#' {
			s.keyTokenFound = false
			s.skipUntilEol()
			if s.ch == -1 {
				return "", false
			}
			s.stats.sample()
			s.tokenCount++
			return tokEOL, true
		}

		// else we found something
		s.token.Reset()
		_, _ = s.token.WriteRune(s.ch)
		s.nextChar()
		for s.ch != -1 && !s.isSpace() && !s.isEol() {
			// Do not consider ":" to be a token separator if a first key
			// token has already been found on this line (avoid cutting an
			// absolute URL after the "http:").
			if s.ch == ':' && !s.keyTokenFound {
				s.nextChar()
				s.keyTokenFound = true
				break
			}

			_, _ = s.token.WriteRune(s.ch)
			s.nextChar()
		}
		s.stats.sample()
		s.tokenCount++
		return s.token.String(), true
	}
}

func (s *streamScanner) error(pos token.Position, msg string) {
	s.ErrorCount++
	if !s.quiet {
		fmt.Fprintf(os.Stderr, "robotstxt from %s: %s\n", pos.String(), msg)
	}
}

func (s *streamScanner) isEol() bool {
	return s.ch == '\n' || s.ch == '\r'
}

func (s *streamScanner) isSpace() bool {
	return slices.Contains(WhitespaceChars, s.ch)
}

func (s *streamScanner) skipSpace() {
	for s.ch != -1 && s.isSpace() {
		s.nextChar()
	}
}

func (s *streamScanner) skipUntilEol() {
	for s.ch != -1 && !s.isEol() {
		s.nextChar()
	}
	// skip subsequent newline chars
	for s.ch != -1 && s.isEol() {
		s.nextChar()
	}
}

// refill pulls more bytes from the underlying Reader. A non-EOF read error
// is remembered and surfaced only after the bytes delivered alongside it
// have been consumed. io.ErrNoProgress is returned immediately when the
// Reader repeatedly makes no progress.
func (s *streamScanner) refill() error {
	if s.start >= s.end {
		s.start = 0
		s.end = 0
	} else if s.start > 0 {
		// Keep an incomplete UTF-8 sequence at the front of the buffer.
		copy(s.buf[:], s.buf[s.start:s.end])
		s.end -= s.start
		s.start = 0
	}

	for {
		p := s.buf[s.end:]
		if len(p) > s.stats.MaxReadSize {
			s.stats.MaxReadSize = len(p)
		}
		n, err := s.r.Read(p)
		if n > 0 {
			s.end += n
			s.emptyRd = 0
		}
		if err != nil {
			if err == io.EOF {
				s.atEOF = true
				return nil
			}
			s.readErr = err
			s.atEOF = true
			return nil
		}
		if n == 0 {
			s.emptyRd++
			if s.emptyRd > maxEmptyReads {
				s.readErr = io.ErrNoProgress
				s.atEOF = true
				return io.ErrNoProgress
			}
			continue
		}
		s.stats.sample()
		return nil
	}
}

// Reads next Unicode char. Mirrors the historical byte scanner, including
// its lenient treatment of illegal UTF-8 (one U+FFFD per invalid byte) and
// its position bookkeeping.
func (s *streamScanner) nextChar() bool {
	if s.start >= s.end {
		if s.atEOF {
			s.ch = -1
			return false
		}
		if err := s.refill(); err != nil {
			s.ch = -1
			return false
		}
		if s.start >= s.end {
			s.ch = -1
			return false
		}
	}

	s.pos.Column++
	if s.ch == '\n' {
		s.pos.Line++
		s.pos.Column = 1
	}

	r, w := rune(s.buf[s.start]), 1
	if r >= 0x80 {
		p := s.buf[s.start:s.end]
		// An incomplete UTF-8 sequence may simply straddle one or more
		// read boundaries (a Reader can return a single byte at a time);
		// keep pulling bytes until the rune is complete or input ends.
		for !utf8.FullRune(p) && !s.atEOF {
			if err := s.refill(); err != nil {
				break
			}
			p = s.buf[s.start:s.end]
		}
		r, w = utf8.DecodeRune(p)
		if r == utf8.RuneError && w == 1 {
			s.error(s.pos, "illegal UTF-8 encoding")
		}
	}

	s.pos.Column++
	s.pos.Offset += w
	s.start += w
	s.ch = r
	return true
}

// drainReader reads r until EOF without retaining the body. It is used for
// response statuses whose bodies must be consumed but not parsed.
func drainReader(r io.Reader) error {
	bp := drainBuffers.Get().(*[]byte)
	defer drainBuffers.Put(bp)

	empty := 0
	for {
		n, err := r.Read(*bp)
		if n > 0 {
			empty = 0
		}
		if err != nil {
			if err == io.EOF {
				return nil
			}
			return err
		}
		if n == 0 {
			empty++
			if empty > maxEmptyReads {
				return io.ErrNoProgress
			}
		}
	}
}
