package pogreb

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/akrylysov/pogreb/fs"
)

// errInjected is the sentinel error returned by the fault-injecting file
// system. Tests assert errors.Is on it to make sure Backup keeps the
// identity of the first primary error.
var errInjected = errors.New("injected backup I/O failure")

type fsEvent struct {
	op   string // "open", "close" or "remove"
	name string
}

// barrierFS is a file system wrapper that drives the concurrent backup
// tests with channel barriers and deterministic fault injection, instead
// of sleeps or timing comparisons.
//
// It additionally records the real lifecycle of segment files: extra
// source/destination OpenFile calls, current and high-water handle counts,
// and every Close/Remove with its order.
type barrierFS struct {
	fs.FileSystem
	dstDirs map[string]bool

	mu sync.Mutex

	events []fsEvent

	srcLive    int
	dstLive    int
	maxSrcLive int
	maxDstLive int
	srcOpens   int64
	dstOpens   int64

	// pausePrefix makes the first WriteAt into a destination file under
	// the prefix block on writeEntered until writeRelease is closed.
	pausePrefix  string
	writeEntered chan struct{}
	writeRelease chan struct{}
	writeOnce    sync.Once

	// failPrefix scopes failDstWrite to a single backup destination.
	failPrefix string

	failDstOpen  int32
	failSrcRead  int32
	failDstWrite int32
	failDstClose int32
	failMarker   int32
}

func (b *barrierFS) record(op, name string) {
	b.events = append(b.events, fsEvent{op: op, name: name})
}

func (b *barrierFS) eventCount() int {
	b.mu.Lock()
	defer b.mu.Unlock()
	return len(b.events)
}

// eventIndex returns the first event matching op and name. Events carry
// sub-file-system paths, so name may be given as a base name and matches
// either an exact path or a "/name" suffix.
func (b *barrierFS) eventIndex(op, name string) int {
	b.mu.Lock()
	defer b.mu.Unlock()
	suffix := string(os.PathSeparator) + name
	for i, e := range b.events {
		if e.op != op {
			continue
		}
		if e.name == name || strings.HasSuffix(e.name, suffix) {
			return i
		}
	}
	return -1
}

func (b *barrierFS) isDst(name string) bool {
	for dir := range b.dstDirs {
		if strings.HasPrefix(name, dir+string(os.PathSeparator)) {
			return true
		}
	}
	return false
}

func (b *barrierFS) OpenFile(name string, flag int, perm os.FileMode) (fs.File, error) {
	dst := b.isDst(name)
	if dst && strings.HasSuffix(name, segmentExt) && flag&os.O_CREATE != 0 &&
		atomic.LoadInt32(&b.failDstOpen) == 1 {
		return nil, errInjected
	}
	if dst && filepath.Base(name) == lockName && atomic.LoadInt32(&b.failMarker) == 1 {
		return nil, errInjected
	}

	f, err := b.FileSystem.OpenFile(name, flag, perm)
	if err != nil {
		return nil, err
	}

	src := strings.HasSuffix(name, segmentExt) && flag == os.O_RDONLY
	dstSeg := dst && strings.HasSuffix(name, segmentExt) && flag&os.O_CREATE != 0
	if !src && !dstSeg {
		return f, nil
	}

	bf := &barrierFile{File: f, b: b, name: name, src: src, dst: dstSeg}
	b.mu.Lock()
	b.record("open", name)
	if src {
		b.srcLive++
		b.srcOpens++
		if b.srcLive > b.maxSrcLive {
			b.maxSrcLive = b.srcLive
		}
	} else {
		b.dstLive++
		b.dstOpens++
		if b.dstLive > b.maxDstLive {
			b.maxDstLive = b.dstLive
		}
	}
	b.mu.Unlock()
	return bf, nil
}

func (b *barrierFS) Remove(name string) error {
	err := b.FileSystem.Remove(name)
	b.mu.Lock()
	b.record("remove", name)
	b.mu.Unlock()
	return err
}

type barrierFile struct {
	fs.File
	b    *barrierFS
	name string
	src  bool
	dst  bool
}

func (f *barrierFile) ReadAt(p []byte, off int64) (int, error) {
	if f.src && atomic.LoadInt32(&f.b.failSrcRead) == 1 {
		return 0, errInjected
	}
	return f.File.ReadAt(p, off)
}

func (f *barrierFile) WriteAt(p []byte, off int64) (int, error) {
	if f.dst {
		if f.b.pausePrefix != "" && strings.HasPrefix(f.name, f.b.pausePrefix) {
			f.b.writeOnce.Do(func() {
				close(f.b.writeEntered)
				<-f.b.writeRelease
			})
		}
		if atomic.LoadInt32(&f.b.failDstWrite) == 1 &&
			(f.b.failPrefix == "" || strings.HasPrefix(f.name, f.b.failPrefix)) {
			return 0, errInjected
		}
	}
	return f.File.WriteAt(p, off)
}

func (f *barrierFile) Close() error {
	err := f.File.Close()
	f.b.mu.Lock()
	f.b.record("close", f.name)
	if f.src {
		f.b.srcLive--
	} else {
		f.b.dstLive--
	}
	f.b.mu.Unlock()
	if f.dst && atomic.LoadInt32(&f.b.failDstClose) == 1 {
		return errInjected
	}
	return err
}

func waitChannel(t *testing.T, ch chan struct{}, msg string) {
	t.Helper()
	select {
	case <-ch:
	case <-time.After(5 * time.Second):
		t.Fatal(msg)
	}
}

func forEachRealFS(t *testing.T, fn func(t *testing.T, srcFS fs.FileSystem)) {
	t.Helper()
	for _, srcFS := range []fs.FileSystem{fs.OS, fs.OSMMap} {
		name := "OS"
		if srcFS == fs.OSMMap {
			name = "OSMMap"
		}
		t.Run(name, func(t *testing.T) {
			fn(t, srcFS)
		})
	}
}

func newBarrierDB(t *testing.T, b *barrierFS, live string) *DB {
	t.Helper()
	opts := &Options{
		FileSystem:                 b,
		maxSegmentSize:             1024,
		compactionMinSegmentSize:   520,
		compactionMinFragmentation: 0.1,
	}
	db, err := Open(live, opts)
	if err != nil {
		t.Fatal(err)
	}
	return db
}

func snapshotItems(t *testing.T, db *DB) map[string]string {
	t.Helper()
	got := map[string]string{}
	it := db.Items()
	for {
		k, v, err := it.Next()
		if err == ErrIterationDone {
			break
		}
		if err != nil {
			t.Fatal(err)
		}
		got[string(k)] = string(v)
	}
	return got
}

func openBackupAndDump(t *testing.T, dir string) map[string]string {
	t.Helper()
	db, err := Open(dir, &Options{FileSystem: fs.OS})
	if err != nil {
		t.Fatal(err)
	}
	defer func() {
		if err := db.Close(); err != nil {
			t.Fatal(err)
		}
	}()
	return snapshotItems(t, db)
}

func assertSameItems(t *testing.T, want, got map[string]string, label string) {
	t.Helper()
	if len(want) != len(got) {
		t.Fatalf("%s: want %d items, got %d", label, len(want), len(got))
	}
	for k, v := range want {
		gv, ok := got[k]
		if !ok {
			t.Fatalf("%s: missing key %q", label, k)
		}
		if gv != v {
			t.Fatalf("%s: key %q = %q, want %q", label, k, gv, v)
		}
	}
}

func segmentFiles(t *testing.T, dir string) map[string]bool {
	t.Helper()
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	names := map[string]bool{}
	for _, e := range entries {
		if strings.HasSuffix(e.Name(), segmentExt) {
			names[e.Name()] = true
		}
	}
	return names
}

// fillCompactableDataset fills segment 0, leaves a partially written active
// segment, and fragments segment 0 with overwrites so Compact has work to
// do. The 42/40 numbers mirror TestCompaction.
func fillCompactableDataset(t *testing.T, db *DB) {
	t.Helper()
	for j := byte(0); j < 42; j++ {
		if err := db.Put([]byte{j}, []byte{j}); err != nil {
			t.Fatal(err)
		}
	}
	for j := byte(42); j < 46; j++ {
		if err := db.Put([]byte{j}, []byte{j}); err != nil {
			t.Fatal(err)
		}
	}
	for j := byte(0); j < 40; j++ {
		if err := db.Put([]byte{j}, []byte{j}); err != nil {
			t.Fatal(err)
		}
	}
}

// TestBackupCompaction verifies that a backup blocked on a slow destination
// no longer blocks Put, Delete or Compact: while the copy is paused, writes
// land and a compaction that actually retires segments completes. The
// reopened backup equals the boundary state, the live DB reflects the later
// writes, and a compacted segment id reused by a new segment does not leak
// into the backup.
func TestBackupCompaction(t *testing.T) {
	forEachRealFS(t, func(t *testing.T, srcFS fs.FileSystem) {
		dir := t.TempDir()
		live := filepath.Join(dir, "live")
		dst := filepath.Join(dir, "backup")

		entered := make(chan struct{})
		release := make(chan struct{})
		b := &barrierFS{
			FileSystem:   srcFS,
			dstDirs:      map[string]bool{dst: true},
			pausePrefix:  dst + string(os.PathSeparator),
			writeEntered: entered,
			writeRelease: release,
		}
		opts := &Options{
			FileSystem:                 b,
			maxSegmentSize:             1024,
			compactionMinSegmentSize:   520,
			compactionMinFragmentation: 0.1,
		}
		db, err := Open(live, opts)
		if err != nil {
			t.Fatal(err)
		}
		defer func() { _ = db.Close() }()

		fillCompactableDataset(t, db)

		// Boundary model and active segment geometry.
		boundary := snapshotItems(t, db)
		db.mu.RLock()
		activeName := db.datalog.curSeg.name
		activeSize := db.datalog.curSeg.size
		db.mu.RUnlock()

		done := make(chan error, 1)
		go func() { done <- db.Backup(dst) }()
		waitChannel(t, entered, "backup did not reach the destination write")

		// Writes proceed while the backup copy is blocked.
		if err := db.Put([]byte{0}, []byte("after")); err != nil {
			t.Fatal(err)
		}
		if err := db.Put([]byte{200}, []byte("new")); err != nil {
			t.Fatal(err)
		}
		if err := db.Delete([]byte{41}); err != nil {
			t.Fatal(err)
		}

		// The active segment was appended to after the boundary.
		liveInfo, err := os.Stat(filepath.Join(live, activeName))
		if err != nil {
			t.Fatal(err)
		}
		if liveInfo.Size() <= activeSize {
			t.Fatalf("active segment was not appended after the backup boundary: size=%d boundary=%d",
				liveInfo.Size(), activeSize)
		}

		// Compaction runs concurrently with the blocked backup and really
		// reclaims a segment instead of returning busy.
		cr, err := db.Compact()
		if err != nil {
			t.Fatalf("compact during backup: %v", err)
		}
		if cr.CompactedSegments == 0 {
			t.Fatal("compaction did not compact any segment while the backup was paused")
		}

		oldGen := segmentName(0, 1)
		if _, err := os.Stat(filepath.Join(live, oldGen)); err != nil {
			t.Fatalf("retained segment removed before the backup finished: %v", err)
		}

		// Drive writes until the physical segment id 0 is reused by a new
		// generation; the retained old file must keep its sequence id name.
		for i := 0; ; i++ {
			key := []byte{byte(i >> 8), byte(i)}
			if err := db.Put(key, []byte("reuse")); err != nil {
				t.Fatal(err)
			}
			for name := range segmentFiles(t, live) {
				id, seq, perr := parseSegmentName(name)
				if perr == nil && id == 0 && seq != 1 {
					goto reused
				}
			}
			if i > 1000 {
				t.Fatal("physical segment id 0 was not reused after compaction")
			}
		}
	reused:
		db.mu.RLock()
		newSeg := db.datalog.segments[0]
		if newSeg == nil || newSeg.sequenceID == 1 {
			t.Fatalf("segment id 0 was not reused: %+v", newSeg)
		}
		db.mu.RUnlock()

		close(release)
		if err := <-done; err != nil {
			t.Fatalf("backup: %v", err)
		}

		// Last reference released: the retired generation and its metadata
		// are purged, while the new generation stays.
		if _, err := os.Stat(filepath.Join(live, oldGen)); !os.IsNotExist(err) {
			t.Fatalf("retained segment %s was not purged: %v", oldGen, err)
		}
		if _, err := os.Stat(filepath.Join(live, oldGen+metaExt)); !os.IsNotExist(err) {
			t.Fatalf("retained segment metadata %s was not purged: %v", oldGen+metaExt, err)
		}
		if !segmentFiles(t, live)[newSeg.name] {
			t.Fatalf("new generation segment %s missing after backup", newSeg.name)
		}

		// Destination artifacts are checked before reopening the backup:
		// opening consumes the lock marker by treating it as a crash signal
		// and removing it on Close.
		if _, err := os.Stat(filepath.Join(dst, lockName)); err != nil {
			t.Fatalf("backup marker missing: %v", err)
		}
		dstInfo, err := os.Stat(filepath.Join(dst, activeName))
		if err != nil {
			t.Fatal(err)
		}
		if dstInfo.Size() != activeSize {
			t.Fatalf("backed up active segment size = %d, want boundary size %d",
				dstInfo.Size(), activeSize)
		}

		// Backup content equals the boundary state, including the active
		// segment truncated to its boundary length.
		backupItems := openBackupAndDump(t, dst)
		assertSameItems(t, boundary, backupItems, "backup snapshot")

		// The live DB kept the newer state.
		if v, err := db.Get([]byte{0}); err != nil || string(v) != "after" {
			t.Fatalf("live key 0 = %q, %v; want %q", v, err, "after")
		}
		if v, err := db.Get([]byte{200}); err != nil || string(v) != "new" {
			t.Fatalf("live key 200 = %q, %v; want %q", v, err, "new")
		}
		if has, err := db.Has([]byte{41}); err != nil || has {
			t.Fatalf("live key 41 present=%v, %v; want deleted", has, err)
		}

		// Handle accounting: one extra source and one destination handle at
		// a time, all released by the time Backup returned.
		b.mu.Lock()
		if b.srcLive != 0 || b.dstLive != 0 {
			t.Fatalf("leaked backup handles: srcLive=%d dstLive=%d", b.srcLive, b.dstLive)
		}
		if b.maxSrcLive > 1 || b.maxDstLive > 1 {
			t.Fatalf("backup held too many handles: maxSrc=%d maxDst=%d", b.maxSrcLive, b.maxDstLive)
		}
		srcOpens, dstOpens := b.srcOpens, b.dstOpens
		b.mu.Unlock()

		dstSegments := segmentFiles(t, dst)
		if srcOpens != int64(len(dstSegments)) || dstOpens != int64(len(dstSegments)) {
			t.Fatalf("open count mismatch: src=%d dst=%d segments=%d",
				srcOpens, dstOpens, len(dstSegments))
		}
	})
}

// TestBackupLifetimes interleaves two backups with different boundaries and
// a compaction. A backup that finishes first must not release a segment a
// still-running backup needs; only the last reference purges a retired
// segment and its metadata.
func TestBackupLifetimes(t *testing.T) {
	forEachRealFS(t, func(t *testing.T, srcFS fs.FileSystem) {
		dir := t.TempDir()
		live := filepath.Join(dir, "live")
		dstA := filepath.Join(dir, "backupA")
		dstB := filepath.Join(dir, "backupB")

		entered := make(chan struct{})
		release := make(chan struct{})
		b := &barrierFS{
			FileSystem:   srcFS,
			dstDirs:      map[string]bool{dstA: true, dstB: true},
			pausePrefix:  dstA + string(os.PathSeparator),
			writeEntered: entered,
			writeRelease: release,
		}
		opts := &Options{
			FileSystem:                 b,
			maxSegmentSize:             1024,
			compactionMinSegmentSize:   520,
			compactionMinFragmentation: 0.1,
		}
		db, err := Open(live, opts)
		if err != nil {
			t.Fatal(err)
		}
		defer func() { _ = db.Close() }()

		fillCompactableDataset(t, db)
		boundaryA := snapshotItems(t, db)

		doneA := make(chan error, 1)
		go func() { doneA <- db.Backup(dstA) }()
		waitChannel(t, entered, "backup A did not reach the destination write")

		// Move the live database forward before backup B captures it.
		if err := db.Put([]byte{0}, []byte("v2")); err != nil {
			t.Fatal(err)
		}
		if err := db.Put([]byte{99}, []byte("new")); err != nil {
			t.Fatal(err)
		}
		if err := db.Delete([]byte{40}); err != nil {
			t.Fatal(err)
		}
		boundaryB := snapshotItems(t, db)
		if boundaryA[string([]byte{0})] != string([]byte{0}) || boundaryB[string([]byte{0})] != "v2" {
			t.Fatal("boundary models do not distinguish the overwritten key 0")
		}
		if _, ok := boundaryA[string([]byte{99})]; ok {
			t.Fatal("key 99 must not exist at boundary A")
		}
		if _, ok := boundaryB[string([]byte{99})]; !ok {
			t.Fatal("key 99 must exist at boundary B")
		}
		if _, ok := boundaryB[string([]byte{40})]; ok {
			t.Fatal("key 40 must be deleted at boundary B")
		}

		// Backup B runs to completion while backup A stays blocked. Both
		// need the oldest segment.
		if err := db.Backup(dstB); err != nil {
			t.Fatalf("backup B: %v", err)
		}
		eventsAfterB := b.eventCount()

		// Compaction retires the oldest segment while backup A still needs
		// it; the files must survive because A's reference remains.
		oldGen := segmentName(0, 1)
		cr, err := db.Compact()
		if err != nil {
			t.Fatalf("compact: %v", err)
		}
		if cr.CompactedSegments == 0 {
			t.Fatal("compaction did not compact any segment")
		}
		if _, err := os.Stat(filepath.Join(live, oldGen)); err != nil {
			t.Fatalf("peer backup A needed %s but it was removed early: %v", oldGen, err)
		}
		if b.eventIndex("remove", oldGen) != -1 || b.eventIndex("remove", oldGen+metaExt) != -1 {
			t.Fatal("retired segment was removed while backup A was still running")
		}
		eventsAfterCompact := b.eventCount()

		close(release)
		if err := <-doneA; err != nil {
			t.Fatalf("backup A: %v", err)
		}

		// The removal happens only after backup A releases the last
		// reference, i.e. after backup B and the compaction events.
		removeIdx := b.eventIndex("remove", oldGen)
		if removeIdx == -1 {
			t.Fatalf("segment %s was never removed", oldGen)
		}
		if removeIdx < eventsAfterCompact || removeIdx < eventsAfterB {
			t.Fatalf("segment removed too early: removeIdx=%d afterB=%d afterCompact=%d",
				removeIdx, eventsAfterB, eventsAfterCompact)
		}
		if _, err := os.Stat(filepath.Join(live, oldGen)); !os.IsNotExist(err) {
			t.Fatalf("segment %s not purged after last release: %v", oldGen, err)
		}
		if _, err := os.Stat(filepath.Join(live, oldGen+metaExt)); !os.IsNotExist(err) {
			t.Fatalf("segment metadata %s not purged: %v", oldGen+metaExt, err)
		}

		itemsA := openBackupAndDump(t, dstA)
		itemsB := openBackupAndDump(t, dstB)
		assertSameItems(t, boundaryA, itemsA, "backup A")
		assertSameItems(t, boundaryB, itemsB, "backup B")

		liveItems := snapshotItems(t, db)
		assertSameItems(t, boundaryB, liveItems, "live database")
		if v := liveItems[string([]byte{0})]; v != "v2" {
			t.Fatalf("live key 0 = %q, want v2", v)
		}
		if _, ok := liveItems[string([]byte{40})]; ok {
			t.Fatal("live key 40 should be deleted")
		}

		b.mu.Lock()
		if b.srcLive != 0 || b.dstLive != 0 {
			t.Fatalf("leaked backup handles: srcLive=%d dstLive=%d", b.srcLive, b.dstLive)
		}
		// Two backups may each hold one source/destination handle while
		// overlapping, but they must never batch-open segments.
		if b.maxSrcLive > 2 || b.maxDstLive > 2 {
			t.Fatalf("too many concurrent backup handles: maxSrc=%d maxDst=%d",
				b.maxSrcLive, b.maxDstLive)
		}
		b.mu.Unlock()
	})
}

// TestBackupFailures injects a failure at every critical I/O point. The
// first primary error must survive errors.Is, every acquired handle and
// segment reference must be released, and a retry after the fault is
// cleared must produce a valid backup.
func TestBackupFailures(t *testing.T) {
	forEachRealFS(t, func(t *testing.T, srcFS fs.FileSystem) {
		faults := []struct {
			name  string
			apply func(b *barrierFS)
		}{
			{"destination create", func(b *barrierFS) { atomic.StoreInt32(&b.failDstOpen, 1) }},
			{"source read", func(b *barrierFS) { atomic.StoreInt32(&b.failSrcRead, 1) }},
			{"destination write", func(b *barrierFS) { atomic.StoreInt32(&b.failDstWrite, 1) }},
			{"destination close", func(b *barrierFS) { atomic.StoreInt32(&b.failDstClose, 1) }},
			{"backup marker", func(b *barrierFS) { atomic.StoreInt32(&b.failMarker, 1) }},
		}

		for _, fault := range faults {
			t.Run(fault.name, func(t *testing.T) {
				root := t.TempDir()
				live := filepath.Join(root, "live")
				dst := filepath.Join(root, "backup")
				retryDst := filepath.Join(root, "retry-backup")
				b := &barrierFS{
					FileSystem: srcFS,
					dstDirs:    map[string]bool{dst: true, retryDst: true},
				}
				db := newBarrierDB(t, b, live)

				fillCompactableDataset(t, db)
				fault.apply(b)

				err := db.Backup(dst)
				if !errors.Is(err, errInjected) {
					t.Fatalf("Backup error = %v, want errors.Is %v", err, errInjected)
				}

				// All extra handles were closed.
				b.mu.Lock()
				if b.srcLive != 0 || b.dstLive != 0 {
					t.Fatalf("handles leaked after failure: srcLive=%d dstLive=%d",
						b.srcLive, b.dstLive)
				}
				b.mu.Unlock()

				// Segment references were released: compaction can purge a
				// retired segment immediately, with no backup holding it.
				oldGen := segmentName(0, 1)
				cr, err := db.Compact()
				if err != nil {
					t.Fatalf("compact after failed backup: %v", err)
				}
				if cr.CompactedSegments == 0 {
					t.Fatal("compaction after failed backup did not reclaim anything")
				}
				if _, err := os.Stat(filepath.Join(live, oldGen)); !os.IsNotExist(err) {
					t.Fatalf("failed backup kept segment %s alive: %v", oldGen, err)
				}

				// Retry after clearing the fault produces a complete,
				// recoverable backup matching the current database state.
				atomic.StoreInt32(&b.failDstOpen, 0)
				atomic.StoreInt32(&b.failSrcRead, 0)
				atomic.StoreInt32(&b.failDstWrite, 0)
				atomic.StoreInt32(&b.failDstClose, 0)
				atomic.StoreInt32(&b.failMarker, 0)
				if err := db.Backup(retryDst); err != nil {
					t.Fatalf("retry backup: %v", err)
				}
				assertSameItems(t, snapshotItems(t, db), openBackupAndDump(t, retryDst), "retry backup")
				if err := db.Close(); err != nil {
					t.Fatal(err)
				}
			})
		}

		// A backup that errors while another backup is paused must not
		// release the files the still-running backup depends on.
		t.Run("erroring holder keeps peer files", func(t *testing.T) {
			root := t.TempDir()
			live := filepath.Join(root, "live")
			dstA := filepath.Join(root, "backupA")
			dstC := filepath.Join(root, "backupC")

			entered := make(chan struct{})
			release := make(chan struct{})
			b := &barrierFS{
				FileSystem:   srcFS,
				dstDirs:      map[string]bool{dstA: true, dstC: true},
				pausePrefix:  dstA + string(os.PathSeparator),
				failPrefix:   dstC + string(os.PathSeparator),
				writeEntered: entered,
				writeRelease: release,
			}
			atomic.StoreInt32(&b.failDstWrite, 1)
			opts := &Options{
				FileSystem:                 b,
				maxSegmentSize:             1024,
				compactionMinSegmentSize:   520,
				compactionMinFragmentation: 0.1,
			}
			db, err := Open(live, opts)
			if err != nil {
				t.Fatal(err)
			}
			defer func() { _ = db.Close() }()

			fillCompactableDataset(t, db)
			oldGen := segmentName(0, 1)

			doneA := make(chan error, 1)
			go func() { doneA <- db.Backup(dstA) }()
			waitChannel(t, entered, "backup A did not reach the destination write")

			err = db.Backup(dstC)
			if !errors.Is(err, errInjected) {
				t.Fatalf("backup C error = %v, want %v", err, errInjected)
			}

			// Backup C failed, but backup A still holds the segment.
			if _, err := os.Stat(filepath.Join(live, oldGen)); err != nil {
				t.Fatalf("peer backup lost segment %s: %v", oldGen, err)
			}
			cr, err := db.Compact()
			if err != nil {
				t.Fatalf("compact: %v", err)
			}
			if cr.CompactedSegments == 0 {
				t.Fatal("compaction did not compact any segment")
			}
			if _, err := os.Stat(filepath.Join(live, oldGen)); err != nil {
				t.Fatalf("segment %s removed while backup A is still running: %v", oldGen, err)
			}

			close(release)
			if err := <-doneA; err != nil {
				t.Fatalf("backup A: %v", err)
			}
			if _, err := os.Stat(filepath.Join(live, oldGen)); !os.IsNotExist(err) {
				t.Fatalf("segment %s not purged after last release: %v", oldGen, err)
			}
			if _, err := os.Stat(filepath.Join(live, oldGen+metaExt)); !os.IsNotExist(err) {
				t.Fatalf("segment metadata not purged: %v", err)
			}

			b.mu.Lock()
			if b.srcLive != 0 || b.dstLive != 0 {
				t.Fatalf("leaked backup handles: srcLive=%d dstLive=%d", b.srcLive, b.dstLive)
			}
			b.mu.Unlock()
		})
	})
}
