// Copyright (c) 2019 FOSS contributors of https://github.com/nxadm/tail
// Copyright (c) 2015 HPE Software Inc. All rights reserved.
// Copyright (c) 2013 ActiveState Software Inc. All rights reserved.

package tail

import (
	"bytes"
	"errors"
	"io"
	"time"
)

// errDelimiterWithoutCompleteLines is returned by TailFile when a custom
// Delimiter is configured together with CompleteLines left false.
var errDelimiterWithoutCompleteLines = errors.New(
	"tail: Config.Delimiter requires Config.CompleteLines to be true")

// delimiterReadSize is the size of the scratch buffer used to drain the
// buffered reader in delimiter mode.
const delimiterReadSize = 4096

// delimiterSplitter accumulates raw bytes and splits them into records on a
// fixed literal delimiter. All matching is byte-wise: the delimiter is taken
// literally (it is never a regular expression or character set) and may
// contain NUL bytes or multi-byte UTF-8 sequences without any rune
// reinterpretation.
type delimiterSplitter struct {
	delim []byte
	buf   []byte
}

// write appends freshly read bytes to the internal buffer.
func (s *delimiterSplitter) write(p []byte) {
	s.buf = append(s.buf, p...)
}

// reset drops every buffered byte. It must be called when the underlying
// file is replaced or truncated so that an unfinished record or a partially
// matched delimiter from the previous file never spills into the new one.
func (s *delimiterSplitter) reset() {
	s.buf = s.buf[:0]
}

// buffered reports the number of bytes that have not been consumed as part
// of a complete record yet.
func (s *delimiterSplitter) buffered() int {
	return len(s.buf)
}

// records removes and returns every complete record currently buffered.
// Delimiters are consumed left-to-right without overlapping; consecutive or
// leading delimiters yield empty records and the delimiters themselves are
// never included in the returned text. Bytes after the last delimiter,
// including a partially matched delimiter prefix, remain buffered and are
// reused on the next write, regardless of where a read boundary fell.
func (s *delimiterSplitter) records() []string {
	var records []string
	rest := s.buf
	for {
		i := bytes.Index(rest, s.delim)
		if i < 0 {
			break
		}
		records = append(records, string(rest[:i]))
		rest = rest[i+len(s.delim):]
	}
	if len(rest) == len(s.buf) {
		// Nothing was consumed; leave the buffer untouched.
		return records
	}
	// Move the unconsumed tail to the front of the backing array.
	s.buf = append(s.buf[:0], rest...)
	return records
}

// tail returns the buffered bytes that never reached a delimiter. A file
// ending exactly on a delimiter leaves this empty, so no spurious trailing
// empty record is produced.
func (s *delimiterSplitter) tail() string {
	return string(s.buf)
}

// tailFileRecords drives the read loop when a custom Delimiter is
// configured. It mirrors tailFileSync's newline-based loop but splits the
// byte stream on the literal delimiter instead of on "\n".
func (tail *Tail) tailFileRecords() {
	readBuf := make([]byte, delimiterReadSize)

	for {
		// Drain everything that is currently readable and emit the
		// complete records found in it. Matching happens on the
		// accumulated splitter buffer, so records and delimiters may be
		// split arbitrarily across reader fills and file appends.
		for {
			tail.lk.Lock()
			n, err := tail.reader.Read(readBuf)
			tail.lk.Unlock()

			if n > 0 {
				tail.splitter.write(readBuf[:n])
				for _, record := range tail.splitter.records() {
					if !tail.sendLine(record) {
						if !tail.delimiterCooloff() {
							return
						}
					}
				}
			}

			if err == io.EOF {
				break
			}
			if err != nil {
				tail.Killf("Error reading %s: %s", tail.Filename, err)
				return
			}
		}

		if !tail.Follow {
			// At a final EOF the unfinished tail is the last record.
			// An empty buffer means the file ended on a delimiter (or was
			// empty), so no additional empty record is delivered.
			if tail.splitter.buffered() > 0 {
				tail.sendLine(tail.splitter.tail())
			}
			return
		}

		// Follow mode: an unfinished record, including a half-matched
		// delimiter prefix, is held back at a temporary EOF and waits for
		// the next append, replacement or truncation event.
		if err := tail.waitForChanges(); err != nil {
			if err != ErrStop {
				tail.Kill(err)
			}
			return
		}

		select {
		case <-tail.Dying():
			if tail.Err() == errStopAtEOF {
				continue
			}
			return
		default:
		}
	}
}

// delimiterCooloff mirrors the rate-limiter cool-off period in the
// newline-based read loop. It returns false when the tail is dying or a
// seek error occurred, in which case the caller must return.
func (tail *Tail) delimiterCooloff() bool {
	msg := "Too much log activity; waiting a second before resuming tailing"
	offset, _ := tail.Tell()
	tail.Lines <- &Line{msg, tail.lineNum, SeekInfo{Offset: offset}, time.Now(), errors.New(msg)}
	select {
	case <-time.After(time.Second):
	case <-tail.Dying():
		return false
	}
	if err := tail.seekEnd(); err != nil {
		tail.Kill(err)
		return false
	}
	// seekEnd discarded the reader buffer and every in-flight byte with it.
	tail.splitter.reset()
	return true
}
