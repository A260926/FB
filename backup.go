package pogreb

import (
	"io"
	"os"

	"github.com/akrylysov/pogreb/fs"
)

// backupCopyBufferSize is the fixed size of the buffer used to stream
// segment data to the backup destination. Backups never hold whole
// segments or records in memory.
const backupCopyBufferSize = 32 * 1024

func touchFile(fsys fs.FileSystem, path string) error {
	f, err := fsys.OpenFile(path, os.O_CREATE|os.O_TRUNC, os.FileMode(0640))
	if err != nil {
		return err
	}
	return f.Close()
}

// Backup creates a database backup at the specified path.
//
// The backup represents a consistent database state captured at the moment
// Backup starts. A short critical section records an immutable manifest of
// the segments existing at that moment - each identified by its physical
// segment id and sequence id, together with its exact byte length - and
// retains every segment on the list. Segment files are then opened, copied
// and closed one at a time without holding db.mu or maintenanceMu, so a
// slow destination does not block Put, Delete or Compact.
//
// Compaction may retire and delete captured segments while a backup is in
// progress, and their segment ids may be reused by new segments. Retained
// files stay available until the backup finishes; the sequence id in the
// file name distinguishes a retained segment from a new segment reusing the
// same physical id, so the backup never reads the wrong generation. When
// the last backup reference to a retired segment is released, the segment
// and its metadata are closed and removed.
//
// The on-disk format is unchanged: the destination contains recoverable
// data segments followed by the lock file marker, and is rebuilt from the
// segments on open. A failed backup may leave an incomplete destination
// directory.
func (db *DB) Backup(path string) (retErr error) {
	if err := db.opts.rootFS.MkdirAll(path, 0755); err != nil {
		return err
	}

	// Short critical section: fix the backup boundary and retain the
	// captured segments. The retention references are released on return.
	db.mu.RLock()
	captured := db.datalog.captureSegments()
	db.mu.RUnlock()

	// On the success path, errors from the final retention cleanup (for
	// example while purging a segment retired by compaction) must not be
	// swallowed; they are returned. After a primary error the first error
	// keeps its errors.Is relationship, so cleanup errors are only logged.
	defer func() {
		if err := db.datalog.releaseSegments(captured); err != nil {
			if retErr == nil {
				retErr = err
			} else {
				logger.Printf("error releasing backup segments: %v", err)
			}
		}
	}()

	srcFS := db.opts.FileSystem
	dstFS := fs.Sub(db.opts.rootFS, path)
	mode := os.FileMode(0640)

	// A single fixed-size buffer is reused for every segment; the backup
	// never buffers a whole segment or record.
	copyBuf := make([]byte, backupCopyBufferSize)
	for _, c := range captured {
		if err := backupSegment(srcFS, dstFS, c, mode, copyBuf); err != nil {
			return err
		}
	}

	if err := touchFile(dstFS, lockName); err != nil {
		return err
	}

	return nil
}

// backupSegment copies a single captured segment to the backup destination.
// It holds at most one source handle and one destination handle at a time,
// reads the source exclusively through ReadAt, and copies only the number
// of bytes the segment had at the backup boundary.
func backupSegment(srcFS, dstFS fs.FileSystem, c *capturedSegment, mode os.FileMode, buf []byte) error {
	srcFile, err := srcFS.OpenFile(c.name, os.O_RDONLY, mode)
	if err != nil {
		return err
	}

	dstFile, err := dstFS.OpenFile(c.name, os.O_CREATE|os.O_RDWR|os.O_TRUNC, mode)
	if err != nil {
		_ = srcFile.Close()
		return err
	}

	var firstErr error
	var copied int64
	for copied < c.size && firstErr == nil {
		chunk := buf
		if remaining := c.size - copied; int64(len(chunk)) > remaining {
			chunk = chunk[:remaining]
		}
		n, readErr := srcFile.ReadAt(chunk, copied)
		if n > 0 {
			wn, writeErr := dstFile.WriteAt(chunk[:n], copied)
			copied += int64(wn)
			if writeErr != nil {
				firstErr = writeErr
			} else if wn != n {
				firstErr = io.ErrShortWrite
			}
		}
		if readErr != nil && !(readErr == io.EOF && copied == c.size) {
			firstErr = readErr
		}
	}

	if err := dstFile.Close(); err != nil && firstErr == nil {
		firstErr = err
	}
	if err := srcFile.Close(); err != nil && firstErr == nil {
		firstErr = err
	}
	return firstErr
}
