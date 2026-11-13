package pogreb

import (
	"fmt"
	"math"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"

	"github.com/akrylysov/pogreb/internal/errors"
)

const (
	maxSegments = math.MaxInt16
)

// datalog is a write-ahead log.
type datalog struct {
	opts          *Options
	curSeg        *segment
	segments      [maxSegments]*segment
	maxSequenceID uint64

	// mu serializes segment lifecycle changes: segment swaps, compaction
	// retirement and backup segment retention. It is never held while
	// segment data is being read or copied, so a slow backup doesn't block
	// writes or compaction.
	mu sync.Mutex
}

func openDatalog(opts *Options) (*datalog, error) {
	files, err := opts.FileSystem.ReadDir(".")
	if err != nil {
		return nil, err
	}

	dl := &datalog{
		opts: opts,
	}

	// Open existing segments.
	for _, file := range files {
		name := file.Name()
		ext := filepath.Ext(name)
		if ext != segmentExt {
			continue
		}
		id, seqID, err := parseSegmentName(name)
		if err != nil {
			return nil, err
		}
		seg, err := dl.openSegment(name, id, seqID)
		if err != nil {
			return nil, errors.Wrapf(err, "opening segment %s", name)
		}
		if seg.sequenceID > dl.maxSequenceID {
			dl.maxSequenceID = seg.sequenceID
		}
		dl.segments[seg.id] = seg
	}

	if err := dl.swapSegment(); err != nil {
		return nil, err
	}

	return dl, nil
}

func parseSegmentName(name string) (uint16, uint64, error) {
	parts := strings.SplitN(strings.TrimSuffix(name, segmentExt), "-", 2)
	id, err := strconv.ParseUint(parts[0], 10, 16)
	if err != nil {
		return 0, 0, err
	}
	var seqID uint64
	if len(parts) == 2 {
		seqID, err = strconv.ParseUint(parts[1], 10, 64)
		if err != nil {
			return 0, 0, err
		}
	}
	return uint16(id), seqID, nil
}

func (dl *datalog) openSegment(name string, id uint16, seqID uint64) (*segment, error) {
	f, err := openFile(dl.opts.FileSystem, name, openFileFlags{})
	if err != nil {
		return nil, err
	}

	meta := &segmentMeta{}
	if !f.empty() {
		metaName := name + metaExt
		if err := readGobFile(dl.opts.FileSystem, metaName, &meta); err != nil {
			logger.Printf("error reading segment meta %d: %v", id, err)
			// TODO: rebuild meta?
		}
	}

	seg := &segment{
		file:       f,
		id:         id,
		sequenceID: seqID,
		name:       name,
		meta:       meta,
	}

	return seg, nil
}

func (dl *datalog) nextWritableSegmentID() (uint16, uint64, error) {
	for id, seg := range dl.segments {
		// Pick empty segment.
		if seg == nil {
			dl.maxSequenceID++
			return uint16(id), dl.maxSequenceID, nil
		}
	}
	return 0, 0, fmt.Errorf("number of segments exceeds %d", maxSegments)
}

func (dl *datalog) swapSegment() error {
	// Pick unfilled segment.
	for _, seg := range dl.segments {
		if seg != nil && !seg.meta.Full {
			dl.curSeg = seg
			return nil
		}
	}

	// Create new segment.
	id, seqID, err := dl.nextWritableSegmentID()
	if err != nil {
		return err
	}

	name := segmentName(id, seqID)
	seg, err := dl.openSegment(name, id, seqID)
	if err != nil {
		return err
	}

	dl.segments[id] = seg
	dl.curSeg = seg

	return nil
}

// removeSegment detaches a compacted segment from the datalog.
// If in-progress backups still need the segment, its files and handle are
// retained until the last backup releases it; otherwise it is purged now.
func (dl *datalog) removeSegment(seg *segment) error {
	dl.mu.Lock()
	defer dl.mu.Unlock()
	return dl.retire(seg)
}

// capturedSegment is an immutable snapshot of a single segment taken at the
// backup boundary. The segment identity (physical id plus sequence id) is
// encoded in name and in the retained segment, while size limits the copy
// for active segments: only the bytes that existed when the backup started
// are copied, so records appended afterwards are not part of the backup.
type capturedSegment struct {
	seg  *segment
	name string
	size int64
}

// captureSegments takes an immutable backup boundary: the segments existing
// at this moment, each with its exact byte length, plus a retention
// reference keeping every segment (including active ones) alive and at a
// stable size for the duration of the backup.
func (dl *datalog) captureSegments() []*capturedSegment {
	dl.mu.Lock()
	defer dl.mu.Unlock()

	segs := dl.segmentsBySequenceID()
	captured := make([]*capturedSegment, 0, len(segs))
	for _, seg := range segs {
		seg.retain()
		captured = append(captured, &capturedSegment{
			seg:  seg,
			name: seg.name,
			size: seg.size,
		})
	}
	return captured
}

// releaseSegments drops the retention references acquired by captureSegments.
// Segments already retired by compaction are purged when the last reference
// goes away. The first cleanup error is returned; every reference is
// released regardless of errors.
func (dl *datalog) releaseSegments(captured []*capturedSegment) error {
	dl.mu.Lock()
	defer dl.mu.Unlock()

	var firstErr error
	for _, c := range captured {
		if err := dl.release(c.seg); err != nil && firstErr == nil {
			firstErr = err
		}
	}
	return firstErr
}

func (dl *datalog) readKeyValue(sl slot) ([]byte, []byte, error) {
	off := int64(sl.offset) + 6 // Skip key size and value size.
	seg := dl.segments[sl.segmentID]
	keyValue, err := seg.Slice(off, off+int64(sl.kvSize()))
	if err != nil {
		return nil, nil, err
	}
	return keyValue[:sl.keySize], keyValue[sl.keySize:], nil
}

func (dl *datalog) readKey(sl slot) ([]byte, error) {
	off := int64(sl.offset) + 6
	seg := dl.segments[sl.segmentID]
	return seg.Slice(off, off+int64(sl.keySize))
}

// trackDel updates segment's metadata for deleted or overwritten items.
func (dl *datalog) trackDel(sl slot) {
	meta := dl.segments[sl.segmentID].meta
	meta.DeletedKeys++
	meta.DeletedBytes += encodedRecordSize(sl.kvSize())
}

func (dl *datalog) del(key []byte) error {
	rec := encodeDeleteRecord(key)
	_, _, err := dl.writeRecord(rec, recordTypeDelete)
	if err != nil {
		return err
	}
	// Compaction removes delete records, increment DeletedBytes.
	dl.curSeg.meta.DeletedBytes += uint32(len(rec))
	return nil
}

func (dl *datalog) writeRecord(data []byte, rt recordType) (uint16, uint32, error) {
	if dl.curSeg.meta.Full || dl.curSeg.size+int64(len(data)) > int64(dl.opts.maxSegmentSize) {
		// Current segment is full, create a new one.
		dl.curSeg.meta.Full = true
		if err := dl.swapSegment(); err != nil {
			return 0, 0, err
		}
	}
	off, err := dl.curSeg.append(data)
	if err != nil {
		return 0, 0, err
	}
	switch rt {
	case recordTypePut:
		dl.curSeg.meta.PutRecords++
	case recordTypeDelete:
		dl.curSeg.meta.DeleteRecords++
	}
	return dl.curSeg.id, uint32(off), nil
}

func (dl *datalog) put(key []byte, value []byte) (uint16, uint32, error) {
	return dl.writeRecord(encodePutRecord(key, value), recordTypePut)
}

func (dl *datalog) sync() error {
	return dl.curSeg.Sync()
}

func (dl *datalog) close() error {
	for _, seg := range dl.segments {
		if seg == nil {
			continue
		}
		if err := seg.Close(); err != nil {
			return err
		}
		metaName := seg.name + metaExt
		if err := writeGobFile(dl.opts.FileSystem, metaName, seg.meta); err != nil {
			return err
		}
	}
	return nil
}

// segmentsBySequenceID returns segments ordered from oldest to newest.
func (dl *datalog) segmentsBySequenceID() []*segment {
	var segments []*segment

	for _, seg := range dl.segments {
		if seg == nil {
			continue
		}
		segments = append(segments, seg)
	}

	sort.SliceStable(segments, func(i, j int) bool {
		return segments[i].sequenceID < segments[j].sequenceID
	})

	return segments
}
