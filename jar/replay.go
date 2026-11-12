package jar

import (
	"bytes"
	"errors"
	"io"
	"os"
	"strings"
)

// DefaultReplayMemoryLimit is the default value for Browser.SetReplayStorage.
// Encoded request bodies up to this size are buffered in memory; larger
// bodies spill into temporary files.
const DefaultReplayMemoryLimit int64 = 1 << 20 // 1 MiB

// ReplayBody stores a fully prepared, encoded HTTP request body so the
// originating request can be replayed (for example by Browser.Reload) even
// when the caller supplied a one-shot io.Reader.
//
// Small bodies stay in memory. Bodies larger than the configured memory
// limit are written to a temporary file; the file is removed once the last
// owner releases the body.
//
// A ReplayBody value may be shared by several page states, for example when
// NewTab hands the current page and its history to another tab. Every owner
// must call Release once it drops its reference. The underlying buffer or
// temporary file is released when the last owner does so.
type ReplayBody struct {
	mem  []byte
	file *os.File
	refs int
}

// ReplayStorage builds ReplayBody values. Bodies up to memoryLimit bytes are
// kept in memory, larger bodies are written to a temporary file created in
// dir. A memoryLimit of 0 stores every body on disk. An empty dir makes the
// storage use the operating system's temporary directory.
type ReplayStorage struct {
	memoryLimit int64
	dir         string
}

// NewReplayStorage creates and returns a *ReplayStorage type.
func NewReplayStorage(memoryLimit int64, dir string) *ReplayStorage {
	return &ReplayStorage{
		memoryLimit: memoryLimit,
		dir:         dir,
	}
}

// MemoryLimit returns the configured in-memory threshold.
func (s *ReplayStorage) MemoryLimit() int64 {
	return s.memoryLimit
}

// Dir returns the directory used for spilled bodies. An empty string means
// the operating system's temporary directory is used.
func (s *ReplayStorage) Dir() string {
	return s.dir
}

// Prepare fully reads src into a replayable body.
//
// In-memory readers whose size fits within the memory limit are taken
// directly. Every other reader is streamed, so buffering a large multipart
// upload does not grow memory with the attachment size: bytes stay in memory
// only while they are at or below the memory limit, and the moment the limit
// is exceeded the buffered bytes are migrated to a temporary file and the
// rest of src is streamed straight to disk.
//
// If reading src, creating the temporary file or writing to it fails, every
// resource already allocated is released and the returned error wraps the
// original cause. No request should be sent when Prepare fails.
func (s *ReplayStorage) Prepare(src io.Reader) (*ReplayBody, error) {
	if src == nil {
		return nil, nil
	}

	// Fast path: readers with a known size that fits inside the memory limit
	// are taken without going through the streaming buffer.
	if s.memoryLimit > 0 {
		switch v := src.(type) {
		case *bytes.Buffer:
			// Copy: a bytes.Buffer is mutable and the caller may keep
			// writing to it after Post returns.
			if int64(v.Len()) <= s.memoryLimit {
				return &ReplayBody{mem: append([]byte(nil), v.Bytes()...), refs: 1}, nil
			}
		case *bytes.Reader:
			if int64(v.Len()) <= s.memoryLimit {
				b, err := io.ReadAll(v)
				if err != nil {
					return nil, err
				}
				return &ReplayBody{mem: b, refs: 1}, nil
			}
		case *strings.Reader:
			if int64(v.Len()) <= s.memoryLimit {
				b, err := io.ReadAll(v)
				if err != nil {
					return nil, err
				}
				return &ReplayBody{mem: b, refs: 1}, nil
			}
		}
	}

	rb := &ReplayBody{refs: 1}
	memLimit := s.memoryLimit
	if memLimit < 0 {
		memLimit = 0
	}

	buf := make([]byte, 0, replayBufferCap(memLimit))
	chunk := make([]byte, 32*1024)
	spilled := false
	for {
		n, readErr := src.Read(chunk)
		if n > 0 {
			if !spilled {
				if int64(len(buf)+n) <= memLimit {
					buf = append(buf, chunk[:n]...)
				} else {
					f, err := os.CreateTemp(s.dir, "surf-replay-*.body")
					if err != nil {
						return nil, err
					}
					if _, err := f.Write(buf); err != nil {
						f.Close()
						os.Remove(f.Name())
						return nil, err
					}
					if _, err := f.Write(chunk[:n]); err != nil {
						f.Close()
						os.Remove(f.Name())
						return nil, err
					}
					rb.file = f
					spilled = true
					buf = nil
				}
			} else if _, err := rb.file.Write(chunk[:n]); err != nil {
				rb.close()
				return nil, err
			}
		}
		if readErr != nil {
			if errors.Is(readErr, io.EOF) {
				break
			}
			if spilled {
				rb.close()
			}
			return nil, readErr
		}
	}

	if !spilled {
		rb.mem = buf
	}
	return rb, nil
}

// Retain marks one additional owner. It is used when a page state carrying
// this body is shared, for example through Browser.NewTab.
func (rb *ReplayBody) Retain() {
	if rb == nil {
		return
	}
	rb.refs++
}

// Release drops one owner reference. The buffered bytes or the backing
// temporary file are released when the last reference goes away.
func (rb *ReplayBody) Release() {
	if rb == nil {
		return
	}
	rb.refs--
	if rb.refs <= 0 {
		rb.close()
	}
}

// Size returns the length of the stored body in bytes.
func (rb *ReplayBody) Size() int64 {
	if rb == nil {
		return 0
	}
	if rb.file != nil {
		info, err := rb.file.Stat()
		if err != nil {
			return 0
		}
		return info.Size()
	}
	return int64(len(rb.mem))
}

// OnDisk reports whether the body is held in a temporary file.
func (rb *ReplayBody) OnDisk() bool {
	return rb != nil && rb.file != nil
}

// NewReader returns a fresh, independent reader over the stored body. It can
// be called any number of times and the readers do not interfere with each
// other.
func (rb *ReplayBody) NewReader() (io.ReadCloser, error) {
	if rb == nil {
		return io.NopCloser(bytes.NewReader(nil)), nil
	}
	if rb.file != nil {
		f, err := os.Open(rb.file.Name())
		if err != nil {
			return nil, err
		}
		return f, nil
	}
	return io.NopCloser(bytes.NewReader(rb.mem)), nil
}

// close releases the underlying resources regardless of the reference count.
func (rb *ReplayBody) close() {
	if rb.file != nil {
		name := rb.file.Name()
		rb.file.Close()
		os.Remove(name)
		rb.file = nil
	}
	rb.mem = nil
	rb.refs = 0
}

func replayBufferCap(memLimit int64) int {
	const maxCap = 32 * 1024
	if memLimit <= 0 {
		return 0
	}
	if memLimit < maxCap {
		return int(memLimit)
	}
	return maxCap
}
