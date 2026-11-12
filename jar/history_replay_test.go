package jar

import (
	"errors"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// oneShotReader is a forward-only reader with no Seek or WriteTo, so the
// storage cannot rely on rewinding it.
type oneShotReader struct{ r io.Reader }

func (o oneShotReader) Read(p []byte) (int, error) { return o.r.Read(p) }

type brokenReader struct{}

func (brokenReader) Read([]byte) (int, error) { return 0, errors.New("fixture-source-read") }

func filesIn(t *testing.T, dir string) []string {
	t.Helper()
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatalf("reading replay dir: %v", err)
	}
	names := make([]string, 0, len(entries))
	for _, e := range entries {
		names = append(names, e.Name())
	}
	return names
}

func stateWithBody(t *testing.T, store *ReplayStorage, payload string) (*State, string) {
	t.Helper()
	rb, err := store.Prepare(oneShotReader{strings.NewReader(payload)})
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}
	return &State{Replay: rb}, rb.file.Name()
}

// TestHistoryReplayStorage covers replayable request bodies on both storage
// paths, their reference counting, and the MemoryHistory eviction and
// sharing lifecycle.
func TestHistoryReplayStorage(t *testing.T) {
	cases := []struct {
		name       string
		memoryLim  int64
		wantOnDisk bool
	}{
		{"memory", 1 << 20, false},
		{"spill", 16, true},
		{"disk", 0, true},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			dir := t.TempDir()
			store := NewReplayStorage(tc.memoryLim, dir)
			payload := strings.Repeat("abcdefgh", 512) // 4096 bytes

			rb, err := store.Prepare(oneShotReader{strings.NewReader(payload)})
			if err != nil {
				t.Fatalf("Prepare: %v", err)
			}
			if rb.Size() != int64(len(payload)) {
				t.Fatalf("size = %d, want %d", rb.Size(), len(payload))
			}
			if rb.OnDisk() != tc.wantOnDisk {
				t.Fatalf("OnDisk = %v, want %v", rb.OnDisk(), tc.wantOnDisk)
			}
			if tc.wantOnDisk {
				if names := filesIn(t, dir); len(names) != 1 {
					t.Fatalf("want exactly one spill file, got %v", names)
				}
			} else if names := filesIn(t, dir); len(names) != 0 {
				t.Fatalf("memory path created files: %v", names)
			}

			// Every replay gets an independent reader and reads the full body.
			r1, err := rb.NewReader()
			if err != nil {
				t.Fatalf("NewReader: %v", err)
			}
			r2, err := rb.NewReader()
			if err != nil {
				t.Fatalf("NewReader: %v", err)
			}
			got1, err := io.ReadAll(r1)
			if err != nil {
				t.Fatalf("read 1: %v", err)
			}
			got2, err := io.ReadAll(r2)
			if err != nil {
				t.Fatalf("read 2: %v", err)
			}
			if string(got1) != payload || string(got2) != payload {
				t.Fatal("replay readers did not return the stored body")
			}

			// A shared owner keeps the body alive after the first release.
			rb.Retain()
			rb.Release()
			if tc.wantOnDisk && len(filesIn(t, dir)) != 1 {
				t.Fatal("shared release removed the file while an owner remained")
			}
			rb.Release()
			if names := filesIn(t, dir); len(names) != 0 {
				t.Fatalf("replay resources not released: %v", names)
			}
		})
	}

	t.Run("source error leaves no files", func(t *testing.T) {
		dir := t.TempDir()
		// Limit zero forces an immediate spill, exercising the file write
		// cleanup path as well.
		store := NewReplayStorage(0, dir)
		if _, err := store.Prepare(brokenReader{}); err == nil {
			t.Fatal("want error from broken reader")
		} else if !strings.Contains(err.Error(), "fixture-source-read") {
			t.Fatalf("error does not wrap the original cause: %v", err)
		}
		if names := filesIn(t, dir); len(names) != 0 {
			t.Fatalf("broken preparation left files: %v", names)
		}
	})

	t.Run("history eviction and clear release resources", func(t *testing.T) {
		dir := t.TempDir()
		store := NewReplayStorage(0, dir)
		hist := NewMemoryHistory()
		hist.SetMax(2)

		s1, file1 := stateWithBody(t, store, "page-1")
		s2, file2 := stateWithBody(t, store, "page-2")
		s3, file3 := stateWithBody(t, store, "page-3")

		hist.Push(s1)
		hist.Push(s2)
		if hist.Len() != 2 {
			t.Fatalf("len = %d, want 2", hist.Len())
		}
		hist.Push(s3) // evicts s1
		if hist.Len() != 2 {
			t.Fatalf("len = %d, want 2", hist.Len())
		}
		if _, err := os.Stat(file1); !os.IsNotExist(err) {
			t.Fatalf("evicted replay file still present: %v", err)
		}
		if _, err := os.Stat(file2); err != nil {
			t.Fatalf("retained file missing: %v", err)
		}
		if _, err := os.Stat(file3); err != nil {
			t.Fatalf("retained file missing: %v", err)
		}

		// Shrinking the limit again evicts the oldest surviving state.
		hist.SetMax(1)
		if _, err := os.Stat(file2); !os.IsNotExist(err) {
			t.Fatalf("SetMax shrink did not release the old file: %v", err)
		}

		// Pop transfers ownership to the caller: Clear must not touch it
		// twice and the popped state stays valid until explicitly released.
		popped := hist.Pop()
		if popped != s3 {
			t.Fatal("Pop did not return the most recent state")
		}
		hist.Clear()
		if hist.Len() != 0 {
			t.Fatalf("len after Clear = %d, want 0", hist.Len())
		}
		if _, err := os.Stat(file3); err != nil {
			t.Fatalf("popped state resource was released early: %v", err)
		}
		popped.ReleaseResources()
		if names := filesIn(t, dir); len(names) != 0 {
			t.Fatalf("resources left after release: %v", names)
		}
	})

	t.Run("shared reference survives eviction", func(t *testing.T) {
		dir := t.TempDir()
		store := NewReplayStorage(0, dir)
		hist := NewMemoryHistory()
		hist.SetMax(1)

		s1, file1 := stateWithBody(t, store, "shared")
		s1.Replay.Retain() // another tab still uses this page
		hist.Push(s1)

		s2, _ := stateWithBody(t, store, "next")
		hist.Push(s2) // evicts s1, but the extra owner keeps the file
		if _, err := os.Stat(file1); err != nil {
			t.Fatalf("shared replay file released too early: %v", err)
		}
		s1.ReleaseResources() // the other tab closes
		if _, err := os.Stat(file1); !os.IsNotExist(err) {
			t.Fatal("shared replay file survived the last owner")
		}

		hist.Clear()
		if names := filesIn(t, dir); len(names) != 0 {
			t.Fatalf("leftover files: %v", names)
		}
	})

	t.Run("tab ownership of shared history", func(t *testing.T) {
		dir := t.TempDir()
		store := NewReplayStorage(0, dir)
		hist := NewMemoryHistory() // starts with one owning tab

		s1, file1 := stateWithBody(t, store, "a")
		hist.Push(s1)
		hist.Attach() // a second tab opens

		hist.Detach() // first tab closes, second still alive
		if _, err := os.Stat(file1); err != nil {
			t.Fatalf("file gone while a tab still owns the history: %v", err)
		}
		if hist.Detach() {
			hist.Clear()
		}
		if _, err := os.Stat(file1); !os.IsNotExist(err) {
			t.Fatal("file survived the last owning tab")
		}
		if names, _ := filepath.Glob(filepath.Join(dir, "*")); len(names) != 0 {
			t.Fatalf("leftover files: %v", names)
		}
	})
}
