package mp4

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math"
	"math/big"
	"sort"

	"github.com/abema/go-mp4/internal/avcstate"
)

// AVCEditReport is the result of ExportAVCEdits. It describes the movie and
// media timescales, every movie edit (including empty ones) in movie order,
// and every exported output sample packet.
type AVCEditReport struct {
	MovieTimescale uint32
	MediaTimescale uint32
	Edits          []AVCEditReportEdit
	Samples        []AVCEditReportSample
}

// AVCEditReportEdit is one elst entry (or the implicit whole-media edit when
// no elst box exists). MovieStart and MovieDuration are in movie timescale
// ticks.
type AVCEditReportEdit struct {
	MovieStart    uint64
	MovieDuration uint64
	Empty         bool
}

// AVCEditReportSample describes one exported packet in w. ByteOffset and
// ByteSize address the packet inside the produced Annex B stream; the first
// packet of an edit also contains the canonical parameter-set prefix in its
// byte range.
type AVCEditReportSample struct {
	EditIndex       uint32
	SourceSample    uint32 // 1-based sample number in decode order
	ByteOffset      uint64
	ByteSize        uint64
	DTS             uint64
	PTS             int64
	Visible         bool
	VisibleStartNum int64
	VisibleEndNum   int64
	VisibleDen      uint32
}

// ExportAVCEdits reads a plain, self-contained, unencrypted MP4 from r,
// applies the selected track's edit list and writes independently startable
// Annex B AVC segments (one per movie edit) to w.
//
// All layout, timing, parameter-reference and overflow validation happens
// before the first byte is written to w. The position of r is restored to
// its value at entry in every case.
func ExportAVCEdits(r io.ReadSeeker, w io.Writer, trackID uint32) (report *AVCEditReport, err error) {
	origin, seekErr := r.Seek(0, io.SeekCurrent)
	if seekErr != nil {
		return nil, seekErr
	}
	defer func() {
		if _, restoreErr := r.Seek(origin, io.SeekStart); restoreErr != nil && err == nil {
			err = restoreErr
		}
	}()

	fileEnd, err := r.Seek(0, io.SeekEnd)
	if err != nil {
		return nil, fmt.Errorf("avc edits: seek end: %w", err)
	}
	if fileEnd < 0 {
		return nil, errors.New("avc edits: invalid file size")
	}

	topBoxes, err := listBoxes(r, 0, fileEnd)
	if err != nil {
		return nil, err
	}
	var moov *rawBox
	for i := range topBoxes {
		switch topBoxes[i].typ {
		case "moov":
			if moov == nil {
				moov = &topBoxes[i]
			}
		case "moof":
			return nil, errors.New("avc edits: fragmented MP4 is not supported")
		}
	}
	if moov == nil {
		return nil, errors.New("avc edits: moov box not found")
	}

	mvhd, err := findDescendant(r, *moov, "mvhd")
	if err != nil {
		return nil, err
	}
	if mvhd == nil {
		return nil, errors.New("avc edits: mvhd box not found")
	}
	mvhdPayload, err := boxPayload(r, *mvhd)
	if err != nil {
		return nil, err
	}
	movieTS, movieDur, err := parseTimeHeader(mvhdPayload)
	if err != nil {
		return nil, fmt.Errorf("mvhd: %w", err)
	}
	if movieTS == 0 {
		return nil, errors.New("avc edits: movie timescale is zero")
	}

	trak, err := findTrackBox(r, *moov, trackID)
	if err != nil {
		return nil, err
	}
	if trak == nil {
		return nil, fmt.Errorf("avc edits: trackID %d not found", trackID)
	}

	mdhd, err := findDescendant(r, *trak, "mdia", "mdhd")
	if err != nil {
		return nil, err
	}
	if mdhd == nil {
		return nil, errors.New("avc edits: mdhd box not found")
	}
	mdhdPayload, err := boxPayload(r, *mdhd)
	if err != nil {
		return nil, err
	}
	mediaTS, mediaDur, err := parseTimeHeader(mdhdPayload)
	if err != nil {
		return nil, fmt.Errorf("mdhd: %w", err)
	}
	if mediaTS == 0 {
		return nil, errors.New("avc edits: media timescale is zero")
	}

	stbl, err := findDescendant(r, *trak, "mdia", "minf", "stbl")
	if err != nil {
		return nil, err
	}
	if stbl == nil {
		return nil, errors.New("avc edits: stbl box not found")
	}

	descs, err := parseAvcDescriptions(r, *stbl, *trak)
	if err != nil {
		return nil, err
	}

	samples, err := buildSamples(r, *stbl, descs, fileEnd)
	if err != nil {
		return nil, err
	}
	if len(samples) == 0 {
		return nil, errors.New("avc edits: track has no samples")
	}

	stssSet, stssPresent, err := parseStss(r, *stbl)
	if err != nil {
		return nil, err
	}

	rawEdits, err := parseEditTimeline(r, *trak, movieTS, movieDur, mediaTS, mediaDur)
	if err != nil {
		return nil, err
	}

	if err := validatePTSIntervals(samples); err != nil {
		return nil, err
	}

	// Full decode-order scan: parameter-set replay, IDR detection and output
	// size computation.
	if err := scanSamples(r, samples, descs); err != nil {
		return nil, err
	}

	edits, err := planEdits(rawEdits, samples, stssSet, stssPresent, movieTS, mediaTS)
	if err != nil {
		return nil, err
	}

	// Assign absolute output offsets and sizes.
	var reportSamples []AVCEditReportSample
	var outOffset uint64
	for ei := range edits {
		for si := range edits[ei].spans {
			span := &edits[ei].spans[si]
			var prefixLen uint64
			if si == 0 {
				prefixLen = uint64(len(edits[ei].prefix))
			}
			size, addErr := addUint64(prefixLen, samples[span.srcIdx].keptSize)
			if addErr != nil {
				return nil, addErr
			}
			span.offset = outOffset
			span.size = size
			next, addErr := addUint64(outOffset, size)
			if addErr != nil {
				return nil, addErr
			}
			outOffset = next

			rs := AVCEditReportSample{
				EditIndex:    uint32(ei),
				SourceSample: uint32(span.srcIdx + 1),
				ByteOffset:   span.offset,
				ByteSize:     span.size,
				DTS:          samples[span.srcIdx].dts,
				PTS:          samples[span.srcIdx].pts,
				Visible:      span.visible,
				VisibleDen:   mediaTS,
			}
			if span.visible {
				rs.VisibleStartNum = span.visStartNum
				rs.VisibleEndNum = span.visEndNum
			} else {
				rs.VisibleDen = 0
			}
			reportSamples = append(reportSamples, rs)
		}
	}
	if uint64(len(reportSamples)) != countSpans(edits) {
		return nil, errors.New("avc edits: internal sample count mismatch")
	}

	rep := &AVCEditReport{
		MovieTimescale: movieTS,
		MediaTimescale: mediaTS,
		Edits:          make([]AVCEditReportEdit, len(edits)),
		Samples:        reportSamples,
	}
	for i := range edits {
		rep.Edits[i] = edits[i].rep
	}

	// All validation and layout is complete: only now touch w.
	if err := writeSegments(r, w, edits, samples); err != nil {
		return nil, err
	}
	return rep, nil
}

/*----------------------------- MP4 box parsing -----------------------------*/

type rawBox struct {
	typ   string
	start int64 // header start
	pos   int64 // payload start
	end   int64 // exclusive box end
}

func listBoxes(r io.ReadSeeker, from, to int64) ([]rawBox, error) {
	var out []rawBox
	at := from
	for at < to {
		b, err := readBoxHeader(r, at, to)
		if err != nil {
			return nil, err
		}
		out = append(out, b)
		at = b.end
	}
	if at != to {
		return nil, fmt.Errorf("avc edits: box walk overshoots boundary at %d", at)
	}
	return out, nil
}

func readBoxHeader(r io.ReadSeeker, at, limit int64) (rawBox, error) {
	var hdr [16]byte
	if _, err := r.Seek(at, io.SeekStart); err != nil {
		return rawBox{}, err
	}
	if _, err := io.ReadFull(r, hdr[:8]); err != nil {
		return rawBox{}, fmt.Errorf("avc edits: read box header at %d: %w", at, err)
	}
	size32 := binary.BigEndian.Uint32(hdr[0:4])
	typ := string(hdr[4:8])
	headerSize := int64(8)
	size := int64(size32)
	if size32 == 1 {
		if _, err := io.ReadFull(r, hdr[8:16]); err != nil {
			return rawBox{}, fmt.Errorf("avc edits: read largesize of %q: %w", typ, err)
		}
		u64 := binary.BigEndian.Uint64(hdr[8:16])
		if u64 > math.MaxInt64 {
			return rawBox{}, fmt.Errorf("avc edits: %q size overflow", typ)
		}
		size = int64(u64)
		headerSize = 16
	} else if size32 == 0 {
		size = limit - at
	}
	if size < headerSize {
		return rawBox{}, fmt.Errorf("avc edits: %q box size %d shorter than header", typ, size)
	}
	end := at + size
	if end > limit || end < at {
		return rawBox{}, fmt.Errorf("avc edits: %q box at %d extends past parent/file end %d", typ, at, limit)
	}
	return rawBox{typ: typ, start: at, pos: at + headerSize, end: end}, nil
}

func boxPayload(r io.ReadSeeker, b rawBox) ([]byte, error) {
	n := b.end - b.pos
	if n < 0 || n > math.MaxInt32 {
		return nil, fmt.Errorf("avc edits: %q payload too large", b.typ)
	}
	buf := make([]byte, n)
	if _, err := r.Seek(b.pos, io.SeekStart); err != nil {
		return nil, err
	}
	if _, err := io.ReadFull(r, buf); err != nil {
		return nil, fmt.Errorf("avc edits: read %q payload: %w", b.typ, err)
	}
	return buf, nil
}

func childBoxes(r io.ReadSeeker, b rawBox) ([]rawBox, error) {
	return listBoxes(r, b.pos, b.end)
}

func findChild(r io.ReadSeeker, b rawBox, typ string) (*rawBox, error) {
	children, err := childBoxes(r, b)
	if err != nil {
		return nil, err
	}
	for i := range children {
		if children[i].typ == typ {
			return &children[i], nil
		}
	}
	return nil, nil
}

func findDescendant(r io.ReadSeeker, b rawBox, path ...string) (*rawBox, error) {
	cur := b
	for _, typ := range path {
		next, err := findChild(r, cur, typ)
		if err != nil {
			return nil, err
		}
		if next == nil {
			return nil, nil
		}
		cur = *next
	}
	return &cur, nil
}

func findTrackBox(r io.ReadSeeker, moov rawBox, trackID uint32) (*rawBox, error) {
	trakBoxes, err := childBoxes(r, moov)
	if err != nil {
		return nil, err
	}
	for i := range trakBoxes {
		if trakBoxes[i].typ != "trak" {
			continue
		}
		tkhd, err := findChild(r, trakBoxes[i], "tkhd")
		if err != nil {
			return nil, err
		}
		if tkhd == nil {
			continue
		}
		p, err := boxPayload(r, *tkhd)
		if err != nil {
			return nil, err
		}
		id, err := parseTKHDTrackID(p)
		if err != nil {
			return nil, err
		}
		if id == trackID {
			t := trakBoxes[i]
			return &t, nil
		}
	}
	return nil, nil
}

/*------------------------------ FullBox helpers ----------------------------*/

func parseTimeHeader(p []byte) (timescale uint32, duration uint64, err error) {
	if len(p) < 4 {
		return 0, 0, io.ErrUnexpectedEOF
	}
	version := p[0]
	switch version {
	case 0:
		if len(p) < 20 {
			return 0, 0, io.ErrUnexpectedEOF
		}
		timescale = binary.BigEndian.Uint32(p[12:16])
		duration = uint64(binary.BigEndian.Uint32(p[16:20]))
	case 1:
		if len(p) < 32 {
			return 0, 0, io.ErrUnexpectedEOF
		}
		timescale = binary.BigEndian.Uint32(p[20:24])
		duration = binary.BigEndian.Uint64(p[24:32])
	default:
		return 0, 0, fmt.Errorf("unsupported version %d", version)
	}
	return timescale, duration, nil
}

func parseTKHDTrackID(p []byte) (uint32, error) {
	if len(p) < 4 {
		return 0, io.ErrUnexpectedEOF
	}
	switch p[0] {
	case 0:
		if len(p) < 16 {
			return 0, io.ErrUnexpectedEOF
		}
		return binary.BigEndian.Uint32(p[12:16]), nil
	case 1:
		if len(p) < 24 {
			return 0, io.ErrUnexpectedEOF
		}
		return binary.BigEndian.Uint32(p[20:24]), nil
	default:
		return 0, fmt.Errorf("tkhd: unsupported version %d", p[0])
	}
}

/*------------------------- stsd / AVC descriptions ------------------------*/

type avcDesc struct {
	typ        string // avc1 or avc3
	avcC       []byte
	lengthSize int
}

func parseAvcDescriptions(r io.ReadSeeker, stbl rawBox, trak rawBox) ([]*avcDesc, error) {
	stsd, err := findChild(r, stbl, "stsd")
	if err != nil {
		return nil, err
	}
	if stsd == nil {
		return nil, errors.New("avc edits: stsd box not found")
	}
	p, err := boxPayload(r, *stsd)
	if err != nil {
		return nil, err
	}
	if len(p) < 8 {
		return nil, errors.New("avc edits: stsd too short")
	}
	entryCount := binary.BigEndian.Uint32(p[4:8])
	pos := 8
	descs := make([]*avcDesc, 0, entryCount)

	// dref validation: every referenced sample entry must be self-contained.
	selfContained, drefErr := drefSelfContained(r, trak)
	if drefErr != nil {
		return nil, drefErr
	}

	for i := uint32(0); i < entryCount; i++ {
		if pos+8 > len(p) {
			return nil, errors.New("avc edits: truncated stsd entry header")
		}
		size := int64(binary.BigEndian.Uint32(p[pos : pos+4]))
		typ := string(p[pos+4 : pos+8])
		if size < 8 || pos+int(size) > len(p) {
			return nil, fmt.Errorf("avc edits: truncated stsd entry %d", i)
		}
		entryStart := stsd.pos + int64(pos)
		entryPayload := entryStart + 8

		// SampleEntry.data_reference_index at payload offset 6.
		drefIndex := binary.BigEndian.Uint16(p[pos+14 : pos+16])
		if selfContained != nil {
			ok, err := selfContained(uint32(drefIndex))
			if err != nil {
				return nil, err
			}
			if !ok {
				return nil, fmt.Errorf("avc edits: sample description %d uses external data reference %d", i+1, drefIndex)
			}
		}

		switch typ {
		case "avc1", "avc3":
		case "encv", "enca":
			return nil, errors.New("avc edits: encrypted sample entry is not supported")
		default:
			return nil, fmt.Errorf("avc edits: sample description %d has unsupported type %q", i+1, typ)
		}

		// VisualSampleEntry fixed fields occupy 78 payload bytes; child boxes
		// (avcC, pasp, ...) follow.
		childFrom := entryPayload + 78
		entryEnd := entryStart + size
		var avcC *rawBox
		at := childFrom
		for at < entryEnd {
			cb, err := readBoxHeader(r, at, entryEnd)
			if err != nil {
				return nil, err
			}
			if cb.typ == "avcC" {
				avcC = &cb
			}
			if cb.typ == "sinf" {
				return nil, errors.New("avc edits: sample entry contains sinf (encrypted)")
			}
			at = cb.end
		}
		if at != entryEnd {
			return nil, errors.New("avc edits: bad sample entry child layout")
		}
		if avcC == nil {
			// An unused description may omit avcC; a description actually
			// selected by stsc is rejected later during the decode scan.
			descs = append(descs, &avcDesc{typ: typ})
			continue
		}
		cfg, err := boxPayload(r, *avcC)
		if err != nil {
			return nil, err
		}
		_, _, lengthSize, err := avcstate.ParseAVCConfig(cfg)
		if err != nil {
			return nil, fmt.Errorf("description %d avcC: %w", i+1, err)
		}
		descs = append(descs, &avcDesc{typ: typ, avcC: cfg, lengthSize: lengthSize})
		pos += int(size)
	}
	if len(descs) == 0 {
		return nil, errors.New("avc edits: stsd has no AVC descriptions")
	}
	return descs, nil
}

// drefSelfContained returns a function reporting whether a 1-based data
// reference index points at a self-contained entry. A nil function means no
// dref box was present and no restriction can be enforced.
func drefSelfContained(r io.ReadSeeker, trak rawBox) (func(uint32) (bool, error), error) {
	dref, err := findDescendant(r, trak, "mdia", "minf", "dinf", "dref")
	if err != nil {
		return nil, err
	}
	if dref == nil {
		return nil, nil
	}
	p, err := boxPayload(r, *dref)
	if err != nil {
		return nil, err
	}
	if len(p) < 8 {
		return nil, errors.New("avc edits: dref too short")
	}
	count := binary.BigEndian.Uint32(p[4:8])
	entries := map[uint32]bool{}
	pos := 8
	for i := uint32(0); i < count; i++ {
		if pos+12 > len(p) {
			return nil, errors.New("avc edits: truncated dref entry")
		}
		size := int(binary.BigEndian.Uint32(p[pos : pos+4]))
		if size < 12 || pos+size > len(p) {
			return nil, errors.New("avc edits: bad dref entry size")
		}
		entryFlags := uint32(p[pos+9])<<16 | uint32(p[pos+10])<<8 | uint32(p[pos+11])
		entries[i+1] = entryFlags&0x000001 != 0
		pos += size
	}
	return func(index uint32) (bool, error) {
		if index == 0 {
			// Index 0 is reserved and never references external data.
			return true, nil
		}
		sc, ok := entries[index]
		if !ok {
			return false, fmt.Errorf("avc edits: data reference index %d out of range", index)
		}
		return sc, nil
	}, nil
}

/*------------------------------ sample tables -----------------------------*/

type sampleInfo struct {
	size     uint32
	offset   uint64
	dts      uint64
	pts      int64
	duration uint32
	desc     int // 0-based stsd index

	isIDR      bool
	keptSize   uint64 // Annex B bytes emitted for the sample (4-byte starts, no 7/8)
	lengthSize int
	snapshot   *avcstate.Table
}

func buildSamples(r io.ReadSeeker, stbl rawBox, descs []*avcDesc, fileEnd int64) ([]*sampleInfo, error) {
	stszBox, err := findChild(r, stbl, "stsz")
	if err != nil {
		return nil, err
	}
	if stszBox == nil {
		return nil, errors.New("avc edits: stsz box not found")
	}
	stsz, err := boxPayload(r, *stszBox)
	if err != nil {
		return nil, err
	}
	if len(stsz) < 12 {
		return nil, errors.New("avc edits: stsz too short")
	}
	uniformSize := binary.BigEndian.Uint32(stsz[4:8])
	sampleCount := binary.BigEndian.Uint32(stsz[8:12])
	sizes := make([]uint32, sampleCount)
	if uniformSize != 0 {
		for i := range sizes {
			sizes[i] = uniformSize
		}
	} else {
		if len(stsz) < 12+4*int(sampleCount) {
			return nil, errors.New("avc edits: stsz entry table truncated")
		}
		for i := range sizes {
			sizes[i] = binary.BigEndian.Uint32(stsz[12+4*i : 16+4*i])
		}
	}

	// stts: per-sample deltas.
	sttsBox, err := findChild(r, stbl, "stts")
	if err != nil {
		return nil, err
	}
	if sttsBox == nil {
		return nil, errors.New("avc edits: stts box not found")
	}
	stts, err := boxPayload(r, *sttsBox)
	if err != nil {
		return nil, err
	}
	deltas, err := expandRunTable(stts, sampleCount, "stts")
	if err != nil {
		return nil, err
	}

	// ctts: per-sample signed composition offsets (optional).
	cttsOffsets := make([]int64, sampleCount)
	cttsBox, err := findChild(r, stbl, "ctts")
	if err != nil {
		return nil, err
	}
	if cttsBox != nil {
		ctts, err := boxPayload(r, *cttsBox)
		if err != nil {
			return nil, err
		}
		if err := expandCTTS(ctts, cttsOffsets); err != nil {
			return nil, err
		}
	}

	// stco / co64 chunk offsets.
	chunkOffsets, err := parseChunkOffsets(r, stbl)
	if err != nil {
		return nil, err
	}
	if len(chunkOffsets) == 0 {
		return nil, errors.New("avc edits: no chunks (stco/co64 empty)")
	}

	// stsc: samples-per-chunk and sample-description-index per chunk.
	stscBox, err := findChild(r, stbl, "stsc")
	if err != nil {
		return nil, err
	}
	if stscBox == nil {
		return nil, errors.New("avc edits: stsc box not found")
	}
	stsc, err := boxPayload(r, *stscBox)
	if err != nil {
		return nil, err
	}
	chunkSPC, chunkDesc, err := expandSTSC(stsc, uint32(len(chunkOffsets)), uint32(len(descs)))
	if err != nil {
		return nil, err
	}

	samples := make([]*sampleInfo, sampleCount)
	var sampleIdx uint32
	for ci, chunkOff := range chunkOffsets {
		cursor := chunkOff
		for k := uint32(0); k < chunkSPC[ci]; k++ {
			if sampleIdx >= sampleCount {
				return nil, errors.New("avc edits: stsc describes more samples than stsz")
			}
			size := sizes[sampleIdx]
			if size == 0 {
				return nil, fmt.Errorf("avc edits: sample %d has zero size", sampleIdx+1)
			}
			end, addErr := addUint64(cursor, uint64(size))
			if addErr != nil {
				return nil, addErr
			}
			if end > uint64(fileEnd) {
				return nil, fmt.Errorf("avc edits: sample %d is truncated by file end", sampleIdx+1)
			}
			samples[sampleIdx] = &sampleInfo{
				size:     size,
				offset:   cursor,
				duration: deltas[sampleIdx],
				desc:     int(chunkDesc[ci]) - 1,
			}
			cursor = end
			sampleIdx++
		}
	}
	if sampleIdx != sampleCount {
		return nil, fmt.Errorf("avc edits: stsc covers %d samples but stsz declares %d", sampleIdx, sampleCount)
	}

	// DTS / PTS.
	var dts uint64
	for i := uint32(0); i < sampleCount; i++ {
		s := samples[i]
		s.dts = dts
		pts, err := addInt64Uint64(cttsOffsets[i], dts)
		if err != nil {
			return nil, err
		}
		s.pts = pts
		next, err := addUint64(dts, uint64(s.duration))
		if err != nil {
			return nil, err
		}
		dts = next
	}
	return samples, nil
}

func expandRunTable(p []byte, want uint32, name string) ([]uint32, error) {
	if len(p) < 8 {
		return nil, fmt.Errorf("avc edits: %s too short", name)
	}
	count := binary.BigEndian.Uint32(p[4:8])
	out := make([]uint32, 0, want)
	pos := 8
	for i := uint32(0); i < count; i++ {
		if pos+8 > len(p) {
			return nil, fmt.Errorf("avc edits: %s entry truncated", name)
		}
		runCount := binary.BigEndian.Uint32(p[pos : pos+4])
		value := binary.BigEndian.Uint32(p[pos+4 : pos+8])
		pos += 8
		for j := uint32(0); j < runCount; j++ {
			out = append(out, value)
		}
	}
	if uint32(len(out)) != want {
		return nil, fmt.Errorf("avc edits: %s covers %d samples, expected %d", name, len(out), want)
	}
	return out, nil
}

func expandCTTS(p []byte, out []int64) error {
	if len(p) < 8 {
		return errors.New("avc edits: ctts too short")
	}
	version := p[0]
	count := binary.BigEndian.Uint32(p[4:8])
	pos := 8
	si := 0
	for i := uint32(0); i < count; i++ {
		if pos+8 > len(p) {
			return errors.New("avc edits: ctts entry truncated")
		}
		runCount := binary.BigEndian.Uint32(p[pos : pos+4])
		var offset int64
		if version == 1 {
			offset = int64(int32(binary.BigEndian.Uint32(p[pos+4 : pos+8])))
		} else {
			offset = int64(binary.BigEndian.Uint32(p[pos+4 : pos+8]))
		}
		pos += 8
		for j := uint32(0); j < runCount; j++ {
			if si >= len(out) {
				return errors.New("avc edits: ctts covers more samples than stsz")
			}
			out[si] = offset
			si++
		}
	}
	if si != len(out) {
		return fmt.Errorf("avc edits: ctts covers %d samples, expected %d", si, len(out))
	}
	return nil
}

func parseChunkOffsets(r io.ReadSeeker, stbl rawBox) ([]uint64, error) {
	if stco, err := findChild(r, stbl, "stco"); err != nil {
		return nil, err
	} else if stco != nil {
		p, err := boxPayload(r, *stco)
		if err != nil {
			return nil, err
		}
		if len(p) < 8 {
			return nil, errors.New("avc edits: stco too short")
		}
		count := binary.BigEndian.Uint32(p[4:8])
		if len(p) < 8+4*int(count) {
			return nil, errors.New("avc edits: stco entries truncated")
		}
		out := make([]uint64, count)
		for i := range out {
			out[i] = uint64(binary.BigEndian.Uint32(p[8+4*i : 12+4*i]))
		}
		return out, nil
	}
	co64, err := findChild(r, stbl, "co64")
	if err != nil {
		return nil, err
	}
	if co64 == nil {
		return nil, errors.New("avc edits: neither stco nor co64 found")
	}
	p, err := boxPayload(r, *co64)
	if err != nil {
		return nil, err
	}
	if len(p) < 8 {
		return nil, errors.New("avc edits: co64 too short")
	}
	count := binary.BigEndian.Uint32(p[4:8])
	if len(p) < 8+8*int(count) {
		return nil, errors.New("avc edits: co64 entries truncated")
	}
	out := make([]uint64, count)
	for i := range out {
		out[i] = binary.BigEndian.Uint64(p[8+8*i : 16+8*i])
	}
	return out, nil
}

func expandSTSC(p []byte, chunkCount uint32, descCount uint32) (spc []uint32, desc []uint32, err error) {
	if len(p) < 8 {
		return nil, nil, errors.New("avc edits: stsc too short")
	}
	entryCount := binary.BigEndian.Uint32(p[4:8])
	type scEntry struct {
		firstChunk uint32
		spc        uint32
		descIndex  uint32
	}
	entries := make([]scEntry, 0, entryCount)
	pos := 8
	for i := uint32(0); i < entryCount; i++ {
		if pos+12 > len(p) {
			return nil, nil, errors.New("avc edits: stsc entry truncated")
		}
		e := scEntry{
			firstChunk: binary.BigEndian.Uint32(p[pos : pos+4]),
			spc:        binary.BigEndian.Uint32(p[pos+4 : pos+8]),
			descIndex:  binary.BigEndian.Uint32(p[pos+8 : pos+12]),
		}
		pos += 12
		if e.firstChunk == 0 || e.spc == 0 || e.descIndex == 0 || e.descIndex > descCount {
			return nil, nil, fmt.Errorf("avc edits: invalid stsc entry %+v", e)
		}
		entries = append(entries, e)
	}
	if len(entries) == 0 {
		return nil, nil, errors.New("avc edits: stsc has no entries")
	}
	spc = make([]uint32, chunkCount)
	desc = make([]uint32, chunkCount)
	for ci := uint32(1); ci <= chunkCount; ci++ {
		chosen := entries[0]
		for _, e := range entries {
			if e.firstChunk <= ci {
				chosen = e
			} else {
				break
			}
		}
		spc[ci-1] = chosen.spc
		desc[ci-1] = chosen.descIndex
	}
	return spc, desc, nil
}

func parseStss(r io.ReadSeeker, stbl rawBox) (map[uint32]bool, bool, error) {
	stss, err := findChild(r, stbl, "stss")
	if err != nil {
		return nil, false, err
	}
	if stss == nil {
		return nil, false, nil
	}
	p, err := boxPayload(r, *stss)
	if err != nil {
		return nil, false, err
	}
	if len(p) < 8 {
		return nil, true, errors.New("avc edits: stss too short")
	}
	count := binary.BigEndian.Uint32(p[4:8])
	if len(p) < 8+4*int(count) {
		return nil, true, errors.New("avc edits: stss entries truncated")
	}
	set := make(map[uint32]bool, count)
	for i := uint32(0); i < count; i++ {
		n := binary.BigEndian.Uint32(p[8+4*i : 12+4*i])
		set[n] = true
	}
	return set, true, nil
}

/*-------------------------------- edit list --------------------------------*/

type rawEdit struct {
	rep       AVCEditReportEdit
	mediaTime int64
	// endNum/endDen describe the exclusive media end in media timescale.
	endNum *big.Int
	endDen *big.Int
	// spanMovieScaled = (end-mediaTime)*movieTimescale, always integral.
	spanMovieScaled *big.Int
}

func parseEditTimeline(r io.ReadSeeker, trak rawBox, movieTS uint32, movieDur uint64, mediaTS uint32, mediaDur uint64) ([]rawEdit, error) {
	elst, err := findDescendant(r, trak, "edts", "elst")
	if err != nil {
		return nil, err
	}
	if elst == nil {
		// Implicit edit: media zero to mdhd.duration, movie start zero.
		md := new(big.Int).SetUint64(mediaDur)
		mv := big.NewInt(int64(movieTS))
		return []rawEdit{{
			rep: AVCEditReportEdit{
				MovieStart:    0,
				MovieDuration: movieDur,
				Empty:         false,
			},
			mediaTime:       0,
			endNum:          new(big.Int).Set(md),
			endDen:          big.NewInt(1),
			spanMovieScaled: new(big.Int).Mul(md, mv),
		}}, nil
	}
	p, err := boxPayload(r, *elst)
	if err != nil {
		return nil, err
	}
	if len(p) < 8 {
		return nil, errors.New("avc edits: elst too short")
	}
	version := p[0]
	count := binary.BigEndian.Uint32(p[4:8])
	pos := 8
	edits := make([]rawEdit, 0, count)
	var movieStart uint64
	for i := uint32(0); i < count; i++ {
		var segDur uint64
		var mediaTime int64
		var rateInt int16
		var rateFrac int16
		switch version {
		case 0:
			if pos+12 > len(p) {
				return nil, errors.New("avc edits: elst v0 entry truncated")
			}
			segDur = uint64(binary.BigEndian.Uint32(p[pos : pos+4]))
			mediaTime = int64(int32(binary.BigEndian.Uint32(p[pos+4 : pos+8])))
			rateInt = int16(binary.BigEndian.Uint16(p[pos+8 : pos+10]))
			rateFrac = int16(binary.BigEndian.Uint16(p[pos+10 : pos+12]))
			pos += 12
		case 1:
			if pos+20 > len(p) {
				return nil, errors.New("avc edits: elst v1 entry truncated")
			}
			segDur = binary.BigEndian.Uint64(p[pos : pos+8])
			mediaTime = int64(binary.BigEndian.Uint64(p[pos+8 : pos+16]))
			rateInt = int16(binary.BigEndian.Uint16(p[pos+16 : pos+18]))
			rateFrac = int16(binary.BigEndian.Uint16(p[pos+18 : pos+20]))
			pos += 20
		default:
			return nil, fmt.Errorf("avc edits: unsupported elst version %d", version)
		}
		empty := mediaTime == -1
		if mediaTime < -1 {
			return nil, fmt.Errorf("avc edits: invalid media_time %d", mediaTime)
		}
		// The unit-rate requirement applies to media segments; empty edits
		// carry no playback rate semantics.
		if !empty && (rateInt != 1 || rateFrac != 0) {
			return nil, fmt.Errorf("avc edits: unsupported edit rate %d.%d (only 1.0)", rateInt, rateFrac)
		}
		re := rawEdit{
			rep: AVCEditReportEdit{
				MovieStart:    movieStart,
				MovieDuration: segDur,
				Empty:         empty,
			},
			mediaTime: mediaTime,
		}
		if !empty {
			// Exclusive media end:
			//   end = mediaTime + segDur*mediaTS/movieTS
			// represented as endNum/endDen with endDen = movieTS.
			mts := big.NewInt(int64(movieTS))
			mdTS := big.NewInt(int64(mediaTS))
			re.endDen = mts
			re.endNum = new(big.Int).Mul(big.NewInt(mediaTime), mts)
			re.endNum.Add(re.endNum, new(big.Int).Mul(new(big.Int).SetUint64(segDur), mdTS))
			// (end-mediaTime)*movieTS = segDur*mediaTS, always integral.
			re.spanMovieScaled = new(big.Int).Mul(new(big.Int).SetUint64(segDur), mdTS)
		}
		edits = append(edits, re)
		var addErr error
		movieStart, addErr = addUint64(movieStart, segDur)
		if addErr != nil {
			return nil, addErr
		}
	}
	return edits, nil
}

/*------------------------------ timing checks -----------------------------*/

// ptsEnd returns the exclusive PTS interval end with overflow validation.
func ptsEnd(s *sampleInfo) (int64, error) {
	d := int64(s.duration)
	if s.duration > math.MaxInt32 {
		// deltas are uint32; int64 conversion is still safe, but keep the
		// arithmetic explicitly checked.
		d = int64(s.duration)
	}
	end := s.pts + d
	if (d > 0 && end < s.pts) || (d < 0 && end > s.pts) {
		return 0, fmt.Errorf("avc edits: PTS interval overflow at sample")
	}
	return end, nil
}

func validatePTSIntervals(samples []*sampleInfo) error {
	type iv struct {
		idx int
		a   int64
		b   int64
	}
	ivs := make([]iv, 0, len(samples))
	for i, s := range samples {
		b, err := ptsEnd(s)
		if err != nil {
			return err
		}
		ivs = append(ivs, iv{idx: i, a: s.pts, b: b})
	}
	sort.SliceStable(ivs, func(i, j int) bool {
		if ivs[i].a != ivs[j].a {
			return ivs[i].a < ivs[j].a
		}
		return ivs[i].b < ivs[j].b
	})
	for i := 1; i < len(ivs); i++ {
		p, c := ivs[i-1], ivs[i]
		// Intervals [a,b) overlap iff max(a) < min(b). Starts are ordered.
		if c.a < p.b && p.a < p.b && c.a < c.b {
			return fmt.Errorf("avc edits: PTS intervals of samples %d and %d overlap", p.idx+1, c.idx+1)
		}
	}
	return nil
}

/*----------------------------- decode scanning ----------------------------*/

func readSampleBytes(r io.ReadSeeker, s *sampleInfo) ([]byte, error) {
	buf := make([]byte, s.size)
	if _, err := r.Seek(int64(s.offset), io.SeekStart); err != nil {
		return nil, err
	}
	if _, err := io.ReadFull(r, buf); err != nil {
		return nil, fmt.Errorf("avc edits: read sample at offset %d (%d bytes): %w", s.offset, s.size, err)
	}
	return buf, nil
}

func scanSamples(r io.ReadSeeker, samples []*sampleInfo, descs []*avcDesc) error {
	var table *avcstate.Table
	prevDesc := -1
	for i, s := range samples {
		if s.desc < 0 || s.desc >= len(descs) {
			return fmt.Errorf("avc edits: sample %d references bad description index %d", i+1, s.desc+1)
		}
		if i == 0 || s.desc != prevDesc {
			if len(descs[s.desc].avcC) == 0 {
				return fmt.Errorf("avc edits: sample %d uses description %d which has no avcC", i+1, s.desc+1)
			}
			table = avcstate.NewTable()
			ls, err := table.ResetAVCConfig(descs[s.desc].avcC)
			if err != nil {
				return fmt.Errorf("avc edits: description %d: %w", s.desc+1, err)
			}
			descs[s.desc].lengthSize = ls
		}
		prevDesc = s.desc
		lengthSize := descs[s.desc].lengthSize
		s.lengthSize = lengthSize

		buf, err := readSampleBytes(r, s)
		if err != nil {
			return err
		}

		pos := 0
		vclSeen := false
		for pos < len(buf) {
			if pos+lengthSize > len(buf) {
				return fmt.Errorf("avc edits: sample %d has truncated NAL length", i+1)
			}
			var n uint64
			for k := 0; k < lengthSize; k++ {
				n = n<<8 | uint64(buf[pos+k])
			}
			if n == 0 {
				return fmt.Errorf("avc edits: sample %d contains zero-length NAL", i+1)
			}
			header := pos + lengthSize
			if n > uint64(len(buf)-header) {
				return fmt.Errorf("avc edits: sample %d NAL length %d exceeds sample bytes", i+1, n)
			}
			nal := buf[header : header+int(n)]

			// Every NAL must have legal emulation prevention.
			if _, err := avcstate.RemoveEmulationPrevention(nal); err != nil {
				return fmt.Errorf("avc edits: sample %d: %w", i+1, err)
			}
			nalType := nal[0] & 0x1f

			switch nalType {
			case 7:
				if vclSeen {
					return fmt.Errorf("avc edits: SPS after first VCL NAL in sample %d", i+1)
				}
				if _, err := table.PutSPS(nal); err != nil {
					return fmt.Errorf("avc edits: sample %d SPS: %w", i+1, err)
				}
			case 8:
				if vclSeen {
					return fmt.Errorf("avc edits: PPS after first VCL NAL in sample %d", i+1)
				}
				if _, _, err := table.PutPPS(nal); err != nil {
					return fmt.Errorf("avc edits: sample %d PPS: %w", i+1, err)
				}
			case 1, 5:
				if _, err := table.CheckSliceHeader(nal); err != nil {
					return fmt.Errorf("avc edits: sample %d: %w", i+1, err)
				}
				vclSeen = true
				if nalType == 5 {
					s.isIDR = true
				}
				s.keptSize += 4 + n
			case 2, 3, 4:
				return fmt.Errorf("avc edits: sample %d contains unsupported data-partition NAL type %d", i+1, nalType)
			case 20:
				return fmt.Errorf("avc edits: sample %d contains unsupported extension VCL NAL type 20", i+1)
			default:
				// SEI, AUD, end-of-stream, filler and other ordinary NALs are
				// preserved in original order.
				s.keptSize += 4 + n
			}
			pos = header + int(n)
		}
		if pos != len(buf) {
			return fmt.Errorf("avc edits: sample %d has %d trailing bytes", i+1, len(buf)-pos)
		}
		if !vclSeen {
			return fmt.Errorf("avc edits: sample %d contains no VCL NAL (one picture per sample required)", i+1)
		}
		if s.isIDR {
			s.snapshot = table.Clone()
		}
	}
	return nil
}

/*--------------------------- edit scheduling ------------------------------*/

type outSpan struct {
	srcIdx int
	offset uint64
	size   uint64

	visible     bool
	visStartNum int64
	visEndNum   int64
}

type plannedEdit struct {
	rep    AVCEditReportEdit
	prefix []byte
	spans  []outSpan
}

func countSpans(edits []plannedEdit) uint64 {
	var n uint64
	for i := range edits {
		n += uint64(len(edits[i].spans))
	}
	return n
}

// cmpIntRat compares integer x with rational num/den (den > 0):
// returns -1 if x < num/den, 0 if equal, +1 if x > num/den.
func cmpIntRat(x int64, num *big.Int, den *big.Int) int {
	lhs := new(big.Int).Mul(big.NewInt(x), den)
	return lhs.Cmp(num)
}

func planEdits(raw []rawEdit, samples []*sampleInfo, stss map[uint32]bool, stssPresent bool, movieTS, mediaTS uint32) ([]plannedEdit, error) {
	planned := make([]plannedEdit, 0, len(raw))
	for ei, e := range raw {
		pe := plannedEdit{rep: e.rep}
		if e.rep.Empty {
			planned = append(planned, pe)
			continue
		}
		nonPositive := new(big.Int).Mul(big.NewInt(e.mediaTime), e.endDen).Cmp(e.endNum) >= 0
		if nonPositive {
			planned = append(planned, pe)
			continue
		}

		mediaTime := e.mediaTime
		visible := make(map[int]bool)
		firstVis := -1
		lastVis := -1

		// PTS order sweep for complete coverage.
		order := make([]int, len(samples))
		for i := range order {
			order[i] = i
		}
		sort.SliceStable(order, func(i, j int) bool { return samples[order[i]].pts < samples[order[j]].pts })

		covered := big.NewInt(mediaTime)
		for _, idx := range order {
			s := samples[idx]
			bEnd, err := ptsEnd(s)
			if err != nil {
				return nil, err
			}
			// Intersects [mediaTime, endNum/endDen) iff b > mediaTime and
			// pts < end.
			if bEnd <= mediaTime {
				continue
			}
			if cmpIntRat(s.pts, e.endNum, e.endDen) >= 0 {
				continue
			}
			visible[idx] = true
			if firstVis < 0 || idx < firstVis {
				firstVis = idx
			}
			if idx > lastVis {
				lastVis = idx
			}

			// Coverage sweep in PTS order: no gap between the covered
			// frontier and this interval's start.
			if cmpIntRat(s.pts, covered, big.NewInt(1)) > 0 {
				return nil, fmt.Errorf("avc edits: edit %d has a media gap before sample %d", ei, idx+1)
			}
			if bEnd > covered.Int64() {
				covered = big.NewInt(bEnd)
			}
		}
		if firstVis < 0 {
			return nil, fmt.Errorf("avc edits: edit %d has no visible samples", ei)
		}
		// Exact coverage check: covered >= endNum/endDen.
		if new(big.Int).Mul(covered, e.endDen).Cmp(e.endNum) < 0 {
			return nil, fmt.Errorf("avc edits: edit %d media interval is not fully covered by sample PTS intervals", ei)
		}

		// Nearest actual IDR at or before the earliest visible decode index.
		idr := -1
		for j := firstVis; j >= 0; j-- {
			if samples[j].isIDR {
				idr = j
				break
			}
		}
		if idr < 0 {
			return nil, fmt.Errorf("avc edits: edit %d has no IDR preceding visible samples", ei)
		}
		if stssPresent && !stss[uint32(idr+1)] {
			return nil, fmt.Errorf("avc edits: edit %d IDR sample %d is not listed in stss", ei, idr+1)
		}
		if samples[idr].snapshot == nil {
			return nil, fmt.Errorf("avc edits: internal error: IDR %d has no parameter snapshot", idr+1)
		}
		prefix, err := samples[idr].snapshot.PrefixBytes()
		if err != nil {
			return nil, fmt.Errorf("avc edits: edit %d parameter sets: %w", ei, err)
		}
		pe.prefix = prefix

		mts := int64(movieTS)
		mdts := int64(mediaTS)
		movieStart := new(big.Int).SetUint64(e.rep.MovieStart)

		for idx := idr; idx <= lastVis; idx++ {
			span := outSpan{srcIdx: idx, visible: visible[idx]}
			if visible[idx] {
				s := samples[idx]
				bEnd, err := ptsEnd(s)
				if err != nil {
					return nil, err
				}
				cropLo := s.pts
				if cropLo < mediaTime {
					cropLo = mediaTime
				}
				startNum := new(big.Int).Mul(movieStart, big.NewInt(mdts))
				startNum.Add(startNum, new(big.Int).Mul(big.NewInt(cropLo-mediaTime), big.NewInt(mts)))
				if !startNum.IsInt64() {
					return nil, fmt.Errorf("avc edits: edit %d visible start numerator overflow", ei)
				}
				span.visStartNum = startNum.Int64()

				var relEndScaled *big.Int
				if cmpIntRat(bEnd, e.endNum, e.endDen) <= 0 {
					relEndScaled = new(big.Int).Mul(big.NewInt(bEnd-mediaTime), big.NewInt(mts))
				} else {
					relEndScaled = new(big.Int).Set(e.spanMovieScaled)
				}
				endNumV := new(big.Int).Mul(movieStart, big.NewInt(mdts))
				endNumV.Add(endNumV, relEndScaled)
				if !endNumV.IsInt64() {
					return nil, fmt.Errorf("avc edits: edit %d visible end numerator overflow", ei)
				}
				span.visEndNum = endNumV.Int64()
			}
			pe.spans = append(pe.spans, span)
		}
		planned = append(planned, pe)
	}
	return planned, nil
}

/*--------------------------------- output ----------------------------------*/

func writeAll(w io.Writer, p []byte) error {
	for len(p) > 0 {
		n, err := w.Write(p)
		if err != nil {
			return err
		}
		if n <= 0 {
			return io.ErrShortWrite
		}
		if n > len(p) {
			return io.ErrShortWrite
		}
		p = p[n:]
	}
	return nil
}

var startCode = []byte{0, 0, 0, 1}

func writeSegments(r io.ReadSeeker, w io.Writer, edits []plannedEdit, samples []*sampleInfo) error {
	for ei := range edits {
		e := &edits[ei]
		for si := range e.spans {
			span := &e.spans[si]
			written := uint64(0)
			if si == 0 && len(e.prefix) > 0 {
				if err := writeAll(w, e.prefix); err != nil {
					return err
				}
				written += uint64(len(e.prefix))
			}

			s := samples[span.srcIdx]
			buf, err := readSampleBytes(r, s)
			if err != nil {
				return err
			}
			lengthSize := s.lengthSize
			pos := 0
			for pos < len(buf) {
				var n uint64
				for k := 0; k < lengthSize; k++ {
					n = n<<8 | uint64(buf[pos+k])
				}
				header := pos + lengthSize
				nalType := buf[header] & 0x1f
				if nalType != 7 && nalType != 8 {
					if err := writeAll(w, startCode); err != nil {
						return err
					}
					if err := writeAll(w, buf[header:header+int(n)]); err != nil {
						return err
					}
					written += 4 + n
				}
				pos = header + int(n)
			}
			if written != span.size {
				return fmt.Errorf("avc edits: internal write size mismatch at edit %d sample %d: wrote %d want %d",
					ei, span.srcIdx+1, written, span.size)
			}
		}
	}
	return nil
}

/*-------------------------------- helpers ----------------------------------*/

func addUint64(a, b uint64) (uint64, error) {
	if a > math.MaxUint64-b {
		return 0, errors.New("avc edits: uint64 overflow")
	}
	return a + b, nil
}

func addInt64Uint64(a int64, b uint64) (int64, error) {
	if b > math.MaxInt64 {
		// b may still combine with negative a; do big arithmetic.
		bb := new(big.Int).SetUint64(b)
		rr := new(big.Int).Add(big.NewInt(a), bb)
		if !rr.IsInt64() {
			return 0, errors.New("avc edits: int64 overflow")
		}
		return rr.Int64(), nil
	}
	r := a + int64(b)
	if int64(b) >= 0 && r < a {
		return 0, errors.New("avc edits: int64 overflow")
	}
	return r, nil
}
