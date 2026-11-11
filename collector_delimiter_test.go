package tail

import (
	"bytes"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"time"
)

// receiveRecord waits for the next record with an explicit timeout and
// checks its text. It is the read side of the write/read handshakes used by
// the following behavior tests.
func receiveRecord(t *testing.T, tl *Tail, want string) {
	t.Helper()
	select {
	case line := <-tl.Lines:
		if line == nil {
			t.Fatalf("tail.Lines closed while waiting for %q", want)
		}
		if line.Err != nil {
			t.Fatalf("unexpected line error while waiting for %q: %v", want, line.Err)
		}
		if line.Text != want {
			t.Fatalf("want record %q, got %q", want, line.Text)
		}
	case <-time.After(3 * time.Second):
		t.Fatalf("timed out waiting for record %q", want)
	}
}

// assertNoRecord fails if a record (or an error line) becomes available
// within the given window.
func assertNoRecord(t *testing.T, tl *Tail, wait time.Duration) {
	t.Helper()
	select {
	case line := <-tl.Lines:
		t.Fatalf("unexpected record delivered before a complete delimiter: %+v", line)
	case <-time.After(wait):
	}
}

// collectFinite tails a file without Follow and returns every delivered
// record text.
func collectFinite(t *testing.T, p string, cfg Config) []string {
	t.Helper()
	cfg.MustExist = true
	cfg.Follow = false
	cfg.Logger = DiscardingLogger
	tl, err := TailFile(p, cfg)
	if err != nil {
		t.Fatal(err)
	}
	defer tl.Cleanup()
	var got []string
	for line := range tl.Lines {
		if line.Err != nil {
			t.Fatal(line.Err)
		}
		got = append(got, line.Text)
	}
	if err := tl.Wait(); err != nil {
		t.Fatal(err)
	}
	return got
}

func delimiterBackends(t *testing.T, fn func(t *testing.T, poll bool)) {
	t.Helper()
	for _, poll := range []bool{false, true} {
		name := "notify"
		if poll {
			name = "poll"
		}
		t.Run(name, func(t *testing.T) { fn(t, poll) })
	}
}

// TestCollectorDelimiterSplitterUnit exercises the byte-wise splitting
// rules directly, including feeds one byte at a time, so that read
// boundaries can never influence the result.
func TestCollectorDelimiterSplitterUnit(t *testing.T) {
	t.Run("ABA example", func(t *testing.T) {
		s := &delimiterSplitter{delim: []byte("ABA")}
		for _, b := range []byte("xABABA") {
			s.write([]byte{b})
		}
		if got := s.records(); !reflect.DeepEqual(got, []string{"x"}) {
			t.Fatalf("records = %q", got)
		}
		if s.tail() != "BA" {
			t.Fatalf("tail = %q", s.tail())
		}
	})

	t.Run("END example with empty record and unfinished tail", func(t *testing.T) {
		s := &delimiterSplitter{delim: []byte("<END>")}
		for _, b := range []byte("a<END><END>b<EN") {
			s.write([]byte{b})
		}
		if got := s.records(); !reflect.DeepEqual(got, []string{"a", ""}) {
			t.Fatalf("records = %q", got)
		}
		if s.tail() != "b<EN" {
			t.Fatalf("tail = %q", s.tail())
		}
	})

	t.Run("non-overlapping left-to-right consumption", func(t *testing.T) {
		s := &delimiterSplitter{delim: []byte("AA")}
		s.write([]byte("AAAA"))
		if got := s.records(); !reflect.DeepEqual(got, []string{"", ""}) {
			t.Fatalf("records = %q", got)
		}
		if s.buffered() != 0 {
			t.Fatalf("file ending on a delimiter must not leave a tail: %q", s.tail())
		}
	})

	t.Run("leading and consecutive delimiters and exact end", func(t *testing.T) {
		s := &delimiterSplitter{delim: []byte("|")}
		s.write([]byte("|a||b|"))
		if got := s.records(); !reflect.DeepEqual(got, []string{"", "a", "", "b"}) {
			t.Fatalf("records = %q", got)
		}
		if s.buffered() != 0 {
			t.Fatalf("trailing delimiter must not yield an extra empty record: %q", s.tail())
		}
	})

	t.Run("partial prefix is retained across writes", func(t *testing.T) {
		s := &delimiterSplitter{delim: []byte("<END>")}
		s.write([]byte("x<EN"))
		if got := s.records(); got != nil {
			t.Fatalf("half-matched delimiter produced records: %q", got)
		}
		if s.buffered() != 4 {
			t.Fatalf("buffered = %d, want 4", s.buffered())
		}
		s.write([]byte("D>y"))
		if got := s.records(); !reflect.DeepEqual(got, []string{"x"}) {
			t.Fatalf("records = %q", got)
		}
		if s.tail() != "y" {
			t.Fatalf("tail = %q", s.tail())
		}
	})

	t.Run("NUL and multi-byte UTF-8 stay raw bytes", func(t *testing.T) {
		s := &delimiterSplitter{delim: []byte("\x00中")}
		s.write([]byte("a\x00中b中"))
		if got := s.records(); !reflect.DeepEqual(got, []string{"a"}) {
			t.Fatalf("records = %q", got)
		}
		if s.tail() != "b中" {
			t.Fatalf("tail = %q", s.tail())
		}
	})

	t.Run("reset discards unfinished record and delimiter prefix", func(t *testing.T) {
		s := &delimiterSplitter{delim: []byte("<END>")}
		s.write([]byte("old<EN"))
		s.reset()
		if s.buffered() != 0 {
			t.Fatalf("reset left %q behind", s.tail())
		}
		s.write([]byte("D>new<END>"))
		if got := s.records(); !reflect.DeepEqual(got, []string{"D>new"}) {
			t.Fatalf("records = %q", got)
		}
	})
}

// referenceSplit mirrors the required finite-file semantics using the
// standard library: bytes.Split consumes delimiters left-to-right without
// overlapping, and its final empty element (present exactly when the data
// ends on a delimiter) is dropped so no trailing empty record is produced.
func referenceSplit(data, delim []byte) []string {
	if len(data) == 0 {
		return nil
	}
	parts := bytes.Split(data, delim)
	if len(parts) > 0 && len(parts[len(parts)-1]) == 0 {
		parts = parts[:len(parts)-1]
	}
	out := make([]string, len(parts))
	for i, p := range parts {
		out[i] = string(p)
	}
	return out
}

// TestCollectorDelimiterBoundaryInvariance feeds every sample through the
// splitter using every possible chunk size (one byte up to the whole input),
// and also one prefix at a time. The accumulated records plus final tail
// must always equal the bytes.Split reference, so read and append boundaries
// can never influence the output.
func TestCollectorDelimiterBoundaryInvariance(t *testing.T) {
	samples := [][]byte{
		[]byte("xABABA"),
		[]byte("a<END><END>b<EN"),
		[]byte("|a||b|"),
		[]byte("|||"),
		[]byte("<END><END>"),
		[]byte("no delimiter at all"),
		[]byte("中\x00a\x00中\x00"),
		[]byte(strings.Repeat("q", 9000) + "<END>"),
	}
	delims := [][]byte{
		[]byte("ABA"),
		[]byte("<END>"),
		[]byte("|"),
		[]byte("\x00"),
		[]byte("中"),
	}
	for _, delim := range delims {
		for _, data := range samples {
			want := referenceSplit(data, delim)

			// Sweep every fixed chunk size.
			for chunk := 1; chunk <= len(data)+1; chunk++ {
				s := &delimiterSplitter{delim: delim}
				var got []string
				for start := 0; start < len(data); start += chunk {
					end := start + chunk
					if end > len(data) {
						end = len(data)
					}
					s.write(data[start:end])
					got = append(got, s.records()...)
				}
				if s.buffered() > 0 {
					got = append(got, s.tail())
				}
				if !reflect.DeepEqual(got, want) {
					t.Fatalf("delim=%q chunk=%d: got %q, want %q", delim, chunk, got, want)
				}
			}

			// Feed one prefix extension at a time; records emitted after
			// each append must already match the complete records of that
			// prefix (everything before the last delimiter), while its
			// unfinished tail stays buffered.
			s := &delimiterSplitter{delim: delim}
			var got []string
			for end := 1; end <= len(data); end++ {
				s.write(data[end-1 : end])
				got = append(got, s.records()...)
				prefixParts := bytes.Split(data[:end], delim)
				var complete []string
				for _, p := range prefixParts[:len(prefixParts)-1] {
					complete = append(complete, string(p))
				}
				if len(got) != len(complete) ||
					(len(complete) > 0 && !reflect.DeepEqual(got, complete)) {
					t.Fatalf("delim=%q prefix=%d: got %q, want %q", delim, end, got, complete)
				}
			}
			if s.buffered() > 0 {
				got = append(got, s.tail())
			}
			if !reflect.DeepEqual(got, want) {
				t.Fatalf("delim=%q prefix feed: got %q, want %q", delim, got, want)
			}
		}
	}
}

// TestCollectorDelimiterFinite covers Follow=false splitting semantics from
// the task description on both watcher backends.
func TestCollectorDelimiterFinite(t *testing.T) {
	delimiterBackends(t, func(t *testing.T, poll bool) {
		cases := []struct {
			name      string
			delimiter string
			content   string
			want      []string
		}{
			{"ABA non-overlap", "ABA", "xABABA", []string{"x", "BA"}},
			{"empty record and unfinished tail", "<END>", "a<END><END>b<EN", []string{"a", "", "b<EN"}},
			{"leading and consecutive delimiters", "|", "|a||b|", []string{"", "a", "", "b"}},
			{"empty file", "|", "", nil},
			{"only delimiters", "|", "|||", []string{"", "", ""}},
			{"NUL delimiter", "\x00", "a\x00\x00b", []string{"a", "", "b"}},
			{"multi-byte UTF-8 delimiter", "事件", "a事件事件b", []string{"a", "", "b"}},
		}
		for _, tc := range cases {
			t.Run(tc.name, func(t *testing.T) {
				p := filepath.Join(t.TempDir(), "events.log")
				if err := os.WriteFile(p, []byte(tc.content), 0600); err != nil {
					t.Fatal(err)
				}
				got := collectFinite(t, p, Config{
					Poll:          poll,
					CompleteLines: true,
					Delimiter:     tc.delimiter,
				})
				if !reflect.DeepEqual(got, tc.want) {
					t.Fatalf("got %q, want %q", got, tc.want)
				}
			})
		}
	})
}

// TestCollectorDelimiterBufferBoundary puts a record longer than the
// 4096-byte reader buffer and places the closing delimiter so that it
// straddles a reader fill boundary.
func TestCollectorDelimiterBufferBoundary(t *testing.T) {
	delimiterBackends(t, func(t *testing.T, poll bool) {
		longRecord := strings.Repeat("y", 8190) // delimiter starts at 8190, crosses 8192
		content := longRecord + "<END>tail<EN"
		p := filepath.Join(t.TempDir(), "events.log")
		if err := os.WriteFile(p, []byte(content), 0600); err != nil {
			t.Fatal(err)
		}
		got := collectFinite(t, p, Config{
			Poll:          poll,
			CompleteLines: true,
			Delimiter:     "<END>",
		})
		want := []string{longRecord, "tail<EN"}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("got %d records (%q…), want %d", len(got), got, len(want))
		}
	})
}

// TestCollectorDelimiterMaxLineSize verifies byte-wise chunking of the
// delimiter-free text, including that empty records add no extra chunks.
func TestCollectorDelimiterMaxLineSize(t *testing.T) {
	p := filepath.Join(t.TempDir(), "events.log")
	if err := os.WriteFile(p, []byte("abcd<END>ab<END><END>cd<END>"), 0600); err != nil {
		t.Fatal(err)
	}
	tl, err := TailFile(p, Config{
		MustExist:     true,
		Follow:        false,
		CompleteLines: true,
		Delimiter:     "<END>",
		MaxLineSize:   2,
		Logger:        DiscardingLogger,
	})
	if err != nil {
		t.Fatal(err)
	}
	defer tl.Cleanup()
	type numbered struct {
		text string
		num  int
	}
	var got []numbered
	for line := range tl.Lines {
		if line.Err != nil {
			t.Fatal(line.Err)
		}
		got = append(got, numbered{line.Text, line.Num})
	}
	if err := tl.Wait(); err != nil {
		t.Fatal(err)
	}
	want := []numbered{{"ab", 1}, {"cd", 2}, {"ab", 3}, {"", 4}, {"cd", 5}}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("got %+v, want %+v", got, want)
	}
}

// TestCollectorDelimiterValidation makes sure unsupported combinations are
// rejected before any goroutine is started.
func TestCollectorDelimiterValidation(t *testing.T) {
	p := filepath.Join(t.TempDir(), "events.log")
	if err := os.WriteFile(p, []byte("a<END>"), 0600); err != nil {
		t.Fatal(err)
	}
	tl, err := TailFile(p, Config{
		MustExist: true,
		Follow:    true,
		Delimiter: "<END>",
		Logger:    DiscardingLogger,
	})
	if err == nil {
		tl.Stop()
		tl.Cleanup()
		t.Fatal("Delimiter without CompleteLines must be rejected")
	}
	if tl != nil {
		t.Fatalf("TailFile returned a non-nil Tail together with error %v", err)
	}
}

// TestCollectorDelimiterLocationFirstOpen checks that Location is honored on
// the first open in delimiter mode.
func TestCollectorDelimiterLocationFirstOpen(t *testing.T) {
	p := filepath.Join(t.TempDir(), "events.log")
	content := "skip<END>a<END>"
	if err := os.WriteFile(p, []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	got := collectFinite(t, p, Config{
		CompleteLines: true,
		Delimiter:     "<END>",
		Location:      &SeekInfo{Offset: int64(len("skip<END>")), Whence: 0},
	})
	if !reflect.DeepEqual(got, []string{"a"}) {
		t.Fatalf("got %q, want [a]", got)
	}
}

// TestCollectorDelimiterFollowAppend verifies that incomplete records and
// half-matched delimiters are withheld at a temporary EOF and completed by
// later appends.
func TestCollectorDelimiterFollowAppend(t *testing.T) {
	delimiterBackends(t, func(t *testing.T, poll bool) {
		p := filepath.Join(t.TempDir(), "events.log")
		if err := os.WriteFile(p, []byte("one<EN"), 0600); err != nil {
			t.Fatal(err)
		}
		tl, err := TailFile(p, Config{
			MustExist:     true,
			Follow:        true,
			ReOpen:        true,
			Poll:          poll,
			CompleteLines: true,
			Delimiter:     "<END>",
			Logger:        DiscardingLogger,
		})
		if err != nil {
			t.Fatal(err)
		}
		defer tl.Cleanup()
		defer tl.Stop()

		// No complete record yet: "one<EN" ends in a half delimiter.
		assertNoRecord(t, tl, 150*time.Millisecond)

		append := func(data string) {
			t.Helper()
			f, err := os.OpenFile(p, os.O_APPEND|os.O_WRONLY, 0600)
			if err != nil {
				t.Fatal(err)
			}
			if _, err := f.WriteString(data); err != nil {
				f.Close()
				t.Fatal(err)
			}
			f.Close()
		}

		append("D>two<END>")
		receiveRecord(t, tl, "one")
		receiveRecord(t, tl, "two")

		append("three<END>")
		receiveRecord(t, tl, "three")

		append("four<EN")
		assertNoRecord(t, tl, 150*time.Millisecond)
		append("D>")
		receiveRecord(t, tl, "four")
	})
}

// TestCollectorDelimiterReopenRename replaces the file via rename while an
// unfinished record is pending. Nothing from the old file may be joined
// with the new one, and the new file's head must be read from byte zero.
func TestCollectorDelimiterReopenRename(t *testing.T) {
	delimiterBackends(t, func(t *testing.T, poll bool) {
		dir := t.TempDir()
		p := filepath.Join(dir, "events.log")
		if err := os.WriteFile(p, []byte("keep<END>old<EN"), 0600); err != nil {
			t.Fatal(err)
		}
		tl, err := TailFile(p, Config{
			MustExist:     true,
			Follow:        true,
			ReOpen:        true,
			Poll:          poll,
			CompleteLines: true,
			Delimiter:     "<END>",
			Logger:        DiscardingLogger,
		})
		if err != nil {
			t.Fatal(err)
		}
		defer tl.Cleanup()
		defer tl.Stop()

		receiveRecord(t, tl, "keep")
		assertNoRecord(t, tl, 150*time.Millisecond)

		rotated := p + ".rotated"
		if err := os.Rename(p, rotated); err != nil {
			t.Fatal(err)
		}
		// Give the watcher time to observe the rename before the new file
		// appears at the old path.
		time.Sleep(150 * time.Millisecond)
		if err := os.WriteFile(p, []byte("D>new<END>"), 0600); err != nil {
			t.Fatal(err)
		}
		defer os.Remove(rotated)

		receiveRecord(t, tl, "D>new")
		assertNoRecord(t, tl, 150*time.Millisecond)
	})
}

// TestCollectorDelimiterTruncate verifies that truncation drops the old
// file's unfinished record and partial delimiter and starts the new content
// from its first byte.
func TestCollectorDelimiterTruncate(t *testing.T) {
	delimiterBackends(t, func(t *testing.T, poll bool) {
		p := filepath.Join(t.TempDir(), "events.log")
		if err := os.WriteFile(p, []byte("first<END>pend<EN"), 0600); err != nil {
			t.Fatal(err)
		}
		tl, err := TailFile(p, Config{
			MustExist:     true,
			Follow:        true,
			ReOpen:        true,
			Poll:          poll,
			CompleteLines: true,
			Delimiter:     "<END>",
			Logger:        DiscardingLogger,
		})
		if err != nil {
			t.Fatal(err)
		}
		defer tl.Cleanup()
		defer tl.Stop()

		receiveRecord(t, tl, "first")
		assertNoRecord(t, tl, 150*time.Millisecond)

		f, err := os.OpenFile(p, os.O_TRUNC|os.O_WRONLY, 0600)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := f.WriteString("fresh<END>"); err != nil {
			f.Close()
			t.Fatal(err)
		}
		f.Close()

		receiveRecord(t, tl, "fresh")
		assertNoRecord(t, tl, 150*time.Millisecond)
	})
}
