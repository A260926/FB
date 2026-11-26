package mp4

import (
	"bufio"
	"encoding/binary"
	"fmt"
	"io"
	"math/bits"
	"sort"
	"time"

	"github.com/nareix/joy4/utils/bits/pio"
)

const (
	rechunkMaxUint32 = uint64(1)<<32 - 1
	rechunkMaxInt64  = uint64(1)<<63 - 1
)

// Rechunk rewrites a completed, non-fragmented MP4 file so that the moov box
// comes first and samples are stored in multi-sample chunks interleaved by
// decode time. Samples, their order and their timing tables are not changed;
// only the physical layout (stco/stsc) is rebuilt.
//
// src and dst must be distinct objects positioned at the start of their
// respective files, and dst must be empty. The input must be a plain,
// self-contained file with avc1/mp4a tracks using 32-bit box sizes, 32-bit
// chunk offsets (stco) and per-sample sizes (stsz). Fragmented files, co64,
// extended box lengths, edit lists, encryption and unknown top-level boxes are
// rejected with an error rather than silently dropped.
func Rechunk(src io.ReadSeeker, dst io.WriteSeeker, chunkDuration time.Duration) error {
	if chunkDuration <= 0 {
		return fmt.Errorf("mp4: rechunk: chunk duration must be positive, got %v", chunkDuration)
	}
	if src == nil {
		return fmt.Errorf("mp4: rechunk: nil source")
	}
	if dst == nil {
		return fmt.Errorf("mp4: rechunk: nil destination")
	}

	fileSize, err := src.Seek(0, io.SeekEnd)
	if err != nil {
		return err
	}
	if _, err = src.Seek(0, io.SeekStart); err != nil {
		return err
	}

	ftyp, moovBox, mdats, err := rechunkReadTop(src, fileSize)
	if err != nil {
		return err
	}
	if moovBox == nil {
		return fmt.Errorf("mp4: rechunk: 'moov' box not found")
	}

	tracks, err := rechunkParseMoov(moovBox[8:])
	if err != nil {
		return err
	}

	// Every source sample must be fully contained in an mdat payload before
	// anything is written.
	for _, tr := range tracks {
		for i := range tr.samples {
			s := &tr.samples[i]
			if !rechunkInMdat(mdats, s.offset, s.size) {
				return fmt.Errorf("mp4: rechunk: track %d sample %d range [%d,%d) is not inside an mdat payload", tr.index, i, s.offset, int64(s.offset)+int64(s.size))
			}
		}
	}

	var chunks []*rechunkChunk
	for _, tr := range tracks {
		tr.chunks = rechunkPlanTrack(tr, chunkDuration)
		chunks = append(chunks, tr.chunks...)
	}

	sort.SliceStable(chunks, func(i, j int) bool {
		return rechunkChunkLess(chunks[i], chunks[j], tracks)
	})

	var total int64
	for _, c := range chunks {
		if c.byteLen < 0 || uint64(c.byteLen) > rechunkMaxInt64-uint64(total) {
			return fmt.Errorf("mp4: rechunk: media data size overflows")
		}
		total += c.byteLen
	}
	if uint64(total) > rechunkMaxUint32-8 {
		return fmt.Errorf("mp4: rechunk: output mdat size exceeds 32-bit limit")
	}

	// First pass with zero offsets only to determine the final moov size.
	for _, tr := range tracks {
		for _, c := range tr.chunks {
			c.offset = 0
		}
	}
	moovBytes, err := rechunkBuildMoov(moovBox[8:], tracks)
	if err != nil {
		return err
	}
	if uint64(len(moovBytes)) > rechunkMaxUint32 {
		return fmt.Errorf("mp4: rechunk: output moov size exceeds 32-bit limit")
	}

	base := int64(len(ftyp)) + int64(len(moovBytes)) + 8
	if uint64(base) > rechunkMaxUint32 {
		return fmt.Errorf("mp4: rechunk: output offset exceeds 32-bit limit")
	}
	pos := base
	for _, c := range chunks {
		if uint64(pos) > rechunkMaxUint32 || uint64(c.byteLen) > rechunkMaxUint32-uint64(pos) {
			return fmt.Errorf("mp4: rechunk: output offset exceeds 32-bit limit")
		}
		c.offset = uint32(pos)
		pos += c.byteLen
	}
	if uint64(pos) > rechunkMaxUint32 {
		return fmt.Errorf("mp4: rechunk: output size exceeds 32-bit limit")
	}

	// Second pass now that every chunk has its final offset.
	moovBytes2, err := rechunkBuildMoov(moovBox[8:], tracks)
	if err != nil {
		return err
	}
	if len(moovBytes2) != len(moovBytes) {
		return fmt.Errorf("mp4: rechunk: internal error: moov size changed")
	}
	moovBytes = moovBytes2

	// Validation is complete. From here on dst may be written; read/write
	// errors are returned unchanged.
	if _, err = dst.Seek(0, io.SeekStart); err != nil {
		return err
	}
	bw := bufio.NewWriterSize(dst, pio.RecommendBufioSize)
	if len(ftyp) > 0 {
		if _, err = bw.Write(ftyp); err != nil {
			return err
		}
	}
	if _, err = bw.Write(moovBytes); err != nil {
		return err
	}
	var mdatHeader [8]byte
	binary.BigEndian.PutUint32(mdatHeader[0:4], uint32(8+total))
	copy(mdatHeader[4:8], "mdat")
	if _, err = bw.Write(mdatHeader[:]); err != nil {
		return err
	}

	copyBuf := make([]byte, rechunkCopyBufSize)
	if err = rechunkCopySamples(bw, src, chunks, tracks, copyBuf); err != nil {
		return err
	}
	return bw.Flush()
}

const (
	rechunkCopyBufSize = 64 * 1024
)

type rechunkMdatRange struct {
	start int64
	end   int64
}

type rechunkSample struct {
	offset int64
	size   uint32
	dts    int64
	dur    uint64
}

type rechunkChunk struct {
	track   int
	first   int
	count   int
	byteLen int64
	offset  uint32
}

type rechunkTrack struct {
	index     int
	timescale uint64
	samples   []rechunkSample
	chunks    []*rechunkChunk

	trakPayload []byte
	mdiaPayload []byte
	minfPayload []byte
	stblPayload []byte
}

type rechunkBoxRef struct {
	tag  string
	off  int
	size int
}

type rechunkStscEntry struct {
	firstChunk      uint32
	samplesPerChunk uint32
	descID          uint32
}

func rechunkInMdat(ranges []rechunkMdatRange, off int64, size uint32) bool {
	end := off + int64(size)
	if end < off {
		return false
	}
	for _, r := range ranges {
		if off >= r.start && end <= r.end {
			return true
		}
	}
	return false
}

func rechunkReadTop(r io.ReadSeeker, fileSize int64) (ftyp, moov []byte, mdats []rechunkMdatRange, err error) {
	var pos int64
	hdr := make([]byte, 8)
	for pos < fileSize {
		if fileSize-pos < 8 {
			return nil, nil, nil, fmt.Errorf("mp4: rechunk: truncated box header at offset %d", pos)
		}
		if _, err = io.ReadFull(r, hdr); err != nil {
			return
		}
		size := binary.BigEndian.Uint32(hdr[0:4])
		tag := string(hdr[4:8])
		if size == 1 {
			return nil, nil, nil, fmt.Errorf("mp4: rechunk: unsupported extended box length for %q", tag)
		}
		if size == 0 {
			// A zero length is only meaningful for a final mdat that runs to
			// the end of the file.
			if tag != "mdat" {
				return nil, nil, nil, fmt.Errorf("mp4: rechunk: unsupported box %q with zero length", tag)
			}
			if fileSize-pos < 8 {
				return nil, nil, nil, fmt.Errorf("mp4: rechunk: truncated mdat at offset %d", pos)
			}
			mdats = append(mdats, rechunkMdatRange{start: pos + 8, end: fileSize})
			pos = fileSize
			continue
		}
		if size < 8 {
			return nil, nil, nil, fmt.Errorf("mp4: rechunk: invalid box %q size %d", tag, size)
		}
		boxSize := int64(size)
		if boxSize > fileSize-pos {
			return nil, nil, nil, fmt.Errorf("mp4: rechunk: truncated box %q at offset %d", tag, pos)
		}
		switch tag {
		case "ftyp":
			if ftyp != nil {
				return nil, nil, nil, fmt.Errorf("mp4: rechunk: duplicate ftyp box")
			}
			b := make([]byte, boxSize)
			copy(b, hdr)
			if _, err = io.ReadFull(r, b[8:]); err != nil {
				return
			}
			ftyp = b
		case "moov":
			if moov != nil {
				return nil, nil, nil, fmt.Errorf("mp4: rechunk: duplicate moov box")
			}
			b := make([]byte, boxSize)
			copy(b, hdr)
			if _, err = io.ReadFull(r, b[8:]); err != nil {
				return
			}
			moov = b
		case "mdat":
			mdats = append(mdats, rechunkMdatRange{start: pos + 8, end: pos + boxSize})
			if _, err = r.Seek(pos+boxSize, io.SeekStart); err != nil {
				return
			}
		case "free":
			if _, err = r.Seek(pos+boxSize, io.SeekStart); err != nil {
				return
			}
		default:
			return nil, nil, nil, fmt.Errorf("mp4: rechunk: unsupported top-level box %q", tag)
		}
		pos += boxSize
	}
	return
}

func rechunkScanBoxes(payload []byte) ([]rechunkBoxRef, error) {
	var refs []rechunkBoxRef
	for off := 0; off < len(payload); {
		if len(payload)-off < 8 {
			return nil, fmt.Errorf("mp4: rechunk: truncated box header")
		}
		size := binary.BigEndian.Uint32(payload[off : off+4])
		tag := string(payload[off+4 : off+8])
		if size == 1 {
			return nil, fmt.Errorf("mp4: rechunk: unsupported extended box length for %q", tag)
		}
		if size == 0 {
			return nil, fmt.Errorf("mp4: rechunk: unsupported box %q with zero length", tag)
		}
		if size < 8 {
			return nil, fmt.Errorf("mp4: rechunk: invalid box %q size %d", tag, size)
		}
		if int64(size) > int64(len(payload)-off) {
			return nil, fmt.Errorf("mp4: rechunk: truncated box %q", tag)
		}
		refs = append(refs, rechunkBoxRef{tag: tag, off: off, size: int(size)})
		off += int(size)
	}
	return refs, nil
}

func rechunkMakeBox(tag string, payload []byte) ([]byte, error) {
	if len(tag) != 4 {
		return nil, fmt.Errorf("mp4: rechunk: invalid box tag %q", tag)
	}
	n := int64(len(payload)) + 8
	if n < 8 || uint64(n) > rechunkMaxUint32 {
		return nil, fmt.Errorf("mp4: rechunk: box %q size exceeds 32-bit limit", tag)
	}
	b := make([]byte, int(n))
	binary.BigEndian.PutUint32(b[0:4], uint32(n))
	copy(b[4:8], tag)
	copy(b[8:], payload)
	return b, nil
}

func rechunkParseMoov(payload []byte) ([]*rechunkTrack, error) {
	refs, err := rechunkScanBoxes(payload)
	if err != nil {
		return nil, err
	}
	var tracks []*rechunkTrack
	for _, ref := range refs {
		switch ref.tag {
		case "trak":
			tr, err := rechunkParseTrak(payload[ref.off:ref.off+ref.size], len(tracks))
			if err != nil {
				return nil, err
			}
			tracks = append(tracks, tr)
		case "mvex":
			return nil, fmt.Errorf("mp4: rechunk: unsupported fragmented movie (mvex)")
		}
	}
	return tracks, nil
}

func rechunkParseTrak(raw []byte, index int) (*rechunkTrack, error) {
	payload := raw[8:]
	refs, err := rechunkScanBoxes(payload)
	if err != nil {
		return nil, err
	}
	tr := &rechunkTrack{index: index, trakPayload: payload}
	var mdia *rechunkBoxRef
	for i := range refs {
		ref := &refs[i]
		switch ref.tag {
		case "edts":
			return nil, fmt.Errorf("mp4: rechunk: unsupported edit list (edts) in track %d", index)
		case "mdia":
			if mdia != nil {
				return nil, fmt.Errorf("mp4: rechunk: duplicate mdia in track %d", index)
			}
			mdia = ref
		}
	}
	if mdia == nil {
		return nil, fmt.Errorf("mp4: rechunk: track %d has no mdia box", index)
	}
	return rechunkParseMdia(payload[mdia.off:mdia.off+mdia.size], tr)
}

func rechunkParseMdia(raw []byte, tr *rechunkTrack) (*rechunkTrack, error) {
	payload := raw[8:]
	tr.mdiaPayload = payload
	refs, err := rechunkScanBoxes(payload)
	if err != nil {
		return nil, err
	}
	var mdhd, minf *rechunkBoxRef
	for i := range refs {
		ref := &refs[i]
		switch ref.tag {
		case "mdhd":
			if mdhd != nil {
				return nil, fmt.Errorf("mp4: rechunk: duplicate mdhd in track %d", tr.index)
			}
			mdhd = ref
		case "minf":
			if minf != nil {
				return nil, fmt.Errorf("mp4: rechunk: duplicate minf in track %d", tr.index)
			}
			minf = ref
		}
	}
	if mdhd == nil {
		return nil, fmt.Errorf("mp4: rechunk: track %d has no mdhd box", tr.index)
	}
	if minf == nil {
		return nil, fmt.Errorf("mp4: rechunk: track %d has no minf box", tr.index)
	}
	if tr.timescale, err = rechunkParseMdhdTimescale(payload[mdhd.off : mdhd.off+mdhd.size]); err != nil {
		return nil, err
	}
	return rechunkParseMinf(payload[minf.off:minf.off+minf.size], tr)
}

func rechunkParseMdhdTimescale(raw []byte) (uint64, error) {
	payload := raw[8:]
	if len(payload) < 4 {
		return 0, fmt.Errorf("mp4: rechunk: truncated mdhd")
	}
	var off int
	switch payload[0] {
	case 0:
		off = 4 + 4 + 4
	case 1:
		off = 4 + 8 + 8
	default:
		return 0, fmt.Errorf("mp4: rechunk: unsupported mdhd version %d", payload[0])
	}
	if len(payload) < off+4 {
		return 0, fmt.Errorf("mp4: rechunk: truncated mdhd")
	}
	timeScale := int32(binary.BigEndian.Uint32(payload[off : off+4]))
	if timeScale <= 0 {
		return 0, fmt.Errorf("mp4: rechunk: invalid mdhd timescale %d", timeScale)
	}
	return uint64(timeScale), nil
}

func rechunkParseMinf(raw []byte, tr *rechunkTrack) (*rechunkTrack, error) {
	payload := raw[8:]
	tr.minfPayload = payload
	refs, err := rechunkScanBoxes(payload)
	if err != nil {
		return nil, err
	}
	var stbl *rechunkBoxRef
	for i := range refs {
		ref := &refs[i]
		if ref.tag == "stbl" {
			if stbl != nil {
				return nil, fmt.Errorf("mp4: rechunk: duplicate stbl in track %d", tr.index)
			}
			stbl = ref
		}
	}
	if stbl == nil {
		return nil, fmt.Errorf("mp4: rechunk: track %d has no stbl box", tr.index)
	}
	return rechunkParseStbl(payload[stbl.off:stbl.off+stbl.size], tr)
}

func rechunkParseStbl(raw []byte, tr *rechunkTrack) (*rechunkTrack, error) {
	payload := raw[8:]
	tr.stblPayload = payload
	refs, err := rechunkScanBoxes(payload)
	if err != nil {
		return nil, err
	}

	var stsd, stts, stsc, stco, stsz *rechunkBoxRef
	for i := range refs {
		ref := &refs[i]
		switch ref.tag {
		case "co64":
			return nil, fmt.Errorf("mp4: rechunk: unsupported 64-bit chunk offsets (co64) in track %d", tr.index)
		case "stsd":
			if stsd != nil {
				return nil, fmt.Errorf("mp4: rechunk: duplicate stsd in track %d", tr.index)
			}
			stsd = ref
		case "stts":
			if stts != nil {
				return nil, fmt.Errorf("mp4: rechunk: duplicate stts in track %d", tr.index)
			}
			stts = ref
		case "stsc":
			if stsc != nil {
				return nil, fmt.Errorf("mp4: rechunk: duplicate stsc in track %d", tr.index)
			}
			stsc = ref
		case "stco":
			if stco != nil {
				return nil, fmt.Errorf("mp4: rechunk: duplicate stco in track %d", tr.index)
			}
			stco = ref
		case "stsz":
			if stsz != nil {
				return nil, fmt.Errorf("mp4: rechunk: duplicate stsz in track %d", tr.index)
			}
			stsz = ref
		}
	}
	if stsd == nil || stts == nil || stsc == nil || stco == nil || stsz == nil {
		return nil, fmt.Errorf("mp4: rechunk: track %d is missing a required sample table", tr.index)
	}

	if _, err := rechunkParseStsd(payload[stsd.off : stsd.off+stsd.size]); err != nil {
		return nil, err
	}

	sizes, err := rechunkParseStsz(payload[stsz.off : stsz.off+stsz.size])
	if err != nil {
		return nil, err
	}
	durations, err := rechunkParseStts(payload[stts.off:stts.off+stts.size], len(sizes))
	if err != nil {
		return nil, err
	}
	stscEntries, err := rechunkParseStsc(payload[stsc.off : stsc.off+stsc.size])
	if err != nil {
		return nil, err
	}
	chunkOffsets, err := rechunkParseStco(payload[stco.off : stco.off+stco.size])
	if err != nil {
		return nil, err
	}

	tr.samples = make([]rechunkSample, len(sizes))
	var dts int64
	for i := range tr.samples {
		tr.samples[i].dur = uint64(durations[i])
		tr.samples[i].dts = dts
		if uint64(durations[i]) > rechunkMaxInt64-uint64(dts) {
			return nil, fmt.Errorf("mp4: rechunk: track %d decode timestamps overflow", tr.index)
		}
		dts += int64(durations[i])
	}
	if err = rechunkExpandChunks(tr, stscEntries, chunkOffsets, sizes); err != nil {
		return nil, err
	}
	return tr, nil
}

func rechunkParseStsd(raw []byte) (string, error) {
	payload := raw[8:]
	if len(payload) < 8 {
		return "", fmt.Errorf("mp4: rechunk: truncated stsd")
	}
	entryCount := binary.BigEndian.Uint32(payload[4:8])
	if entryCount != 1 {
		return "", fmt.Errorf("mp4: rechunk: unsupported stsd entry count %d (exactly one avc1/mp4a description is required)", entryCount)
	}
	rest := payload[8:]
	if len(rest) < 8 {
		return "", fmt.Errorf("mp4: rechunk: truncated stsd entry")
	}
	size := binary.BigEndian.Uint32(rest[0:4])
	tag := string(rest[4:8])
	if size == 1 {
		return "", fmt.Errorf("mp4: rechunk: unsupported extended box length for %q", tag)
	}
	if size < 8 || int64(size) > int64(len(rest)) {
		return "", fmt.Errorf("mp4: rechunk: truncated stsd entry %q", tag)
	}
	if int(size) != len(rest) {
		return "", fmt.Errorf("mp4: rechunk: invalid stsd entry size for %q", tag)
	}
	switch tag {
	case "avc1", "mp4a":
		return tag, nil
	default:
		return "", fmt.Errorf("mp4: rechunk: unsupported sample description %q (only avc1/mp4a are supported)", tag)
	}
}

func rechunkParseStsz(raw []byte) ([]uint32, error) {
	payload := raw[8:]
	if len(payload) < 12 {
		return nil, fmt.Errorf("mp4: rechunk: truncated stsz")
	}
	sampleSize := binary.BigEndian.Uint32(payload[4:8])
	count := binary.BigEndian.Uint32(payload[8:12])
	if sampleSize != 0 {
		return nil, fmt.Errorf("mp4: rechunk: unsupported fixed sample size stsz")
	}
	if uint64(count) > uint64(len(payload)-12)/4 {
		return nil, fmt.Errorf("mp4: rechunk: truncated stsz entries")
	}
	if len(payload) != 12+4*int(count) {
		return nil, fmt.Errorf("mp4: rechunk: invalid stsz size")
	}
	sizes := make([]uint32, count)
	for i := range sizes {
		sizes[i] = binary.BigEndian.Uint32(payload[12+4*i:])
	}
	return sizes, nil
}

func rechunkParseStts(raw []byte, sampleCount int) ([]uint32, error) {
	payload := raw[8:]
	if len(payload) < 8 {
		return nil, fmt.Errorf("mp4: rechunk: truncated stts")
	}
	count := binary.BigEndian.Uint32(payload[4:8])
	if uint64(count) > uint64(len(payload)-8)/8 {
		return nil, fmt.Errorf("mp4: rechunk: truncated stts entries")
	}
	if len(payload) != 8+8*int(count) {
		return nil, fmt.Errorf("mp4: rechunk: invalid stts size")
	}
	durations := make([]uint32, 0, sampleCount)
	for i := 0; i < int(count); i++ {
		n := binary.BigEndian.Uint32(payload[8+8*i:])
		d := binary.BigEndian.Uint32(payload[12+8*i:])
		if int64(n) > int64(sampleCount)-int64(len(durations)) {
			return nil, fmt.Errorf("mp4: rechunk: stts sample count does not match stsz")
		}
		for j := uint32(0); j < n; j++ {
			durations = append(durations, d)
		}
	}
	if len(durations) != sampleCount {
		return nil, fmt.Errorf("mp4: rechunk: stts sample count %d does not match stsz sample count %d", len(durations), sampleCount)
	}
	return durations, nil
}

func rechunkParseStsc(raw []byte) ([]rechunkStscEntry, error) {
	payload := raw[8:]
	if len(payload) < 8 {
		return nil, fmt.Errorf("mp4: rechunk: truncated stsc")
	}
	count := binary.BigEndian.Uint32(payload[4:8])
	if uint64(count) > uint64(len(payload)-8)/12 {
		return nil, fmt.Errorf("mp4: rechunk: truncated stsc entries")
	}
	if len(payload) != 8+12*int(count) {
		return nil, fmt.Errorf("mp4: rechunk: invalid stsc size")
	}
	entries := make([]rechunkStscEntry, count)
	for i := range entries {
		entries[i].firstChunk = binary.BigEndian.Uint32(payload[8+12*i:])
		entries[i].samplesPerChunk = binary.BigEndian.Uint32(payload[12+12*i:])
		entries[i].descID = binary.BigEndian.Uint32(payload[16+12*i:])
	}
	if len(entries) > 0 {
		if entries[0].firstChunk != 1 {
			return nil, fmt.Errorf("mp4: rechunk: stsc must start at chunk 1")
		}
		for i := range entries {
			if entries[i].samplesPerChunk == 0 {
				return nil, fmt.Errorf("mp4: rechunk: stsc entry %d has zero samples per chunk", i)
			}
			if entries[i].descID != 1 {
				return nil, fmt.Errorf("mp4: rechunk: stsc entry %d references unsupported sample description %d", i, entries[i].descID)
			}
			if i > 0 && entries[i].firstChunk <= entries[i-1].firstChunk {
				return nil, fmt.Errorf("mp4: rechunk: stsc chunk groups are not strictly increasing")
			}
		}
	}
	return entries, nil
}

func rechunkParseStco(raw []byte) ([]uint32, error) {
	payload := raw[8:]
	if len(payload) < 8 {
		return nil, fmt.Errorf("mp4: rechunk: truncated stco")
	}
	count := binary.BigEndian.Uint32(payload[4:8])
	if uint64(count) > uint64(len(payload)-8)/4 {
		return nil, fmt.Errorf("mp4: rechunk: truncated stco entries")
	}
	if len(payload) != 8+4*int(count) {
		return nil, fmt.Errorf("mp4: rechunk: invalid stco size")
	}
	offsets := make([]uint32, count)
	for i := range offsets {
		offsets[i] = binary.BigEndian.Uint32(payload[8+4*i:])
	}
	return offsets, nil
}

func rechunkExpandChunks(tr *rechunkTrack, stsc []rechunkStscEntry, stco []uint32, sizes []uint32) error {
	chunkCount := len(stco)
	if chunkCount == 0 {
		if len(sizes) != 0 {
			return fmt.Errorf("mp4: rechunk: track %d has samples but no chunks", tr.index)
		}
		return nil
	}
	if len(stsc) == 0 {
		return fmt.Errorf("mp4: rechunk: track %d has chunks but empty stsc", tr.index)
	}
	for i := range stsc {
		if int64(stsc[i].firstChunk) > int64(chunkCount) {
			return fmt.Errorf("mp4: rechunk: track %d stsc chunk group starts past the last chunk", tr.index)
		}
	}

	sampleIndex := 0
	run := 0
	for chunkIndex := 0; chunkIndex < chunkCount; chunkIndex++ {
		for run+1 < len(stsc) && int64(stsc[run+1].firstChunk) <= int64(chunkIndex+1) {
			run++
		}
		n := int(stsc[run].samplesPerChunk)
		var offsetInChunk int64
		for k := 0; k < n; k++ {
			if sampleIndex >= len(sizes) {
				return fmt.Errorf("mp4: rechunk: track %d stsc/stco expansion covers more samples than stsz", tr.index)
			}
			tr.samples[sampleIndex].offset = int64(stco[chunkIndex]) + offsetInChunk
			tr.samples[sampleIndex].size = sizes[sampleIndex]
			offsetInChunk += int64(sizes[sampleIndex])
			sampleIndex++
		}
	}
	if sampleIndex != len(sizes) {
		return fmt.Errorf("mp4: rechunk: track %d stsc/stco expansion covers %d samples, stsz has %d", tr.index, sampleIndex, len(sizes))
	}
	return nil
}

func rechunkPlanTrack(tr *rechunkTrack, chunkDuration time.Duration) []*rechunkChunk {
	n := len(tr.samples)
	var chunks []*rechunkChunk
	for i := 0; i < n; {
		first := i
		acc := tr.samples[i].dur
		size := int64(tr.samples[i].size)
		i++
		if rechunkWithinLimit(acc, chunkDuration, tr.timescale) {
			for i < n {
				d := tr.samples[i].dur
				if acc > ^uint64(0)-d {
					break
				}
				if !rechunkWithinLimit(acc+d, chunkDuration, tr.timescale) {
					break
				}
				acc += d
				size += int64(tr.samples[i].size)
				i++
			}
		}
		chunks = append(chunks, &rechunkChunk{
			track:   tr.index,
			first:   first,
			count:   i - first,
			byteLen: size,
		})
	}
	return chunks
}

// rechunkWithinLimit reports whether ticks/timeScale <= limit, exactly, using
// integer arithmetic only.
func rechunkWithinLimit(ticks uint64, limit time.Duration, timeScale uint64) bool {
	hi1, lo1 := bits.Mul64(ticks, 1_000_000_000)
	hi2, lo2 := bits.Mul64(uint64(limit), timeScale)
	if hi1 != hi2 {
		return hi1 < hi2
	}
	return lo1 <= lo2
}

func rechunkChunkLess(a, b *rechunkChunk, tracks []*rechunkTrack) bool {
	ta := tracks[a.track]
	tb := tracks[b.track]
	da := uint64(ta.samples[a.first].dts)
	db := uint64(tb.samples[b.first].dts)
	hi1, lo1 := bits.Mul64(da, tb.timescale)
	hi2, lo2 := bits.Mul64(db, ta.timescale)
	if hi1 != hi2 {
		return hi1 < hi2
	}
	if lo1 != lo2 {
		return lo1 < lo2
	}
	return a.track < b.track
}

func rechunkBuildMoov(moovPayload []byte, tracks []*rechunkTrack) ([]byte, error) {
	newTraks := make([][]byte, len(tracks))
	for i, tr := range tracks {
		offsets := make([]uint32, len(tr.chunks))
		for j, c := range tr.chunks {
			offsets[j] = c.offset
		}
		stco, err := rechunkMarshalStco(offsets)
		if err != nil {
			return nil, err
		}
		stsc, err := rechunkMarshalStsc(tr.chunks)
		if err != nil {
			return nil, err
		}
		newStbl, err := rechunkReplaceStbl(tr.stblPayload, stco, stsc)
		if err != nil {
			return nil, err
		}
		newMinf, err := rechunkReplaceChildBox("minf", tr.minfPayload, "stbl", newStbl)
		if err != nil {
			return nil, err
		}
		newMdia, err := rechunkReplaceChildBox("mdia", tr.mdiaPayload, "minf", newMinf)
		if err != nil {
			return nil, err
		}
		newTrak, err := rechunkReplaceChildBox("trak", tr.trakPayload, "mdia", newMdia)
		if err != nil {
			return nil, err
		}
		newTraks[i] = newTrak
	}

	refs, err := rechunkScanBoxes(moovPayload)
	if err != nil {
		return nil, err
	}
	var out []byte
	k := 0
	for _, ref := range refs {
		if ref.tag == "trak" {
			if k >= len(newTraks) {
				return nil, fmt.Errorf("mp4: rechunk: internal error: unexpected trak")
			}
			out = append(out, newTraks[k]...)
			k++
		} else {
			out = append(out, moovPayload[ref.off:ref.off+ref.size]...)
		}
	}
	if k != len(newTraks) {
		return nil, fmt.Errorf("mp4: rechunk: internal error: trak count changed")
	}
	return rechunkMakeBox("moov", out)
}

func rechunkReplaceStbl(payload, stcoFull, stscFull []byte) ([]byte, error) {
	refs, err := rechunkScanBoxes(payload)
	if err != nil {
		return nil, err
	}
	var out []byte
	stcoSeen, stscSeen := false, false
	for _, ref := range refs {
		switch ref.tag {
		case "stco":
			if stcoSeen {
				return nil, fmt.Errorf("mp4: rechunk: duplicate stco")
			}
			out = append(out, stcoFull...)
			stcoSeen = true
		case "stsc":
			if stscSeen {
				return nil, fmt.Errorf("mp4: rechunk: duplicate stsc")
			}
			out = append(out, stscFull...)
			stscSeen = true
		default:
			out = append(out, payload[ref.off:ref.off+ref.size]...)
		}
	}
	if !stcoSeen || !stscSeen {
		return nil, fmt.Errorf("mp4: rechunk: internal error: stbl is missing stco/stsc")
	}
	return rechunkMakeBox("stbl", out)
}

func rechunkReplaceChildBox(containerTag string, payload []byte, childTag string, newChild []byte) ([]byte, error) {
	refs, err := rechunkScanBoxes(payload)
	if err != nil {
		return nil, err
	}
	var out []byte
	found := 0
	for _, ref := range refs {
		if ref.tag == childTag {
			out = append(out, newChild...)
			found++
		} else {
			out = append(out, payload[ref.off:ref.off+ref.size]...)
		}
	}
	if found != 1 {
		return nil, fmt.Errorf("mp4: rechunk: internal error: expected one %q in %q, found %d", childTag, containerTag, found)
	}
	return rechunkMakeBox(containerTag, out)
}

func rechunkMarshalStco(offsets []uint32) ([]byte, error) {
	if uint64(len(offsets)) > rechunkMaxUint32 {
		return nil, fmt.Errorf("mp4: rechunk: too many chunks")
	}
	payload := make([]byte, 8+4*len(offsets))
	binary.BigEndian.PutUint32(payload[4:8], uint32(len(offsets)))
	for i, off := range offsets {
		binary.BigEndian.PutUint32(payload[8+4*i:], off)
	}
	return rechunkMakeBox("stco", payload)
}

func rechunkMarshalStsc(chunks []*rechunkChunk) ([]byte, error) {
	type run struct {
		first uint32
		count uint32
	}
	var runs []run
	for i, c := range chunks {
		n := uint32(c.count)
		if len(runs) == 0 || runs[len(runs)-1].count != n {
			runs = append(runs, run{first: uint32(i + 1), count: n})
		}
	}
	if uint64(len(runs)) > rechunkMaxUint32 {
		return nil, fmt.Errorf("mp4: rechunk: too many stsc runs")
	}
	payload := make([]byte, 8+12*len(runs))
	binary.BigEndian.PutUint32(payload[4:8], uint32(len(runs)))
	for i, r := range runs {
		binary.BigEndian.PutUint32(payload[8+12*i:], r.first)
		binary.BigEndian.PutUint32(payload[12+12*i:], r.count)
		binary.BigEndian.PutUint32(payload[16+12*i:], 1)
	}
	return rechunkMakeBox("stsc", payload)
}

func rechunkCopySamples(dst io.Writer, src io.ReadSeeker, chunks []*rechunkChunk, tracks []*rechunkTrack, buf []byte) error {
	for _, c := range chunks {
		tr := tracks[c.track]
		var srcPos int64 = -1
		for k := 0; k < c.count; k++ {
			s := &tr.samples[c.first+k]
			if s.size == 0 {
				continue
			}
			if srcPos != s.offset {
				if _, err := src.Seek(s.offset, io.SeekStart); err != nil {
					return err
				}
			}
			remaining := int64(s.size)
			for remaining > 0 {
				n := int64(len(buf))
				if n > remaining {
					n = remaining
				}
				if _, err := io.ReadFull(src, buf[:n]); err != nil {
					return err
				}
				if _, err := dst.Write(buf[:n]); err != nil {
					return err
				}
				remaining -= n
			}
			srcPos = s.offset + int64(s.size)
		}
	}
	return nil
}
