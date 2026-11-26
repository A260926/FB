package mp4

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"io"
	"sort"
	"testing"
	"time"

	"github.com/nareix/joy4/codec/h264parser"
	"github.com/nareix/joy4/format/mp4/mp4io"
)

// ---------------------------------------------------------------------------
// Fixture construction
// ---------------------------------------------------------------------------

type fxSample struct {
	data []byte
	dur  uint32
	cto  uint32
}

type fxTrack struct {
	video     bool
	timescale uint32
	trackID   int32
	width     int16
	height    int16
	samples   []fxSample
	// chunks describes how source samples are grouped into source chunks;
	// each inner slice holds sample indices.
	chunks [][]int
}

type fxRef struct {
	tr int
	s  int
}

type fxFixture struct {
	file  []byte
	track []*fxTrack
	// sampleOff maps [2]int{track,sample} to its absolute source offset.
	sampleOff map[[2]int]int64
	mdatStart []int64
	mdatEnd   []int64
}

func fxMarshal(a mp4io.Atom) []byte {
	b := make([]byte, a.Len())
	a.Marshal(b)
	return b
}

func fxBox(tag string, payload []byte) []byte {
	b := make([]byte, 8+len(payload))
	binary.BigEndian.PutUint32(b[0:4], uint32(len(b)))
	copy(b[4:8], tag)
	copy(b[8:], payload)
	return b
}

func fxAVC1(t *testing.T) *mp4io.AVC1Desc {
	sps := mustHex(t, "6764000AAC7284442684000003000400000300CA3C4896118000")
	pps := mustHex(t, "68E8438F132130")
	codec, err := h264parser.NewCodecDataFromSPSAndPPS(sps, pps)
	if err != nil {
		t.Fatalf("build avc codec data: %v", err)
	}
	return &mp4io.AVC1Desc{
		DataRefIdx:           1,
		HorizontalResolution: 72,
		VorizontalResolution: 72,
		Width:                16,
		Height:               16,
		FrameCount:           1,
		Depth:                24,
		ColorTableId:         -1,
		Conf:                 &mp4io.AVC1Conf{Data: codec.AVCDecoderConfRecordBytes()},
	}
}

func fxMP4A() *mp4io.MP4ADesc {
	return &mp4io.MP4ADesc{
		DataRefIdx:       1,
		NumberOfChannels: 2,
		SampleSize:       16,
		SampleRate:       48000,
		Conf:             &mp4io.ElemStreamDesc{DecConfig: []byte{0x12, 0x10}},
	}
}

func mustHex(t *testing.T, s string) []byte {
	t.Helper()
	b := make([]byte, len(s)/2)
	for i := 0; i < len(b); i++ {
		var v int
		if _, err := fmt.Sscanf(s[i*2:i*2+2], "%02x", &v); err != nil {
			t.Fatalf("bad hex: %v", err)
		}
		b[i] = byte(v)
	}
	return b
}

func fxMergeSTTS(samples []fxSample) *mp4io.TimeToSample {
	tts := &mp4io.TimeToSample{}
	for _, s := range samples {
		if n := len(tts.Entries); n > 0 && tts.Entries[n-1].Duration == s.dur {
			tts.Entries[n-1].Count++
		} else {
			tts.Entries = append(tts.Entries, mp4io.TimeToSampleEntry{Count: 1, Duration: s.dur})
		}
	}
	return tts
}

func fxMergeCTTS(samples []fxSample) *mp4io.CompositionOffset {
	ctts := &mp4io.CompositionOffset{}
	for _, s := range samples {
		if n := len(ctts.Entries); n > 0 && ctts.Entries[n-1].Offset == s.cto {
			ctts.Entries[n-1].Count++
		} else {
			ctts.Entries = append(ctts.Entries, mp4io.CompositionOffsetEntry{Count: 1, Offset: s.cto})
		}
	}
	return ctts
}

func fxSTSC(chunks [][]int) *mp4io.SampleToChunk {
	stsc := &mp4io.SampleToChunk{}
	for i, c := range chunks {
		n := uint32(len(c))
		if k := len(stsc.Entries); k > 0 && stsc.Entries[k-1].SamplesPerChunk == n {
			continue
		}
		stsc.Entries = append(stsc.Entries, mp4io.SampleToChunkEntry{
			FirstChunk:      uint32(i + 1),
			SamplesPerChunk: n,
			SampleDescId:    1,
		})
	}
	return stsc
}

func fxTrackBoxes(t *testing.T, tr *fxTrack, chunkOffsets []uint32, movieTimescale int32) []byte {
	t.Helper()
	marshal := func(a mp4io.Atom) []byte { return fxMarshal(a) }

	duration := uint32(0)
	for _, s := range tr.samples {
		duration += s.dur
	}
	var movieDur int32
	if tr.timescale != 0 {
		movieDur = int32(int64(duration) * int64(movieTimescale) / int64(tr.timescale))
	}

	tkhd := &mp4io.TrackHeader{
		TrackId:     tr.trackID,
		Duration:    movieDur,
		Flags:       0x0003,
		Matrix:      [9]int32{0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000},
		TrackWidth:  float64(tr.width),
		TrackHeight: float64(tr.height),
	}
	mdhd := &mp4io.MediaHeader{
		TimeScale: int32(tr.timescale),
		Duration:  int32(duration),
		Language:  21956,
	}

	var hdlr *mp4io.HandlerRefer
	var mhd []byte
	if tr.video {
		hdlr = &mp4io.HandlerRefer{SubType: [4]byte{'v', 'i', 'd', 'e'}, Name: []byte("VideoHandler")}
		mhd = marshal(&mp4io.VideoMediaInfo{Flags: 1})
	} else {
		hdlr = &mp4io.HandlerRefer{SubType: [4]byte{'s', 'o', 'u', 'n'}, Name: []byte("SoundHandler")}
		mhd = marshal(&mp4io.SoundMediaInfo{})
	}

	dinf := marshal(&mp4io.DataInfo{
		Refer: &mp4io.DataRefer{Url: &mp4io.DataReferUrl{Flags: 1}},
	})

	var stsd []byte
	if tr.video {
		stsd = marshal(&mp4io.SampleDesc{AVC1Desc: fxAVC1(t)})
	} else {
		stsd = marshal(&mp4io.SampleDesc{MP4ADesc: fxMP4A()})
	}
	stts := marshal(fxMergeSTTS(tr.samples))
	var ctts []byte
	if tr.video {
		ctts = marshal(fxMergeCTTS(tr.samples))
	}
	stsc := marshal(fxSTSC(tr.chunks))
	var stss []byte
	if tr.video {
		sync := &mp4io.SyncSample{}
		for i := range tr.samples {
			if i%3 == 0 {
				sync.Entries = append(sync.Entries, uint32(i+1))
			}
		}
		stss = marshal(sync)
	}
	stco := marshal(&mp4io.ChunkOffset{Entries: chunkOffsets})
	stsz := &mp4io.SampleSize{Entries: make([]uint32, len(tr.samples))}
	for i, s := range tr.samples {
		stsz.Entries[i] = uint32(len(s.data))
	}

	stblBody := append([]byte{}, stsd...)
	stblBody = append(stblBody, stts...)
	stblBody = append(stblBody, ctts...)
	stblBody = append(stblBody, stsc...)
	stblBody = append(stblBody, stss...)
	stblBody = append(stblBody, stco...)
	stblBody = append(stblBody, fxMarshal(stsz)...)
	stbl := fxBox("stbl", stblBody)

	minfBody := append([]byte{}, mhd...)
	minfBody = append(minfBody, dinf...)
	minfBody = append(minfBody, stbl...)
	minf := fxBox("minf", minfBody)

	mdiaBody := append([]byte{}, marshal(mdhd)...)
	mdiaBody = append(mdiaBody, marshal(hdlr)...)
	mdiaBody = append(mdiaBody, minf...)
	mdia := fxBox("mdia", mdiaBody)

	trakBody := append([]byte{}, marshal(tkhd)...)
	trakBody = append(trakBody, mdia...)
	return fxBox("trak", trakBody)
}

// fxBuild assembles a source file of the form
//
//	[ftyp] free mdat* moov
//
// with samples laid out in mdat payloads following mdats, and small non-sample
// gaps between the tracks' sections so that a rechunker must not copy them.
func fxBuild(t *testing.T, withFtyp bool, tracks []*fxTrack, mdats [][]fxRef) *fxFixture {
	t.Helper()
	return fxBuildAt(t, withFtyp, false, tracks, mdats)
}

// fxBuildAt is fxBuild with a choice of moov placement. When moovFirst is set
// the file layout is [ftyp] moov mdat*, otherwise [ftyp] free mdat* moov.
func fxBuildAt(t *testing.T, withFtyp, moovFirst bool, tracks []*fxTrack, mdats [][]fxRef) *fxFixture {
	t.Helper()

	var ftyp []byte
	if withFtyp {
		ftyp = fxBox("ftyp", []byte{'i', 's', 'o', 'm', 0, 0, 0, 0, 'i', 's', 'o', 'm'})
	}
	var free []byte
	if !moovFirst {
		free = fxBox("free", make([]byte, 8))
	}
	gap := []byte{0xDE, 0xAD, 0xBE, 0xEF, 0xEE}

	payloads := make([][]byte, len(mdats))
	for m, refs := range mdats {
		var payload []byte
		for i, r := range refs {
			if i > 0 && refs[i-1].tr != r.tr {
				payload = append(payload, gap...)
			}
			payload = append(payload, tracks[r.tr].samples[r.s].data...)
		}
		payload = append(payload, gap...)
		payloads[m] = payload
	}

	fixture := &fxFixture{
		track:     tracks,
		sampleOff: map[[2]int]int64{},
		mdatStart: make([]int64, len(mdats)),
		mdatEnd:   make([]int64, len(mdats)),
	}

	movieTimescale := int32(1000)
	var movieDuration int32
	for _, tr := range tracks {
		var d uint32
		for _, s := range tr.samples {
			d += s.dur
		}
		if md := int32(int64(d) * int64(movieTimescale) / int64(tr.timescale)); md > movieDuration {
			movieDuration = md
		}
	}
	mvhd := &mp4io.MovieHeader{
		TimeScale:       movieTimescale,
		Duration:        movieDuration,
		PreferredRate:   1,
		PreferredVolume: 1,
		Matrix:          [9]int32{0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000},
		NextTrackId:     int32(len(tracks) + 1),
	}

	buildMoov := func(offsetFor func(ti, ci int) uint32) []byte {
		traks := make([][]byte, len(tracks))
		for ti, tr := range tracks {
			offsets := make([]uint32, len(tr.chunks))
			for ci := range tr.chunks {
				offsets[ci] = offsetFor(ti, ci)
			}
			traks[ti] = fxTrackBoxes(t, tr, offsets, movieTimescale)
		}
		moovBody := append([]byte{}, fxMarshal(mvhd)...)
		for _, trak := range traks {
			moovBody = append(moovBody, trak...)
		}
		return fxBox("moov", moovBody)
	}

	// Chunk offsets do not affect the moov byte length, so the size is stable
	// before real offsets are known.
	moovSize := len(buildMoov(func(int, int) uint32 { return 0 }))
	mdatStartOffset := int64(len(ftyp) + len(free))
	if moovFirst {
		mdatStartOffset = int64(len(ftyp) + moovSize)
	}

	pos := mdatStartOffset
	for m, payload := range payloads {
		fixture.mdatStart[m] = pos
		payloadStart := pos + 8
		var within int64
		for i, r := range mdats[m] {
			if i > 0 && mdats[m][i-1].tr != r.tr {
				within += int64(len(gap))
			}
			fixture.sampleOff[[2]int{r.tr, r.s}] = payloadStart + within
			within += int64(len(tracks[r.tr].samples[r.s].data))
		}
		fixture.mdatEnd[m] = pos + 8 + int64(len(payload))
		pos += 8 + int64(len(payload))
	}

	// Verify each chunk's samples are physically contiguous, as a real source
	// file would have them.
	for ti, tr := range tracks {
		for _, chunk := range tr.chunks {
			for k := 1; k < len(chunk); k++ {
				prev := fixture.sampleOff[[2]int{ti, chunk[k-1]}]
				prev += int64(len(tr.samples[chunk[k-1]].data))
				if got := fixture.sampleOff[[2]int{ti, chunk[k]}]; got != prev {
					t.Fatalf("fixture: track %d chunk sample %d is not contiguous", ti, chunk[k])
				}
			}
		}
	}

	moov := buildMoov(func(ti, ci int) uint32 {
		return uint32(fixture.sampleOff[[2]int{ti, tracks[ti].chunks[ci][0]}])
	})
	if len(moov) != moovSize {
		t.Fatalf("fixture: moov size instability")
	}

	var file []byte
	file = append(file, ftyp...)
	file = append(file, free...)
	if moovFirst {
		file = append(file, moov...)
	}
	for _, payload := range payloads {
		header := make([]byte, 8)
		binary.BigEndian.PutUint32(header[0:4], uint32(8+len(payload)))
		copy(header[4:8], "mdat")
		file = append(file, header...)
		file = append(file, payload...)
	}
	if !moovFirst {
		file = append(file, moov...)
	}
	fixture.file = file
	return fixture
}

// ---------------------------------------------------------------------------
// In-memory destination and table helpers
// ---------------------------------------------------------------------------

type memWriteSeeker struct {
	buf []byte
	pos int64
}

func (m *memWriteSeeker) Write(p []byte) (int, error) {
	end := m.pos + int64(len(p))
	if end > int64(len(m.buf)) {
		grown := make([]byte, end)
		copy(grown, m.buf)
		m.buf = grown
	}
	copy(m.buf[m.pos:end], p)
	m.pos = end
	return len(p), nil
}

func (m *memWriteSeeker) Seek(offset int64, whence int) (int64, error) {
	var abs int64
	switch whence {
	case io.SeekStart:
		abs = offset
	case io.SeekCurrent:
		abs = m.pos + offset
	case io.SeekEnd:
		abs = int64(len(m.buf)) + offset
	default:
		return 0, fmt.Errorf("bad whence %d", whence)
	}
	if abs < 0 {
		return 0, fmt.Errorf("negative seek")
	}
	m.pos = abs
	return abs, nil
}

func fxParse(t *testing.T, data []byte) (*mp4io.Movie, []mp4io.Atom) {
	t.Helper()
	atoms, err := mp4io.ReadFileAtoms(bytes.NewReader(data))
	if err != nil {
		t.Fatalf("parse file: %v", err)
	}
	for _, a := range atoms {
		if mv, ok := a.(*mp4io.Movie); ok {
			return mv, atoms
		}
	}
	t.Fatalf("no moov found")
	return nil, nil
}

// fxSampleOffsets expands stsc/stco for a track and returns the absolute file
// offset of every sample in decode order.
func fxSampleOffsets(t *testing.T, track *mp4io.Track) []int64 {
	t.Helper()
	stbl := track.Media.Info.Sample
	sizeCount := len(stbl.SampleSize.Entries)
	offsets := make([]int64, sizeCount)

	sample := 0
	run := 0
	for ci := 0; ci < len(stbl.ChunkOffset.Entries); ci++ {
		for run+1 < len(stbl.SampleToChunk.Entries) &&
			int(stbl.SampleToChunk.Entries[run+1].FirstChunk) <= ci+1 {
			run++
		}
		n := int(stbl.SampleToChunk.Entries[run].SamplesPerChunk)
		off := int64(stbl.ChunkOffset.Entries[ci])
		for k := 0; k < n; k++ {
			if sample >= sizeCount {
				t.Fatalf("stsc/stco cover more samples than stsz")
			}
			offsets[sample] = off
			off += int64(stbl.SampleSize.Entries[sample])
			sample++
		}
	}
	if sample != sizeCount {
		t.Fatalf("stsc/stco cover %d samples, stsz has %d", sample, sizeCount)
	}
	return offsets
}

// fxChunks expands stsc/stco into per-chunk sample counts (decode order).
func fxChunks(t *testing.T, track *mp4io.Track) []int {
	t.Helper()
	var out []int
	run := 0
	for ci := 0; ci < len(track.Media.Info.Sample.ChunkOffset.Entries); ci++ {
		for run+1 < len(track.Media.Info.Sample.SampleToChunk.Entries) &&
			int(track.Media.Info.Sample.SampleToChunk.Entries[run+1].FirstChunk) <= ci+1 {
			run++
		}
		out = append(out, int(track.Media.Info.Sample.SampleToChunk.Entries[run].SamplesPerChunk))
	}
	return out
}

func fxEqualChunks(a, b []int) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// topTags returns the tags of every top-level box in data, in file order.
func topTags(t *testing.T, data []byte) []string {
	t.Helper()
	var tags []string
	for off := 0; off+8 <= len(data); {
		size := int(binary.BigEndian.Uint32(data[off : off+4]))
		if size < 8 || off+size > len(data) {
			t.Fatalf("topTags: bad box at %d", off)
		}
		tags = append(tags, string(data[off+4:off+8]))
		off += size
	}
	return tags
}

// ---------------------------------------------------------------------------
// TestRechunkLayout
// ---------------------------------------------------------------------------

func fxLayoutFixture(t *testing.T) *fxFixture {
	t.Helper()
	video := &fxTrack{
		video:     true,
		timescale: 90000,
		trackID:   1,
		width:     16,
		height:    16,
		chunks:    [][]int{{0}, {1, 2}, {3}, {4, 5}},
	}
	videoData := []int{4, 7, 5, 9, 6, 8}
	for i := 0; i < 6; i++ {
		data := make([]byte, videoData[i])
		data[0] = 0xA0 + byte(i)
		for k := 1; k < len(data); k++ {
			data[k] = byte(0xC0 + i + k)
		}
		video.samples = append(video.samples, fxSample{
			data: data,
			dur:  3000,
			cto:  []uint32{0, 6000, 3000, 0, 3000, 0}[i],
		})
	}
	audio := &fxTrack{
		video:     false,
		timescale: 48000,
		trackID:   2,
		chunks:    [][]int{{0, 1}, {2}, {3, 4}, {5}},
	}
	audioData := []int{3, 5, 4, 6, 3, 7}
	for i := 0; i < 6; i++ {
		data := make([]byte, audioData[i])
		data[0] = 0x50 + byte(i)
		for k := 1; k < len(data); k++ {
			data[k] = byte(0x90+i) ^ byte(k)
		}
		audio.samples = append(audio.samples, fxSample{
			data: data,
			dur:  2048,
		})
	}

	mdat0 := []fxRef{}
	mdat1 := []fxRef{}
	for i := 0; i < 3; i++ {
		mdat0 = append(mdat0, fxRef{0, i})
	}
	for i := 0; i < 3; i++ {
		mdat0 = append(mdat0, fxRef{1, i})
	}
	for i := 3; i < 6; i++ {
		mdat1 = append(mdat1, fxRef{0, i})
	}
	for i := 3; i < 6; i++ {
		mdat1 = append(mdat1, fxRef{1, i})
	}
	return fxBuild(t, true, []*fxTrack{video, audio}, [][]fxRef{mdat0, mdat1})
}

func TestRechunkLayout(t *testing.T) {
	fx := fxLayoutFixture(t)
	const chunkDuration = 100 * time.Millisecond

	src := bytes.NewReader(fx.file)
	var dst memWriteSeeker
	if err := Rechunk(src, &dst, chunkDuration); err != nil {
		t.Fatalf("Rechunk: %v", err)
	}
	out := dst.buf

	// --- top-level layout: ftyp, moov, single mdat -------------------------
	srcMovie, srcAtoms := fxParse(t, fx.file)
	mdats := 0
	for _, a := range srcAtoms {
		if a.Tag() == mp4io.MDAT {
			mdats++
		}
	}
	if mdats != 2 {
		t.Fatalf("source has %d mdats, want 2", mdats)
	}

	movie, atoms := fxParse(t, out)
	if len(atoms) != 3 {
		t.Fatalf("output has %d top-level atoms, want 3 (ftyp, moov, mdat): %v", len(atoms), atomTags(atoms))
	}
	if atoms[0].Tag() != mp4io.StringToTag("ftyp") ||
		atoms[1].Tag() != mp4io.MOOV ||
		atoms[2].Tag() != mp4io.MDAT {
		t.Fatalf("output top-level order = %v, want ftyp, moov, mdat", atomTags(atoms))
	}
	moovPos, moovSize := atoms[1].Pos()
	if ftypPos, ftypSize := atoms[0].Pos(); moovPos != ftypPos+ftypSize {
		t.Fatalf("moov does not immediately follow ftyp: moov off=%d ftyp end=%d", moovPos, ftypPos+ftypSize)
	}
	if _, srcMoovSize := fxFindTop(t, fx.file, "moov"); srcMoovSize == moovSize {
		t.Fatalf("moov size did not change")
	}
	mdatOff, mdatSize := atoms[2].Pos()
	mdatPayload := int64(mdatOff) + 8
	mdatPayloadEnd := int64(mdatOff + mdatSize)
	if mdatPayloadEnd != int64(len(out)) {
		t.Fatalf("mdat does not end at EOF: end=%d len=%d", mdatPayloadEnd, len(out))
	}
	if len(movie.Tracks) != 2 {
		t.Fatalf("output has %d tracks, want 2", len(movie.Tracks))
	}

	// --- preserved movie/track identity and tables ------------------------
	if !bytes.Equal(fxMarshal(movie.Header), fxMarshal(srcMovie.Header)) {
		t.Fatalf("mvhd changed")
	}
	if !bytes.Equal(fxMarshal(movie.Tracks[0].Header), fxMarshal(srcMovie.Tracks[0].Header)) {
		t.Fatalf("tkhd[0] changed")
	}
	if !bytes.Equal(fxMarshal(movie.Tracks[1].Header), fxMarshal(srcMovie.Tracks[1].Header)) {
		t.Fatalf("tkhd[1] changed")
	}
	if !bytes.Equal(fxMarshal(movie.Tracks[0].Media.Header), fxMarshal(srcMovie.Tracks[0].Media.Header)) {
		t.Fatalf("mdhd[0] changed")
	}
	if !bytes.Equal(fxMarshal(movie.Tracks[1].Media.Header), fxMarshal(srcMovie.Tracks[1].Media.Header)) {
		t.Fatalf("mdhd[1] changed")
	}

	for ti := 0; ti < 2; ti++ {
		got := movie.Tracks[ti].Media.Info.Sample
		want := srcMovie.Tracks[ti].Media.Info.Sample
		if !bytes.Equal(fxMarshal(got.SampleDesc), fxMarshal(want.SampleDesc)) {
			t.Fatalf("track %d stsd changed", ti)
		}
		if !bytes.Equal(fxMarshal(got.TimeToSample), fxMarshal(want.TimeToSample)) {
			t.Fatalf("track %d stts changed", ti)
		}
		if !bytes.Equal(fxMarshal(got.SampleSize), fxMarshal(want.SampleSize)) {
			t.Fatalf("track %d stsz changed", ti)
		}
		if (got.CompositionOffset == nil) != (want.CompositionOffset == nil) {
			t.Fatalf("track %d ctts presence changed", ti)
		}
		if got.CompositionOffset != nil && !bytes.Equal(fxMarshal(got.CompositionOffset), fxMarshal(want.CompositionOffset)) {
			t.Fatalf("track %d ctts changed", ti)
		}
		if ti == 0 {
			if (got.SyncSample == nil) != (want.SyncSample == nil) {
				t.Fatalf("track %d stss presence changed", ti)
			}
			if got.SyncSample != nil && !bytes.Equal(fxMarshal(got.SyncSample), fxMarshal(want.SyncSample)) {
				t.Fatalf("track %d stss changed", ti)
			}
		}
		if got.SampleSize.SampleSize != 0 {
			t.Fatalf("stsz became fixed-size")
		}
	}

	// --- new stsc starts at 1, monotonic, covers exactly the sample count --
	for ti := 0; ti < 2; ti++ {
		stsc := movie.Tracks[ti].Media.Info.Sample.SampleToChunk
		if len(stsc.Entries) == 0 || stsc.Entries[0].FirstChunk != 1 {
			t.Fatalf("track %d stsc does not start at chunk 1", ti)
		}
		for i := 1; i < len(stsc.Entries); i++ {
			if stsc.Entries[i].FirstChunk <= stsc.Entries[i-1].FirstChunk {
				t.Fatalf("track %d stsc is not strictly increasing", ti)
			}
		}
		for _, e := range stsc.Entries {
			if e.SampleDescId != 1 {
				t.Fatalf("track %d stsc sample description index = %d", ti, e.SampleDescId)
			}
			if e.SamplesPerChunk == 0 {
				t.Fatalf("track %d stsc has zero samples per chunk", ti)
			}
		}
	}

	// --- expected chunk boundaries -----------------------------------------
	wantChunks := [][]int{
		{3, 3},    // video: 3 * 3000 ticks exactly reaches 100ms
		{2, 2, 2}, // audio: 2 * 2048 <= 4800 < 3 * 2048
	}
	for ti := 0; ti < 2; ti++ {
		if got := fxChunks(t, movie.Tracks[ti]); !fxEqualChunks(got, wantChunks[ti]) {
			t.Fatalf("track %d chunk sizes = %v, want %v", ti, got, wantChunks[ti])
		}
	}

	// --- interleaving order and exact addresses ---------------------------
	type expectedChunk struct {
		track  int
		first  int
		count  int
		offset int64
	}
	// Video chunk 0 dts=0, audio chunk 0 dts=0 -> track order; then audio
	// chunk 1 (4096/48000) before video chunk 1 (9000/90000).
	expected := []expectedChunk{
		{track: 0, first: 0, count: 3},
		{track: 1, first: 0, count: 2},
		{track: 1, first: 2, count: 2},
		{track: 0, first: 3, count: 3},
		{track: 1, first: 4, count: 2},
	}
	var cursor int64 = mdatPayload
	for i := range expected {
		e := &expected[i]
		e.offset = cursor
		for k := 0; k < e.count; k++ {
			cursor += int64(len(fx.track[e.track].samples[e.first+k].data))
		}
	}
	if cursor != int64(len(out)) {
		t.Fatalf("expected chunk sizes sum to %d, mdat payload ends at %d", cursor, len(out))
	}

	perTrackChunk := [2]int{}
	for i, e := range expected {
		stco := movie.Tracks[e.track].Media.Info.Sample.ChunkOffset.Entries
		if perTrackChunk[e.track] >= len(stco) {
			t.Fatalf("expected chunk %d references missing stco entry for track %d", i, e.track)
		}
		gotOffset := int64(stco[perTrackChunk[e.track]])
		if gotOffset != e.offset {
			t.Fatalf("chunk %d (track %d) offset = %d, want %d", i, e.track, gotOffset, e.offset)
		}
		perTrackChunk[e.track]++
	}

	// --- sample bytes and contiguity at their actual addresses ------------
	srcOffsets := map[int][]int64{}
	for ti := 0; ti < 2; ti++ {
		srcOffsets[ti] = fxSampleOffsets(t, srcMovie.Tracks[ti])
		outOffsets := fxSampleOffsets(t, movie.Tracks[ti])
		for si := range fx.track[ti].samples {
			want := fx.track[ti].samples[si].data
			srcStart := srcOffsets[ti][si]
			if !bytes.Equal(fx.file[srcStart:srcStart+int64(len(want))], want) {
				t.Fatalf("fixture: source bytes for track %d sample %d mismatch", ti, si)
			}
			start := outOffsets[si]
			got := out[start : start+int64(len(want))]
			if !bytes.Equal(got, want) {
				t.Fatalf("track %d sample %d bytes = %x, want %x", ti, si, got, want)
			}
		}
	}

	// The padding bytes present in the source mdats must not have been copied.
	totalSamples := int64(0)
	for _, tr := range fx.track {
		for _, s := range tr.samples {
			totalSamples += int64(len(s.data))
		}
	}
	if mdatPayloadEnd-mdatPayload != totalSamples {
		t.Fatalf("mdat payload size = %d, want sum of sample sizes %d (gaps copied?)", mdatPayloadEnd-mdatPayload, totalSamples)
	}

	// --- original Demuxer can read a prefix and seek ----------------------
	d := NewDemuxer(bytes.NewReader(out))
	if _, err := d.Streams(); err != nil {
		t.Fatalf("demuxer Streams: %v", err)
	}
	wantPrefix := []struct {
		idx  int8
		data []byte
	}{
		{0, fx.track[0].samples[0].data},
		{1, fx.track[1].samples[0].data},
		{0, fx.track[0].samples[1].data},
		{1, fx.track[1].samples[1].data},
	}
	for i, wp := range wantPrefix {
		pkt, err := d.ReadPacket()
		if err != nil {
			t.Fatalf("demuxer ReadPacket #%d: %v", i, err)
		}
		if pkt.Idx != wp.idx {
			t.Fatalf("packet #%d stream = %d, want %d", i, pkt.Idx, wp.idx)
		}
		if !bytes.Equal(pkt.Data, wp.data) {
			t.Fatalf("packet #%d data = %x, want %x", i, pkt.Data, wp.data)
		}
		if pkt.Time < 0 {
			t.Fatalf("packet #%d has negative time", i)
		}
	}
	if err := d.SeekToTime(30 * time.Millisecond); err != nil {
		t.Fatalf("SeekToTime: %v", err)
	}
	pkt, err := d.ReadPacket()
	if err != nil {
		t.Fatalf("ReadPacket after seek: %v", err)
	}
	if !bytes.Equal(pkt.Data, fx.track[0].samples[0].data) &&
		!bytes.Equal(pkt.Data, fx.track[1].samples[0].data) {
		t.Fatalf("packet after seek does not match a sample at the seek point: %x", pkt.Data)
	}
	// The remaining chunks, in order, must be the physical order of the output
	// mdat. Every chunk's stco entry is cross-checked against the running byte
	// position, and the walk must consume exactly the mdat payload.
	seenPerTrack := [2]int{}
	var physPos int64 = mdatPayload
	firstSampleOf := func(ti int) int {
		n := 0
		for _, c := range wantChunks[ti][:seenPerTrack[ti]] {
			n += c
		}
		return n
	}
	for {
		bestTrack := -1
		var bestDTS int64
		for ti := 0; ti < 2; ti++ {
			if seenPerTrack[ti] >= len(wantChunks[ti]) {
				continue
			}
			first := firstSampleOf(ti)
			dts := int64(0)
			for k := 0; k < first; k++ {
				dts += int64(fx.track[ti].samples[k].dur)
			}
			if bestTrack < 0 ||
				dts*int64(fx.track[bestTrack].timescale) < bestDTS*int64(fx.track[ti].timescale) {
				bestTrack = ti
				bestDTS = dts
			}
		}
		if bestTrack < 0 {
			break
		}
		offset := int64(movie.Tracks[bestTrack].Media.Info.Sample.ChunkOffset.Entries[seenPerTrack[bestTrack]])
		if offset != physPos {
			t.Fatalf("track %d chunk %d physical offset = %d, want %d", bestTrack, seenPerTrack[bestTrack], offset, physPos)
		}
		sample := firstSampleOf(bestTrack)
		for k, n := 0, wantChunks[bestTrack][seenPerTrack[bestTrack]]; k < n; k++ {
			physPos += int64(len(fx.track[bestTrack].samples[sample+k].data))
		}
		seenPerTrack[bestTrack]++
	}
	if physPos != int64(len(out)) {
		t.Fatalf("walked %d bytes of mdat, file ends at %d", physPos, len(out))
	}
}

func atomTags(atoms []mp4io.Atom) string {
	s := ""
	for i, a := range atoms {
		if i > 0 {
			s += ","
		}
		s += a.Tag().String()
	}
	return s
}

// ---------------------------------------------------------------------------
// TestRechunkTables
// ---------------------------------------------------------------------------

// fxBuildAudioOnly builds a single-track file whose samples have the given
// durations, split into source chunks of at most two samples.
func fxBuildAudioOnly(t *testing.T, durs []uint32) *fxFixture {
	t.Helper()
	tr := &fxTrack{video: false, timescale: 1000, trackID: 1}
	for i, d := range durs {
		tr.samples = append(tr.samples, fxSample{
			data: []byte{byte(0x10 + i), byte(i), byte(0x80 ^ i)},
			dur:  d,
		})
	}
	var refs []fxRef
	var chunk []int
	for i := range tr.samples {
		chunk = append(chunk, i)
		if len(chunk) == 2 || i == len(tr.samples)-1 {
			tr.chunks = append(tr.chunks, chunk)
			for _, s := range chunk {
				refs = append(refs, fxRef{0, s})
			}
			chunk = nil
		}
	}
	return fxBuild(t, false, []*fxTrack{tr}, [][]fxRef{refs})
}

func fxRechunkOK(t *testing.T, fx *fxFixture, chunkDuration time.Duration) (*mp4io.Movie, []byte) {
	t.Helper()
	out := fxRechunkBytes(t, fx.file, chunkDuration)
	movie, _ := fxParse(t, out)
	return movie, out
}

func fxRechunkBytes(t *testing.T, file []byte, chunkDuration time.Duration) []byte {
	t.Helper()
	var dst memWriteSeeker
	if err := Rechunk(bytes.NewReader(file), &dst, chunkDuration); err != nil {
		t.Fatalf("Rechunk: %v", err)
	}
	return dst.buf
}

func TestRechunkTables(t *testing.T) {
	// A single track: all samples land in one chunk when the duration budget
	// is large.
	t.Run("single track", func(t *testing.T) {
		fx := fxBuildAudioOnly(t, []uint32{1000, 1000, 1000})
		movie, out := fxRechunkOK(t, fx, 10*time.Second)
		if got := fxChunks(t, movie.Tracks[0]); !fxEqualChunks(got, []int{3}) {
			t.Fatalf("chunks = %v, want [3]", got)
		}
		srcMovie, _ := fxParse(t, fx.file)
		if !bytes.Equal(fxMarshal(movie.Tracks[0].Media.Info.Sample.TimeToSample),
			fxMarshal(srcMovie.Tracks[0].Media.Info.Sample.TimeToSample)) {
			t.Fatalf("stts changed")
		}
		// No ftyp in the source means no ftyp in the output.
		if tag := topTags(t, out); tag[0] != "moov" || tag[1] != "mdat" || len(tag) != 2 {
			t.Fatalf("output without source ftyp = %v, want [moov mdat]", tag)
		}
	})

	// A group whose accumulated duration exactly equals the budget must stay
	// together; the next sample opens a new chunk.
	t.Run("exact duration", func(t *testing.T) {
		fx := fxBuildAudioOnly(t, []uint32{1000, 1000, 1000, 1000})
		movie, _ := fxRechunkOK(t, fx, 3*time.Second)
		if got := fxChunks(t, movie.Tracks[0]); !fxEqualChunks(got, []int{3, 1}) {
			t.Fatalf("chunks = %v, want [3 1]", got)
		}
		// A budget one tick short of the exact total must split earlier.
		movie, _ = fxRechunkOK(t, fx, time.Duration(2999)*time.Millisecond)
		if got := fxChunks(t, movie.Tracks[0]); !fxEqualChunks(got, []int{2, 2}) {
			t.Fatalf("chunks = %v, want [2 2] under a short budget", got)
		}
	})

	// A sample longer than the whole budget occupies a chunk by itself.
	t.Run("oversized sample", func(t *testing.T) {
		fx := fxBuildAudioOnly(t, []uint32{1000, 5000, 1000})
		movie, _ := fxRechunkOK(t, fx, 2*time.Second)
		if got := fxChunks(t, movie.Tracks[0]); !fxEqualChunks(got, []int{1, 1, 1}) {
			t.Fatalf("chunks = %v, want [1 1 1]", got)
		}
	})

	// The source may already put moov before the media. Rechunking must still
	// produce the same canonical layout and identical bytes.
	t.Run("moov before mdat", func(t *testing.T) {
		base := fxLayoutFixture(t)
		mdat0 := []fxRef{}
		mdat1 := []fxRef{}
		for i := 0; i < 3; i++ {
			mdat0 = append(mdat0, fxRef{0, i})
		}
		for i := 0; i < 3; i++ {
			mdat0 = append(mdat0, fxRef{1, i})
		}
		for i := 3; i < 6; i++ {
			mdat1 = append(mdat1, fxRef{0, i})
		}
		for i := 3; i < 6; i++ {
			mdat1 = append(mdat1, fxRef{1, i})
		}
		early := fxBuildAt(t, true, true, base.track, [][]fxRef{mdat0, mdat1})
		atoms, err := mp4io.ReadFileAtoms(bytes.NewReader(early.file))
		if err != nil {
			t.Fatalf("parse source: %v", err)
		}
		moovOff, mdatOff := -1, -1
		for _, a := range atoms {
			off, _ := a.Pos()
			if a.Tag() == mp4io.MOOV {
				moovOff = off
			}
			if a.Tag() == mp4io.MDAT && mdatOff < 0 {
				mdatOff = off
			}
		}
		if moovOff < 0 || mdatOff < 0 || moovOff > mdatOff {
			t.Fatalf("fixture: moov is not before mdat (moov=%d mdat=%d)", moovOff, mdatOff)
		}
		lateOut := fxRechunkBytes(t, base.file, 100*time.Millisecond)
		earlyOut := fxRechunkBytes(t, early.file, 100*time.Millisecond)
		if !bytes.Equal(lateOut, earlyOut) {
			t.Fatalf("rechunk output depends on source moov placement")
		}
	})

	// A trailing zero-duration sample must be kept, and its zero duration must
	// still appear in stts.
	t.Run("trailing zero duration", func(t *testing.T) {
		fx := fxBuildAudioOnly(t, []uint32{1000, 1000, 0})
		movie, _ := fxRechunkOK(t, fx, 2*time.Second)
		if got := fxChunks(t, movie.Tracks[0]); !fxEqualChunks(got, []int{3}) {
			t.Fatalf("chunks = %v, want [3]", got)
		}
		srcMovie, _ := fxParse(t, fx.file)
		srcSTTS := srcMovie.Tracks[0].Media.Info.Sample.TimeToSample
		gotSTTS := movie.Tracks[0].Media.Info.Sample.TimeToSample
		if !bytes.Equal(fxMarshal(gotSTTS), fxMarshal(srcSTTS)) {
			t.Fatalf("stts changed for trailing zero-duration sample")
		}
		total := uint32(0)
		for _, e := range gotSTTS.Entries {
			total += e.Count
		}
		if total != 3 || len(movie.Tracks[0].Media.Info.Sample.SampleSize.Entries) != 3 {
			t.Fatalf("trailing zero-duration sample was dropped")
		}
	})

	// The full two-track fixture: chunking must be driven by each track's own
	// integer timescale, and stsc must cover exactly the sample count.
	t.Run("timescales and coverage", func(t *testing.T) {
		fx := fxLayoutFixture(t)
		movie, _ := fxRechunkOK(t, fx, 100*time.Millisecond)
		for ti, want := range [][]int{{3, 3}, {2, 2, 2}} {
			stbl := movie.Tracks[ti].Media.Info.Sample
			sampleCount := 0
			for _, n := range fxChunks(t, movie.Tracks[ti]) {
				sampleCount += n
			}
			if sampleCount != len(stbl.SampleSize.Entries) {
				t.Fatalf("track %d stsc covers %d samples, stsz has %d", ti, sampleCount, len(stbl.SampleSize.Entries))
			}
			if got := fxChunks(t, movie.Tracks[ti]); !fxEqualChunks(got, want) {
				t.Fatalf("track %d chunks = %v, want %v", ti, got, want)
			}
			if int(movie.Tracks[ti].Media.Header.TimeScale) != int(fx.track[ti].timescale) {
				t.Fatalf("track %d timescale changed", ti)
			}
		}
	})

	// Sample offsets must be recomputed from the final moov size, not shifted
	// by a constant: the source file's first sample offset is inside its first
	// mdat (after ftyp+free), while the output's is after ftyp+moov.
	t.Run("offsets follow final moov size", func(t *testing.T) {
		fx := fxLayoutFixture(t)
		movie, out := fxRechunkOK(t, fx, 100*time.Millisecond)
		firstSrc := fx.sampleOff[[2]int{0, 0}]
		firstOut := fxSampleOffsets(t, movie.Tracks[0])[0]
		if firstSrc == firstOut {
			t.Fatalf("source and output first offsets are identical")
		}
		moovOff, moovSize := int64(0), int64(0)
		atoms, err := mp4io.ReadFileAtoms(bytes.NewReader(out))
		if err != nil {
			t.Fatalf("parse: %v", err)
		}
		for _, a := range atoms {
			if a.Tag() == mp4io.MOOV {
				off, size := a.Pos()
				moovOff, moovSize = int64(off), int64(size)
			}
		}
		if want := moovOff + moovSize + 8; firstOut != want {
			t.Fatalf("first output sample offset = %d, want %d (after ftyp+moov+mdat header)", firstOut, want)
		}
	})

	// The physical order of chunks, derived from stco, must be sorted by each
	// chunk's first-sample DTS (ties broken by track order), and each track's
	// chunks must appear in decode order.
	t.Run("chunk order follows dts", func(t *testing.T) {
		fx := fxLayoutFixture(t)
		movie, out := fxRechunkOK(t, fx, 100*time.Millisecond)

		type chunkRef struct {
			track int
			first int
			count int
			dts   int64
			off   int64
		}
		var list []chunkRef
		for ti := 0; ti < 2; ti++ {
			stbl := movie.Tracks[ti].Media.Info.Sample
			sample := 0
			for ci := 0; ci < len(stbl.ChunkOffset.Entries); ci++ {
				run := 0
				for run+1 < len(stbl.SampleToChunk.Entries) &&
					int(stbl.SampleToChunk.Entries[run+1].FirstChunk) <= ci+1 {
					run++
				}
				count := int(stbl.SampleToChunk.Entries[run].SamplesPerChunk)
				dts := int64(0)
				for k := 0; k < sample; k++ {
					dts += int64(fx.track[ti].samples[k].dur)
				}
				list = append(list, chunkRef{
					track: ti, first: sample, count: count, dts: dts,
					off: int64(stbl.ChunkOffset.Entries[ci]),
				})
				sample += count
			}
			if sample != len(fx.track[ti].samples) {
				t.Fatalf("track %d coverage mismatch", ti)
			}
		}
		sort.Slice(list, func(i, j int) bool { return list[i].off < list[j].off })
		for i := 1; i < len(list); i++ {
			a, b := list[i-1], list[i]
			// Cross-multiply to compare dts/timescale exactly.
			la := a.dts * int64(fx.track[b.track].timescale)
			rb := b.dts * int64(fx.track[a.track].timescale)
			if la > rb {
				t.Fatalf("chunk %d (track %d dts=%d) is written before chunk %d (track %d dts=%d)",
					i-1, a.track, a.dts, i, b.track, b.dts)
			}
			if la == rb && a.track > b.track {
				t.Fatalf("equal-dts chunks are ordered by track %d before %d", a.track, b.track)
			}
			if a.track == b.track && a.first > b.first {
				t.Fatalf("track %d chunks are out of decode order", a.track)
			}
		}
		// Chunk offsets must exactly tile the single mdat payload.
		atoms, err := mp4io.ReadFileAtoms(bytes.NewReader(out))
		if err != nil {
			t.Fatalf("parse output: %v", err)
		}
		pos := int64(0)
		for _, a := range atoms {
			if a.Tag() == mp4io.MDAT {
				off, _ := a.Pos()
				pos = int64(off) + 8
			}
		}
		if pos == 0 {
			t.Fatalf("no mdat in output")
		}
		for _, a := range list {
			if a.off != pos {
				t.Fatalf("chunk at physical position %d, want %d", a.off, pos)
			}
			ti := a.track
			for k := 0; k < a.count; k++ {
				pos += int64(len(fx.track[ti].samples[a.first+k].data))
			}
		}
		if pos != int64(len(out)) {
			t.Fatalf("chunks end at %d, file is %d bytes", pos, len(out))
		}
	})
}

// ---------------------------------------------------------------------------
// TestRechunkInvalid
// ---------------------------------------------------------------------------

// fxFindTop returns the offset and size of the first top-level box with tag.
func fxFindTop(t *testing.T, data []byte, tag string) (int, int) {
	t.Helper()
	for off := 0; off+8 <= len(data); {
		size := int(binary.BigEndian.Uint32(data[off : off+4]))
		if size < 8 || off+size > len(data) {
			break
		}
		if string(data[off+4:off+8]) == tag {
			return off, size
		}
		off += size
	}
	t.Fatalf("top-level box %q not found", tag)
	return 0, 0
}

func fxChild(t *testing.T, data []byte, boxOff, boxSize int, tag string, nth int) (int, int) {
	t.Helper()
	seen := 0
	for off := boxOff + 8; off+8 <= boxOff+boxSize; {
		size := int(binary.BigEndian.Uint32(data[off : off+4]))
		if size < 8 || off+size > boxOff+boxSize {
			break
		}
		if string(data[off+4:off+8]) == tag {
			if seen == nth {
				return off, size
			}
			seen++
		}
		off += size
	}
	t.Fatalf("child box %q[%d] not found", tag, nth)
	return 0, 0
}

// fxLocateStblTable finds stco/stsc inside the nth track.
func fxLocateStblTable(t *testing.T, data []byte, trackNth int, tag string) (int, int) {
	t.Helper()
	moovOff, moovSize := fxFindTop(t, data, "moov")
	trakOff, trakSize := fxChild(t, data, moovOff, moovSize, "trak", trackNth)
	mdiaOff, mdiaSize := fxChild(t, data, trakOff, trakSize, "mdia", 0)
	minfOff, minfSize := fxChild(t, data, mdiaOff, mdiaSize, "minf", 0)
	stblOff, stblSize := fxChild(t, data, minfOff, minfSize, "stbl", 0)
	return fxChild(t, data, stblOff, stblSize, tag, 0)
}

func fxExpectRechunkError(t *testing.T, file []byte, chunkDuration time.Duration, substr string) {
	t.Helper()
	var dst memWriteSeeker
	err := Rechunk(bytes.NewReader(file), &dst, chunkDuration)
	if err == nil {
		t.Fatalf("Rechunk succeeded, want error containing %q", substr)
	}
	if substr != "" && !bytes.Contains([]byte(err.Error()), []byte(substr)) {
		t.Fatalf("error = %q, want it to contain %q", err, substr)
	}
	if len(dst.buf) != 0 {
		t.Fatalf("dst was written despite error (%d bytes)", len(dst.buf))
	}
}

func TestRechunkInvalid(t *testing.T) {
	fx := fxLayoutFixture(t)

	t.Run("non-positive chunk duration", func(t *testing.T) {
		fxExpectRechunkError(t, fx.file, 0, "positive")
		fxExpectRechunkError(t, fx.file, -time.Second, "positive")
	})

	t.Run("truncated file", func(t *testing.T) {
		fxExpectRechunkError(t, fx.file[:len(fx.file)-5], time.Second, "")
	})

	t.Run("extended box length", func(t *testing.T) {
		bad := append([]byte{}, fx.file...)
		off, _ := fxFindTop(t, bad, "ftyp")
		binary.BigEndian.PutUint32(bad[off:off+4], 1)
		fxExpectRechunkError(t, bad, time.Second, "extended")
	})

	t.Run("unknown top-level box", func(t *testing.T) {
		bad := append([]byte{}, fx.file...)
		off, _ := fxFindTop(t, bad, "free")
		copy(bad[off+4:off+8], "skip")
		fxExpectRechunkError(t, bad, time.Second, "unsupported top-level")
	})

	t.Run("co64", func(t *testing.T) {
		bad := append([]byte{}, fx.file...)
		off, _ := fxLocateStblTable(t, bad, 0, "stco")
		copy(bad[off+4:off+8], "co64")
		fxExpectRechunkError(t, bad, time.Second, "co64")
	})

	t.Run("stsc does not start at chunk 1", func(t *testing.T) {
		bad := append([]byte{}, fx.file...)
		off, _ := fxLocateStblTable(t, bad, 0, "stsc")
		binary.BigEndian.PutUint32(bad[off+16:off+20], 2)
		fxExpectRechunkError(t, bad, time.Second, "chunk 1")
	})

	t.Run("stsc non-monotonic", func(t *testing.T) {
		bad := append([]byte{}, fx.file...)
		off, _ := fxLocateStblTable(t, bad, 0, "stsc")
		// Second entry begins at payload offset 8 + 12.
		binary.BigEndian.PutUint32(bad[off+28:off+32], 1)
		fxExpectRechunkError(t, bad, time.Second, "increasing")
	})

	t.Run("stsc sample count mismatch", func(t *testing.T) {
		bad := append([]byte{}, fx.file...)
		off, _ := fxLocateStblTable(t, bad, 0, "stsc")
		// First entry's samples per chunk: payload offset 8 + 4.
		binary.BigEndian.PutUint32(bad[off+20:off+24], 4)
		fxExpectRechunkError(t, bad, time.Second, "")
	})

	t.Run("sample range outside mdat", func(t *testing.T) {
		bad := append([]byte{}, fx.file...)
		off, _ := fxLocateStblTable(t, bad, 0, "stco")
		binary.BigEndian.PutUint32(bad[off+16:off+20], 0x7FFFFFFF)
		fxExpectRechunkError(t, bad, time.Second, "mdat")
	})

	t.Run("stsz sample count mismatch", func(t *testing.T) {
		bad := append([]byte{}, fx.file...)
		off, _ := fxLocateStblTable(t, bad, 0, "stsz")
		// stsz payload: version/flags(4), sample_size(4), count(4).
		binary.BigEndian.PutUint32(bad[off+16:off+20], 5)
		fxExpectRechunkError(t, bad, time.Second, "")
	})
}
