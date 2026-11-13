package mp4

import (
	"errors"
	"fmt"
	"io"
	"math"
	"math/bits"
	"sort"
	"time"

	"github.com/nareix/joy4/format/mp4/mp4io"
	"github.com/nareix/joy4/utils/bits/pio"
)

// rechunkCopyBufSize is the fixed buffer used while copying sample data.
// Media payloads are streamed through it, so an mdat is never loaded whole.
const rechunkCopyBufSize = 64 * 1024

const (
	boxFTYP = mp4io.Tag(0x66747970)
	boxFREE = mp4io.Tag(0x66726565)
	boxEDTS = mp4io.Tag(0x65647473)
)

var (
	errRechunkUnsupported = errors.New("mp4: rechunk input unsupported")
	errRechunkInvalid     = errors.New("mp4: rechunk invalid input")
)

func rechunkUnsupported(format string, args ...interface{}) error {
	return fmt.Errorf("%w: %s", errRechunkUnsupported, fmt.Sprintf(format, args...))
}

func rechunkInvalid(format string, args ...interface{}) error {
	return fmt.Errorf("%w: %s", errRechunkInvalid, fmt.Sprintf(format, args...))
}

// topLevelAtom describes one box found at the file root.
type topLevelAtom struct {
	tag  mp4io.Tag
	pos  int64
	size int64
	atom mp4io.Atom
}

// mdatRange is the payload range [start, end) of one mdat box.
type mdatRange struct {
	start int64
	end   int64
}

// srcSample is one sample located in the source file, in decode order.
type srcSample struct {
	offset int64
	size   int64
	dts    int64 // track timebase units
	dur    int64 // stts duration, track timebase units (last sample kept as-is)
}

// newChunk is a planned output chunk: the consecutive run of source samples
// [start, end) of one track.
type newChunk struct {
	track int
	start int
	end   int
	dts   int64
}

type rechunkState struct {
	src io.ReadSeeker
	dst io.WriteSeeker

	trackCount int
	samples    [][]srcSample
	tracks     []*mp4io.Track
	timeScales []int64
	mdats      []mdatRange
	ftyp       *mp4io.Dummy

	chunks [][]newChunk // per track, in decode order

	copyBuf []byte // fixed-size media copy buffer, reused for every sample
}

// readTopLevel walks the whole file once, accepting only the 32-bit-length,
// non-extended box layout and the root boxes rechunk knows how to handle.
func readTopLevel(r io.ReadSeeker) (atoms []topLevelAtom, err error) {
	if _, err = r.Seek(0, io.SeekStart); err != nil {
		return
	}

	hdr := make([]byte, 8)
	var pos int64
	for {
		if _, err = io.ReadFull(r, hdr); err != nil {
			if err == io.EOF {
				err = nil
			}
			return
		}

		size32 := pio.U32BE(hdr[0:])
		tag := mp4io.Tag(pio.U32BE(hdr[4:]))

		// size==1 means the real length is a 64-bit largesize; size==0 means
		// the box runs to end of file. Neither is representable with stco.
		if size32 == 1 || size32 == 0 {
			err = rechunkUnsupported("extended or open-ended box length for '%s'", tag)
			return
		}
		size := int64(size32)
		if size < 8 {
			err = rechunkInvalid("box '%s' has size %d < 8", tag, size)
			return
		}

		var atom mp4io.Atom
		switch tag {
		case mp4io.MOOV:
			atom = &mp4io.Movie{}
		case boxFTYP:
			// ftyp is small and is reproduced verbatim, header included.
			raw := make([]byte, size)
			copy(raw, hdr)
			if _, err = io.ReadFull(r, raw[8:]); err != nil {
				return
			}
			atom = &mp4io.Dummy{Tag_: tag, Data: raw}
		case mp4io.MDAT, boxFREE:
			// mdat payload is indexed, never buffered; free is dropped.
			if _, err = r.Seek(size-8, io.SeekCurrent); err != nil {
				return
			}
		default:
			err = rechunkUnsupported("top-level box '%s'", tag)
			return
		}

		if atom != nil {
			if movie, ok := atom.(*mp4io.Movie); ok {
				b := make([]byte, size)
				copy(b, hdr)
				if _, err = io.ReadFull(r, b[8:]); err != nil {
					return
				}
				if _, err = movie.Unmarshal(b, int(pos)); err != nil {
					return
				}
			}
		}

		atoms = append(atoms, topLevelAtom{tag: tag, pos: pos, size: size, atom: atom})
		pos += size
	}
}

// checkStblChildren scans the raw stbl box so a table the typed parser would
// skip silently (co64 in particular) is rejected instead of being dropped.
func checkStblChildren(r io.ReadSeeker, stbl *mp4io.SampleTable) error {
	base, size := stbl.Pos()
	end := int64(base + size)
	pos := int64(base) + 8

	hdr := make([]byte, 8)
	for pos < end {
		if end-pos < 8 {
			return rechunkInvalid("stbl: truncated child box header at %d", pos)
		}
		if _, err := r.Seek(pos, io.SeekStart); err != nil {
			return err
		}
		if _, err := io.ReadFull(r, hdr); err != nil {
			return err
		}
		childSize := int64(pio.U32BE(hdr[0:]))
		childTag := mp4io.Tag(pio.U32BE(hdr[4:]))
		if childSize < 8 || pos+childSize > end {
			return rechunkInvalid("stbl: child '%s' has invalid size %d", childTag, childSize)
		}
		switch childTag {
		case mp4io.STSD, mp4io.STTS, mp4io.CTTS, mp4io.STSC,
			mp4io.STSS, mp4io.STCO, mp4io.STSZ:
		default:
			return rechunkUnsupported("stbl child box '%s'", childTag)
		}
		pos += childSize
	}
	return nil
}

// expandTrack turns stsc/stco/stsz into one entry per sample and validates
// every table against the others. Samples stay in source decode order.
func (st *rechunkState) expandTrack(trackIdx int, t *mp4io.Track) (samples []srcSample, err error) {
	mdia := t.Media
	if mdia == nil || mdia.Info == nil || mdia.Info.Sample == nil {
		err = rechunkInvalid("track %d: sample table missing", trackIdx)
		return
	}
	stbl := mdia.Info.Sample

	if err = checkStblChildren(st.src, stbl); err != nil {
		return
	}

	// No edit lists: an elst would remap the decode timeline rechunk keeps.
	for _, a := range t.Unknowns {
		if a.Tag() == boxEDTS {
			err = rechunkUnsupported("track %d: edit list", trackIdx)
			return
		}
	}
	for _, a := range mdia.Unknowns {
		if a.Tag() == boxEDTS {
			err = rechunkUnsupported("track %d: edit list", trackIdx)
			return
		}
	}

	if stbl.SampleDesc == nil || stbl.TimeToSample == nil ||
		stbl.SampleToChunk == nil || stbl.ChunkOffset == nil || stbl.SampleSize == nil {
		err = rechunkInvalid("track %d: incomplete sample table", trackIdx)
		return
	}

	// Exactly one sample description per track, and it must be avc1 or mp4a.
	descCount := 0
	if stbl.SampleDesc.AVC1Desc != nil {
		descCount++
	}
	if stbl.SampleDesc.MP4ADesc != nil {
		descCount++
	}
	descCount += len(stbl.SampleDesc.Unknowns)
	if stbl.SampleDesc.AVC1Desc == nil && stbl.SampleDesc.MP4ADesc == nil {
		err = rechunkUnsupported("track %d: sample description is neither avc1 nor mp4a", trackIdx)
		return
	}
	if descCount != 1 {
		err = rechunkUnsupported("track %d: %d sample descriptions, want exactly 1", trackIdx, descCount)
		return
	}

	// Per-sample stsz only: a uniform SampleSize cannot describe the variable
	// samples rechunk regroups, and co64 offsets were rejected above.
	if stbl.SampleSize.SampleSize != 0 {
		err = rechunkUnsupported("track %d: uniform stsz sample size", trackIdx)
		return
	}

	stsc := stbl.SampleToChunk.Entries
	stco := stbl.ChunkOffset.Entries
	stsz := stbl.SampleSize.Entries

	if len(stsc) == 0 {
		err = rechunkInvalid("track %d: empty stsc", trackIdx)
		return
	}
	if len(stco) == 0 {
		err = rechunkInvalid("track %d: empty stco", trackIdx)
		return
	}

	// stsc runs must start at chunk 1 and advance monotonically.
	for i, e := range stsc {
		if e.FirstChunk < 1 {
			err = rechunkInvalid("track %d: stsc first_chunk %d out of range", trackIdx, e.FirstChunk)
			return
		}
		if e.SamplesPerChunk == 0 {
			err = rechunkInvalid("track %d: stsc samples_per_chunk is 0", trackIdx)
			return
		}
		if e.SampleDescId != 1 {
			err = rechunkUnsupported("track %d: stsc sample_description_index %d, want 1", trackIdx, e.SampleDescId)
			return
		}
		if i > 0 && e.FirstChunk <= stsc[i-1].FirstChunk {
			err = rechunkInvalid("track %d: stsc first_chunk not strictly increasing", trackIdx)
			return
		}
	}
	if stsc[0].FirstChunk != 1 {
		err = rechunkInvalid("track %d: stsc must start at chunk 1, got %d", trackIdx, stsc[0].FirstChunk)
		return
	}

	// Walk the chunks, expanding each one into its samples.
	samples = make([]srcSample, 0, len(stsz))
	runIdx := 0
	var inChunkOffset int64
	for chunkIdx := 0; chunkIdx < len(stco); chunkIdx++ {
		if runIdx+1 < len(stsc) && uint32(chunkIdx+1) >= stsc[runIdx+1].FirstChunk {
			runIdx++
		}
		n := int(stsc[runIdx].SamplesPerChunk)

		chunkStart := int64(stco[chunkIdx])
		inChunkOffset = 0
		for j := 0; j < n; j++ {
			sampleIdx := len(samples)
			if sampleIdx >= len(stsz) {
				err = rechunkInvalid("track %d: stsc covers more samples than stsz (%d)", trackIdx, len(stsz))
				return
			}
			size := int64(stsz[sampleIdx])
			if size == 0 {
				err = rechunkInvalid("track %d: sample %d has zero size", trackIdx, sampleIdx)
				return
			}
			samples = append(samples, srcSample{
				offset: chunkStart + inChunkOffset,
				size:   size,
			})
			inChunkOffset += size
		}
	}
	if len(samples) != len(stsz) {
		err = rechunkInvalid("track %d: stsc covers %d samples but stsz has %d", trackIdx, len(samples), len(stsz))
		return
	}

	// Decorate with decode timestamps and the real per-sample stts durations,
	// keeping everything in the track's own integer time base. A genuinely
	// zero-duration final sample is preserved rather than synthesized away.
	dts, idx := int64(0), 0
	for _, e := range stbl.TimeToSample.Entries {
		if e.Count == 0 {
			err = rechunkInvalid("track %d: stts entry count is 0", trackIdx)
			return
		}
		for j := uint32(0); j < e.Count; j++ {
			if idx >= len(samples) {
				err = rechunkInvalid("track %d: stts covers more samples than stsz", trackIdx)
				return
			}
			samples[idx].dts = dts
			samples[idx].dur = int64(e.Duration)
			idx++
			dts += int64(e.Duration)
		}
	}
	if idx != len(samples) {
		err = rechunkInvalid("track %d: stts covers %d samples but stsz has %d", trackIdx, idx, len(samples))
		return
	}

	// ctts, when present, must cover every sample; the table itself is copied
	// unchanged, so decode order never gets confused with display order.
	if stbl.CompositionOffset != nil && len(stbl.CompositionOffset.Entries) > 0 {
		covered := 0
		for _, e := range stbl.CompositionOffset.Entries {
			covered += int(e.Count)
		}
		if covered != len(samples) {
			err = rechunkInvalid("track %d: ctts covers %d samples but stsz has %d", trackIdx, covered, len(samples))
			return
		}
	}
	if stbl.SyncSample != nil {
		last := uint32(0)
		for _, n := range stbl.SyncSample.Entries {
			if n < 1 || int(n) > len(samples) {
				err = rechunkInvalid("track %d: stss entry %d out of range", trackIdx, n)
				return
			}
			if n <= last {
				err = rechunkInvalid("track %d: stss entries not strictly increasing", trackIdx)
				return
			}
			last = n
		}
	}

	return
}

// containsRange reports whether [off, off+size) lies fully inside one mdat
// payload. Gaps between a source chunk's samples are therefore rejected too.
func rangeInMdats(mdats []mdatRange, off, size int64) bool {
	if size <= 0 || off < 0 {
		return false
	}
	end := off + size
	if end < off { // int64 overflow
		return false
	}
	for _, m := range mdats {
		if off >= m.start && end <= m.end {
			return true
		}
	}
	return false
}

// planChunks groups each track's samples by accumulated decode duration. The
// running total may equal chunkDuration; adding the next sample would have to
// exceed it to start a new chunk. A sample longer than the budget owns a chunk.
// Grouping uses the actual stts durations, so even the final sample counts.
func planChunks(samples []srcSample, timeScale int64, chunkDuration time.Duration) (chunks []newChunk) {
	budget := chunkDuration.Nanoseconds() * timeScale / int64(time.Second)
	if budget <= 0 {
		// timeScale is validated as positive by the caller; guard anyway.
		budget = 1
	}

	start := 0
	acc := samples[0].dur // duration already accumulated for the open chunk
	for i := 1; i < len(samples); i++ {
		d := samples[i].dur
		if acc+d > budget {
			chunks = append(chunks, newChunk{start: start, end: i, dts: samples[start].dts})
			start = i
			acc = d
			continue
		}
		acc += d
	}
	chunks = append(chunks, newChunk{start: start, end: len(samples), dts: samples[start].dts})
	return
}

// buildStsc merges the planned chunks into stsc runs, coalescing neighbours
// with the same samples_per_chunk. sample_description_index stays 1.
func buildStsc(chunks []newChunk) []mp4io.SampleToChunkEntry {
	entries := make([]mp4io.SampleToChunkEntry, 0, len(chunks))
	for i, c := range chunks {
		n := uint32(c.end - c.start)
		if len(entries) > 0 && entries[len(entries)-1].SamplesPerChunk == n {
			continue
		}
		entries = append(entries, mp4io.SampleToChunkEntry{
			FirstChunk:      uint32(i + 1),
			SamplesPerChunk: n,
			SampleDescId:    1,
		})
	}
	return entries
}

// cmpFraction compares aNum/aDen and bNum/bDen exactly, using 128-bit
// products so cross-time-base ordering never goes through float seconds.
// All arguments are non-negative and denominators positive.
func cmpFraction(aNum, aDen, bNum, bDen int64) int {
	hi1, lo1 := bits.Mul64(uint64(aNum), uint64(bDen))
	hi2, lo2 := bits.Mul64(uint64(bNum), uint64(aDen))
	if hi1 != hi2 {
		if hi1 < hi2 {
			return -1
		}
		return 1
	}
	if lo1 == lo2 {
		return 0
	}
	if lo1 < lo2 {
		return -1
	}
	return 1
}

// writeSample copies one sample's bytes through the shared fixed-size buffer.
func (st *rechunkState) writeSample(s srcSample) (err error) {
	if _, err = st.src.Seek(s.offset, io.SeekStart); err != nil {
		return
	}
	if st.copyBuf == nil {
		st.copyBuf = make([]byte, rechunkCopyBufSize)
	}
	buf := st.copyBuf
	remaining := s.size
	for remaining > 0 {
		n := len(buf)
		if int64(n) > remaining {
			n = int(remaining)
		}
		if _, err = io.ReadFull(st.src, buf[:n]); err != nil {
			return
		}
		if _, err = st.dst.Write(buf[:n]); err != nil {
			return
		}
		remaining -= int64(n)
	}
	return
}

// Rechunk rewrites a finished, non-fragmented MP4 into a fast-start layout:
// an optional ftyp, then moov, then a single mdat whose chunks hold multiple
// samples and are interleaved by each chunk's first sample DTS. Compressed
// sample bytes and every timing table are preserved byte for byte; no decoder
// or packet remuxing is involved.
//
// src and dst are distinct objects both positioned at offset 0, and dst is
// empty. All validation happens before dst is touched: a rejected input leaves
// dst unwritten, while I/O errors during the actual copy are returned as-is.
func Rechunk(src io.ReadSeeker, dst io.WriteSeeker, chunkDuration time.Duration) error {
	if chunkDuration <= 0 {
		return rechunkInvalid("chunkDuration must be positive, got %s", chunkDuration)
	}
	if src == nil || dst == nil {
		return rechunkInvalid("nil source or destination")
	}

	st := &rechunkState{src: src, dst: dst}

	topAtoms, err := readTopLevel(src)
	if err != nil {
		return err
	}

	var movie *mp4io.Movie
	for _, a := range topAtoms {
		switch a.tag {
		case mp4io.MOOV:
			if movie != nil {
				return rechunkInvalid("multiple moov boxes")
			}
			movie = a.atom.(*mp4io.Movie)
		case boxFTYP:
			if st.ftyp != nil {
				return rechunkInvalid("multiple ftyp boxes")
			}
			st.ftyp = a.atom.(*mp4io.Dummy)
		case mp4io.MDAT:
			st.mdats = append(st.mdats, mdatRange{
				start: a.pos + 8,
				end:   a.pos + a.size,
			})
		}
	}
	if movie == nil {
		return rechunkInvalid("moov box not found")
	}
	if len(movie.Tracks) == 0 {
		return rechunkInvalid("moov has no tracks")
	}
	if movie.MovieExtend != nil {
		return rechunkUnsupported("fragmented movie (mvex)")
	}
	if len(st.mdats) == 0 {
		return rechunkInvalid("no mdat box")
	}
	for _, m := range st.mdats {
		if m.end <= m.start {
			return rechunkInvalid("empty mdat box")
		}
	}

	st.trackCount = len(movie.Tracks)
	st.tracks = movie.Tracks
	st.samples = make([][]srcSample, st.trackCount)
	st.timeScales = make([]int64, st.trackCount)
	st.chunks = make([][]newChunk, st.trackCount)

	for i, t := range movie.Tracks {
		if t.Media == nil || t.Media.Header == nil {
			return rechunkInvalid("track %d: media header missing", i)
		}
		ts := int64(t.Media.Header.TimeScale)
		if ts <= 0 {
			return rechunkInvalid("track %d: non-positive time scale %d", i, ts)
		}
		st.timeScales[i] = ts

		samples, err := st.expandTrack(i, t)
		if err != nil {
			return err
		}
		if len(samples) == 0 {
			return rechunkInvalid("track %d: no samples", i)
		}
		for _, s := range samples {
			if !rangeInMdats(st.mdats, s.offset, s.size) {
				return rechunkInvalid("track %d: sample at offset %d size %d is not fully inside an mdat payload", i, s.offset, s.size)
			}
		}
		st.samples[i] = samples
		st.chunks[i] = planChunks(samples, ts, chunkDuration)
	}

	// Flatten and order all chunks by first-sample DTS. Cross-track equality is
	// resolved by trak order; within one track decode order is already intact.
	// A stable sort keeps equal keys (including two zero-duration chunks of one
	// track) in their original order.
	type ordered struct {
		track int
		idx   int
	}
	all := make([]ordered, 0)
	for ti, cs := range st.chunks {
		for ci := range cs {
			all = append(all, ordered{ti, ci})
		}
	}
	sort.SliceStable(all, func(i, j int) bool {
		a, b := all[i], all[j]
		ca := st.chunks[a.track][a.idx]
		cb := st.chunks[b.track][b.idx]
		cmp := cmpFraction(ca.dts, st.timeScales[a.track], cb.dts, st.timeScales[b.track])
		if cmp != 0 {
			return cmp < 0
		}
		return a.track < b.track
	})

	type placed struct {
		track  int
		chunk  newChunk
		offset int64
	}
	plan := make([]placed, len(all))
	for i, o := range all {
		plan[i] = placed{track: o.track, chunk: st.chunks[o.track][o.idx]}
	}

	// Total payload size, summing the per-sample sizes the new layout keeps.
	var payloadSize int64
	for _, s := range st.samples {
		for _, sm := range s {
			payloadSize += sm.size
		}
	}

	// Rewrite only stco and stsc. stsd/stsz/stts/ctts/stss and every identity
	// and duration field are left exactly as the source had them.
	newOffsets := make([][]uint32, st.trackCount)
	for ti := range st.tracks {
		stbl := st.tracks[ti].Media.Info.Sample
		newOffsets[ti] = make([]uint32, len(st.chunks[ti]))
		stbl.ChunkOffset.Entries = newOffsets[ti]
		stbl.SampleToChunk.Entries = buildStsc(st.chunks[ti])
	}

	// Layout: [ftyp][moov][mdat header][payload]. The stco entry counts are
	// already final, so moov.Len() is the size the offsets must account for.
	var headerSize int64
	if st.ftyp != nil {
		headerSize += int64(st.ftyp.Len())
	}
	moovSize := int64(movie.Len())
	const mdatHeaderSize = 8
	mdatStart := headerSize + moovSize + mdatHeaderSize
	totalSize := mdatStart + payloadSize
	if totalSize > math.MaxUint32 {
		return rechunkInvalid("output size %d exceeds 32-bit box addressing", totalSize)
	}

	// Assign each chunk its absolute offset in write order.
	off := mdatStart
	for i := range plan {
		if off > math.MaxUint32 {
			return rechunkInvalid("chunk offset %d exceeds 32-bit range", off)
		}
		plan[i].offset = off
		for k := plan[i].chunk.start; k < plan[i].chunk.end; k++ {
			off += st.samples[plan[i].track][k].size
		}
	}

	// Fill stco per track in the track's own chunk order.
	cursor := make([]int, st.trackCount)
	for _, p := range plan {
		newOffsets[p.track][cursor[p.track]] = uint32(p.offset)
		cursor[p.track]++
	}

	// Everything above was pure validation and table math: dst has not been
	// written. From here on, I/O errors are surfaced without rollback.
	if _, err = dst.Seek(0, io.SeekStart); err != nil {
		return err
	}

	if st.ftyp != nil {
		b := make([]byte, st.ftyp.Len())
		st.ftyp.Marshal(b)
		if _, err = dst.Write(b); err != nil {
			return err
		}
	}

	moovBytes := make([]byte, movie.Len())
	movie.Marshal(moovBytes)
	if _, err = dst.Write(moovBytes); err != nil {
		return err
	}

	mdatHdr := make([]byte, 8)
	pio.PutU32BE(mdatHdr[0:], uint32(mdatHeaderSize+payloadSize))
	pio.PutU32BE(mdatHdr[4:], uint32(mp4io.MDAT))
	if _, err = dst.Write(mdatHdr); err != nil {
		return err
	}

	for _, p := range plan {
		for k := p.chunk.start; k < p.chunk.end; k++ {
			if err = st.writeSample(st.samples[p.track][k]); err != nil {
				return err
			}
		}
	}

	return nil
}
