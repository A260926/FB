package mp4

import (
	"bytes"
	"io"
	"reflect"
	"testing"
	"time"

	"github.com/nareix/joy4/av"
	"github.com/nareix/joy4/codec/aacparser"
	"github.com/nareix/joy4/codec/h264parser"
	"github.com/nareix/joy4/format/mp4/mp4io"
	"github.com/nareix/joy4/utils/bits/pio"
)

// memFile is an in-memory io.ReadWriteSeeker, so tests never touch the disk.
type memFile struct {
	b   []byte
	pos int64
}

func (m *memFile) Write(p []byte) (int, error) {
	end := m.pos + int64(len(p))
	if end > int64(len(m.b)) {
		grown := make([]byte, end)
		copy(grown, m.b)
		m.b = grown
	}
	copy(m.b[m.pos:], p)
	m.pos = end
	return len(p), nil
}

func (m *memFile) Read(p []byte) (int, error) {
	if m.pos >= int64(len(m.b)) {
		return 0, io.EOF
	}
	n := copy(p, m.b[m.pos:])
	m.pos += int64(n)
	if n < len(p) {
		return n, io.ErrUnexpectedEOF
	}
	return n, nil
}

func (m *memFile) Seek(offset int64, whence int) (int64, error) {
	var p int64
	switch whence {
	case io.SeekStart:
		p = offset
	case io.SeekCurrent:
		p = m.pos + offset
	case io.SeekEnd:
		p = int64(len(m.b)) + offset
	}
	if p < 0 {
		return 0, io.EOF
	}
	m.pos = p
	return p, nil
}

func (m *memFile) ReadAt(p []byte, off int64) (int, error) {
	if off < 0 || off >= int64(len(m.b)) {
		return 0, io.EOF
	}
	return copy(p, m.b[off:]), nil
}

func (m *memFile) Len() int { return len(m.b) }

func newMemFile() *memFile { return &memFile{} }

// sampleSpec is one source sample: payload, decode duration in track units,
// composition offset in track units, and whether it is a sync sample.
type sampleSpec struct {
	size     int
	duration uint32
	ctts     uint32
	key      bool
}

// trackSpec describes one source track and its original chunk boundaries.
type trackSpec struct {
	isVideo   bool
	timeScale int32
	samples   []sampleSpec
	chunks    [][]int // source chunk -> sample indices, in decode order
}

// sampleData makes each sample's bytes recognizable and unique.
func sampleData(track, idx, size int) []byte {
	b := make([]byte, size)
	for k := range b {
		b[k] = byte(int(0xA0+track*16+idx) + k)
	}
	return b
}

func rawBox(tag mp4io.Tag, payload []byte) []byte {
	b := make([]byte, 8+len(payload))
	pio.PutU32BE(b[0:], uint32(len(b)))
	pio.PutU32BE(b[4:], uint32(tag))
	copy(b[8:], payload)
	return b
}

// testAACConfig is the clean MPEG4AudioConfig the fixture writes into mp4a.
var testAACConfig = []byte{0x12, 0x10}

// buildInput assembles a plain, non-fragmented MP4 with the given tracks:
// an ftyp, two mdats holding the source chunks, and a trailing moov. Tables
// are built explicitly so stsc run changes, multi-sample chunks and arbitrary
// absolute stco offsets are all exercised.
func buildInput(t *testing.T, tracks []trackSpec, mdatSplit int, tweakMovie func(*mp4io.Movie)) *memFile {
	t.Helper()

	// Decide the physical write order of source chunks across both mdats.
	type writeOp struct{ track, chunk int }
	var ops []writeOp
	cursors := make([]int, len(tracks))
	alive := true
	for alive {
		alive = false
		for ti := range tracks {
			if cursors[ti] < len(tracks[ti].chunks) {
				ops = append(ops, writeOp{ti, cursors[ti]})
				cursors[ti]++
				alive = true
			}
		}
	}
	if mdatSplit < 0 || mdatSplit > len(ops) {
		mdatSplit = len(ops)
	}

	// Lay payloads out and remember each source chunk's absolute offset.
	chunkOffset := make([][]uint32, len(tracks))
	var mdat1, mdat2 bytes.Buffer
	runPos := func(part int) int64 {
		if part == 0 {
			return int64(mdat1.Len())
		}
		return int64(mdat2.Len())
	}
	// ftyp size is fixed below; the second mdat starts after ftyp+mdat1 box.
	const ftypSize = 24
	for oi, op := range ops {
		ti, ci := op.track, op.chunk
		part := 0
		if oi >= mdatSplit {
			part = 1
		}
		var absolute int64
		if part == 0 {
			absolute = int64(ftypSize) + 8 + runPos(0)
		} else {
			absolute = int64(ftypSize) + 8 + int64(mdat1.Len()) + 8 + runPos(1)
		}
		if len(chunkOffset[ti]) <= ci {
			chunkOffset[ti] = append(chunkOffset[ti], uint32(absolute))
		}
		buf := &mdat1
		if part == 1 {
			buf = &mdat2
		}
		for _, si := range tracks[ti].chunks[ci] {
			buf.Write(sampleData(ti, si, tracks[ti].samples[si].size))
		}
	}

	movie := &mp4io.Movie{
		Header: &mp4io.MovieHeader{
			PreferredRate:   1,
			PreferredVolume: 1,
			Matrix:          [9]int32{0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000},
			TimeScale:       10000,
			NextTrackId:     int32(len(tracks) + 1),
		},
	}

	var maxDur int64
	for ti, tr := range tracks {
		stbl := &mp4io.SampleTable{
			SampleDesc:    &mp4io.SampleDesc{},
			TimeToSample:  &mp4io.TimeToSample{},
			SampleToChunk: &mp4io.SampleToChunk{},
			SampleSize:    &mp4io.SampleSize{},
			ChunkOffset:   &mp4io.ChunkOffset{Entries: chunkOffset[ti]},
		}

		// stts: merge adjacent equal durations into runs.
		for si, s := range tr.samples {
			if n := len(stbl.TimeToSample.Entries); n > 0 && stbl.TimeToSample.Entries[n-1].Duration == s.duration {
				stbl.TimeToSample.Entries[n-1].Count++
			} else {
				stbl.TimeToSample.Entries = append(stbl.TimeToSample.Entries,
					mp4io.TimeToSampleEntry{Count: 1, Duration: s.duration})
			}
			stbl.SampleSize.Entries = append(stbl.SampleSize.Entries, uint32(s.size))
			_ = si
		}

		// ctts: build runs from the per-sample offsets.
		var ctts []mp4io.CompositionOffsetEntry
		for _, s := range tr.samples {
			if n := len(ctts); n > 0 && ctts[n-1].Offset == s.ctts {
				ctts[n-1].Count++
			} else {
				ctts = append(ctts, mp4io.CompositionOffsetEntry{Count: 1, Offset: s.ctts})
			}
		}
		if len(ctts) > 0 {
			stbl.CompositionOffset = &mp4io.CompositionOffset{Entries: ctts}
		}

		// stsc: one run per change of samples-per-chunk.
		for ci, ch := range tr.chunks {
			n := uint32(len(ch))
			if e := len(stbl.SampleToChunk.Entries); e > 0 && stbl.SampleToChunk.Entries[e-1].SamplesPerChunk == n {
				continue
			}
			stbl.SampleToChunk.Entries = append(stbl.SampleToChunk.Entries,
				mp4io.SampleToChunkEntry{FirstChunk: uint32(ci + 1), SamplesPerChunk: n, SampleDescId: 1})
		}

		if tr.isVideo {
			codec, err := h264parser.NewCodecDataFromSPSAndPPS(
				[]byte{0x67, 0x42, 0x00, 0x0a, 0xfb, 0x88},
				[]byte{0x68, 0xce, 0x38, 0x80})
			if err != nil {
				t.Fatalf("h264 codec data: %v", err)
			}
			stbl.SampleDesc.AVC1Desc = &mp4io.AVC1Desc{
				DataRefIdx:           1,
				HorizontalResolution: 72,
				VorizontalResolution: 72,
				Width:                int16(codec.Width()),
				Height:               int16(codec.Height()),
				FrameCount:           1,
				Depth:                24,
				ColorTableId:         -1,
				Conf:                 &mp4io.AVC1Conf{Data: codec.AVCDecoderConfRecordBytes()},
			}
			var keys []uint32
			for si, s := range tr.samples {
				if s.key {
					keys = append(keys, uint32(si+1))
				}
			}
			if len(keys) > 0 {
				stbl.SyncSample = &mp4io.SyncSample{Entries: keys}
			}
		} else {
			codec, err := aacparser.NewCodecDataFromMPEG4AudioConfigBytes(append([]byte(nil), testAACConfig...))
			if err != nil {
				t.Fatalf("aac codec data: %v", err)
			}
			stbl.SampleDesc.MP4ADesc = &mp4io.MP4ADesc{
				DataRefIdx:       1,
				NumberOfChannels: int16(codec.ChannelLayout().Count()),
				SampleSize:       int16(codec.SampleFormat().BytesPerSample()),
				SampleRate:       float64(codec.SampleRate()),
				Conf:             &mp4io.ElemStreamDesc{DecConfig: codec.MPEG4AudioConfigBytes()},
			}
		}

		var trackDur int64
		for _, s := range tr.samples {
			trackDur += int64(s.duration)
		}
		if d := trackDur * 10000 / int64(tr.timeScale); d > maxDur {
			maxDur = d
		}

		track := &mp4io.Track{
			Header: &mp4io.TrackHeader{
				TrackId:  int32(ti + 1),
				Flags:    0x0003,
				Matrix:   [9]int32{0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000},
				Duration: int32(trackDur * 10000 / int64(tr.timeScale)),
			},
			Media: &mp4io.Media{
				Header: &mp4io.MediaHeader{
					TimeScale: tr.timeScale,
					Duration:  int32(trackDur),
					Language:  21956,
				},
				Handler: &mp4io.HandlerRefer{
					SubType: [4]byte{'s', 'o', 'u', 'n'},
					Name:    []byte("Sound Handler"),
				},
				Info: &mp4io.MediaInfo{
					Sound: &mp4io.SoundMediaInfo{Flags: 0x000001},
					Data: &mp4io.DataInfo{
						Refer: &mp4io.DataRefer{
							Url: &mp4io.DataReferUrl{Flags: 0x000001},
						},
					},
					Sample: stbl,
				},
			},
		}
		if tr.isVideo {
			track.Media.Handler.SubType = [4]byte{'v', 'i', 'd', 'e'}
			track.Media.Handler.Name = []byte("Video Media Handler")
			track.Media.Info.Sound = nil
			track.Media.Info.Video = &mp4io.VideoMediaInfo{Flags: 0x000001}
			track.Header.TrackWidth = 16
			track.Header.TrackHeight = 16
		}
		movie.Tracks = append(movie.Tracks, track)
	}
	movie.Header.Duration = int32(maxDur)

	if tweakMovie != nil {
		tweakMovie(movie)
	}

	out := newMemFile()
	// ftyp (24 bytes), then two mdats, then moov at the end.
	ftyp := make([]byte, ftypSize)
	pio.PutU32BE(ftyp[0:], ftypSize)
	copy(ftyp[4:], []byte("ftyp"))
	copy(ftyp[8:], []byte("isom"))
	pio.PutU32BE(ftyp[16:], 0x200)
	copy(ftyp[20:], []byte("isom"))
	out.Write(ftyp)

	out.Write(rawBox(mp4io.MDAT, mdat1.Bytes()))
	out.Write(rawBox(mp4io.MDAT, mdat2.Bytes()))
	moovBytes := make([]byte, movie.Len())
	movie.Marshal(moovBytes)
	out.Write(moovBytes)

	if _, err := out.Seek(0, io.SeekStart); err != nil {
		t.Fatalf("seek: %v", err)
	}
	return out
}

// mainFixture is the shared two-track, two-mdat, two-time-base input.
func mainFixture() []trackSpec {
	return []trackSpec{
		{
			isVideo:   true,
			timeScale: 1000,
			// 100ms frames, last sample has a zero duration but stays.
			samples: []sampleSpec{
				{size: 10, duration: 100, ctts: 0, key: true},
				{size: 7, duration: 100, ctts: 200},
				{size: 12, duration: 100, ctts: 100},
				{size: 5, duration: 100, ctts: 0, key: true},
				{size: 9, duration: 100, ctts: 200},
				{size: 6, duration: 0, ctts: 100},
			},
			// stsc runs: 2 samples, then 1, then 3.
			chunks: [][]int{{0, 1}, {2}, {3, 4, 5}},
		},
		{
			isVideo:   false,
			timeScale: 48000,
			samples: []sampleSpec{
				{size: 4, duration: 1024},
				{size: 6, duration: 1024},
				{size: 4, duration: 1024},
				{size: 8, duration: 1024},
				{size: 5, duration: 1024},
			},
			chunks: [][]int{{0}, {1}, {2}, {3}, {4}},
		},
	}
}

// scanTopBoxes splits a file into its raw top-level boxes without typed
// parsing, returning (tag, payload-inclusive raw bytes) for each.
func scanTopBoxes(t *testing.T, b []byte) []struct {
	tag mp4io.Tag
	raw []byte
} {
	t.Helper()
	var out []struct {
		tag mp4io.Tag
		raw []byte
	}
	pos := 0
	for pos+8 <= len(b) {
		size := int(pio.U32BE(b[pos:]))
		if size < 8 || pos+size > len(b) {
			t.Fatalf("bad top-level box at %d size %d", pos, size)
		}
		tag := mp4io.Tag(pio.U32BE(b[pos+4:]))
		out = append(out, struct {
			tag mp4io.Tag
			raw []byte
		}{tag, b[pos : pos+size]})
		pos += size
	}
	if pos != len(b) {
		t.Fatalf("trailing %d bytes after top-level boxes", len(b)-pos)
	}
	return out
}

// shiftStcoRaw adds delta to every entry of every stco atom found inside the
// raw moov bytes, editing the slice in place and avoiding any typed re-parse.
func shiftStcoRaw(t *testing.T, moovRaw []byte, delta uint32) {
	t.Helper()
	needle := []byte("stco")
	from := 0
	for {
		idx := bytes.Index(moovRaw[from:], needle)
		if idx < 0 {
			return
		}
		tagPos := from + idx
		atomStart := tagPos - 4
		if atomStart < 0 {
			t.Fatal("stco tag with no size header")
		}
		atomSize := int(pio.U32BE(moovRaw[atomStart:]))
		// header(8) + version/flags(4) + count(4), then count * uint32 entries.
		countPos := tagPos + 8
		if atomSize < 16 || countPos+4 > atomStart+atomSize {
			t.Fatal("truncated stco atom")
		}
		count := int(pio.U32BE(moovRaw[countPos:]))
		entryPos := countPos + 4
		if entryPos+4*count > atomStart+atomSize {
			t.Fatal("stco entries exceed atom size")
		}
		for i := 0; i < count; i++ {
			p := entryPos + 4*i
			pio.PutU32BE(moovRaw[p:], pio.U32BE(moovRaw[p:])+delta)
		}
		from = tagPos + 4
	}
}

// rebuildSource re-serializes a source, optionally inserting a leading free
// box and/or moving moov ahead of the mdats. The media block keeps its
// relative layout, so every stco entry shifts by exactly the inserted size.
// Boxes are handled as raw bytes (stco is patched in place) so the moov is not
// re-parsed and stays byte-identical apart from the offset shifts.
func rebuildSource(t *testing.T, src *memFile, freeSize int, moovFirst bool) *memFile {
	t.Helper()
	boxes := scanTopBoxes(t, append([]byte(nil), src.b...))
	var ftypRaw, moovRaw []byte
	var mdatRaw [][]byte
	for _, bx := range boxes {
		switch bx.tag {
		case boxFTYP:
			ftypRaw = bx.raw
		case mp4io.MOOV:
			moovRaw = bx.raw
		case mp4io.MDAT:
			mdatRaw = append(mdatRaw, bx.raw)
		}
	}
	if moovRaw == nil {
		t.Fatal("no moov")
	}

	shift := uint32(freeSize)
	if moovFirst {
		shift += uint32(len(moovRaw))
	}
	shiftStcoRaw(t, moovRaw, shift)

	out := newMemFile()
	if ftypRaw != nil {
		out.Write(ftypRaw)
	}
	if freeSize >= 8 {
		freeBox := make([]byte, freeSize)
		pio.PutU32BE(freeBox[0:], uint32(freeSize))
		copy(freeBox[4:], []byte("free"))
		out.Write(freeBox)
	}
	if moovFirst {
		out.Write(moovRaw)
	}
	for _, raw := range mdatRaw {
		out.Write(raw)
	}
	if !moovFirst {
		out.Write(moovRaw)
	}
	if _, err := out.Seek(0, io.SeekStart); err != nil {
		t.Fatal(err)
	}
	return out
}

func mustRechunk(t *testing.T, src *memFile, d time.Duration) *memFile {
	t.Helper()
	if _, err := src.Seek(0, io.SeekStart); err != nil {
		t.Fatalf("src seek: %v", err)
	}
	dst := newMemFile()
	if err := Rechunk(src, dst, d); err != nil {
		t.Fatalf("Rechunk: %v", err)
	}
	if _, err := dst.Seek(0, io.SeekStart); err != nil {
		t.Fatalf("dst seek: %v", err)
	}
	return dst
}

func parseOutput(t *testing.T, dst *memFile) ([]mp4io.Atom, *mp4io.Movie) {
	t.Helper()
	atoms, err := mp4io.ReadFileAtoms(dst)
	if err != nil {
		t.Fatalf("ReadFileAtoms: %v", err)
	}
	var movie *mp4io.Movie
	for _, a := range atoms {
		if m, ok := a.(*mp4io.Movie); ok {
			movie = m
		}
	}
	if movie == nil {
		t.Fatal("output has no moov")
	}
	return atoms, movie
}

// demuxAll reads packets through the original Demuxer until EOF. The legacy
// Demuxer stops once the earliest-DTS track runs out, so this is a prefix of
// the media, but it must be byte-for-byte identical before and after rechunk.
func demuxAll(t *testing.T, f *memFile) []av.Packet {
	t.Helper()
	if _, err := f.Seek(0, io.SeekStart); err != nil {
		t.Fatal(err)
	}
	d := NewDemuxer(f)
	if _, err := d.Streams(); err != nil {
		t.Fatalf("Streams: %v", err)
	}
	var pkts []av.Packet
	for {
		pkt, err := d.ReadPacket()
		if err == io.EOF {
			break
		}
		if err != nil {
			t.Fatalf("ReadPacket: %v", err)
		}
		pkts = append(pkts, pkt)
	}
	return pkts
}

func samePacket(a, b av.Packet) bool {
	return a.Idx == b.Idx && a.Time == b.Time &&
		a.CompositionTime == b.CompositionTime &&
		a.IsKeyFrame == b.IsKeyFrame && bytes.Equal(a.Data, b.Data)
}

// marshalAtom renders an atom back to its on-disk bytes (nil stays nil).
func marshalAtom(a mp4io.Atom) []byte {
	v := reflect.ValueOf(a)
	if !v.IsValid() || (v.Kind() == reflect.Ptr && v.IsNil()) {
		return nil
	}
	b := make([]byte, a.Len())
	a.Marshal(b)
	return b
}

// withoutFtyp rebuilds a source file with its leading ftyp removed. All stco
// entries are shifted down by the ftyp length so the mdats remain consistent.
func withoutFtyp(t *testing.T, ftypSize int, src *memFile) *memFile {
	t.Helper()
	if _, err := src.Seek(0, io.SeekStart); err != nil {
		t.Fatal(err)
	}
	atoms, err := mp4io.ReadFileAtoms(src)
	if err != nil {
		t.Fatal(err)
	}
	var movie *mp4io.Movie
	var mdatBounds [][2]int64
	for _, a := range atoms {
		switch x := a.(type) {
		case *mp4io.Movie:
			movie = x
		case *mp4io.Dummy:
			if x.Tag() == mp4io.MDAT {
				p, s := x.Pos()
				mdatBounds = append(mdatBounds, [2]int64{int64(p), int64(p) + int64(s)})
			}
		}
	}
	if movie == nil {
		t.Fatal("no moov")
	}
	shift := int64(ftypSize)
	for _, tr := range movie.Tracks {
		stco := tr.Media.Info.Sample.ChunkOffset.Entries
		for i := range stco {
			stco[i] = uint32(int64(stco[i]) - shift)
		}
	}
	out := newMemFile()
	for _, bd := range mdatBounds {
		if _, err := out.Write(src.b[bd[0]:bd[1]]); err != nil {
			t.Fatal(err)
		}
	}
	moovBytes := make([]byte, movie.Len())
	movie.Marshal(moovBytes)
	out.Write(moovBytes)
	if _, err := out.Seek(0, io.SeekStart); err != nil {
		t.Fatal(err)
	}
	return out
}

func TestRechunkLayout(t *testing.T) {
	src := buildInput(t, mainFixture(), 3, nil) // first three chunk writes in mdat #1
	dst := mustRechunk(t, src, 200*time.Millisecond)

	atoms, movie := parseOutput(t, dst)

	// Root layout: ftyp, moov, exactly one mdat, moov ahead of the data.
	if len(atoms) != 3 {
		t.Fatalf("root boxes = %d, want 3", len(atoms))
	}
	wantTags := []mp4io.Tag{boxFTYP, mp4io.MOOV, mp4io.MDAT}
	for i, want := range wantTags {
		if atoms[i].Tag() != want {
			t.Fatalf("root box %d = %s, want %s", i, atoms[i].Tag(), want)
		}
	}
	mdatPos, mdatSize := atoms[2].Pos()
	if _, moovEnd := atoms[1].Pos(); int64(mdatPos) < int64(moovEnd) {
		t.Fatal("moov is not ahead of mdat")
	}
	payloadStart := int64(mdatPos) + 8
	payloadEnd := int64(mdatPos) + int64(mdatSize)

	tracks := mainFixture()
	video := movie.Tracks[0].Media.Info.Sample
	audio := movie.Tracks[1].Media.Info.Sample

	// 200ms budget at 1000 units/s: three 2-sample video chunks; all five
	// 1024/48000 (~21.3ms) audio samples (~106.7ms total) fit in one chunk.
	if got := len(video.ChunkOffset.Entries); got != 3 {
		t.Fatalf("video chunks = %d, want 3", got)
	}
	if got := len(audio.ChunkOffset.Entries); got != 1 {
		t.Fatalf("audio chunks = %d, want 1", got)
	}
	wantVideoStsc := []mp4io.SampleToChunkEntry{{FirstChunk: 1, SamplesPerChunk: 2, SampleDescId: 1}}
	if !reflect.DeepEqual(video.SampleToChunk.Entries, wantVideoStsc) {
		t.Fatalf("video stsc = %+v, want %+v", video.SampleToChunk.Entries, wantVideoStsc)
	}
	wantAudioStsc := []mp4io.SampleToChunkEntry{{FirstChunk: 1, SamplesPerChunk: 5, SampleDescId: 1}}
	if !reflect.DeepEqual(audio.SampleToChunk.Entries, wantAudioStsc) {
		t.Fatalf("audio stsc = %+v, want %+v", audio.SampleToChunk.Entries, wantAudioStsc)
	}

	// Expected interleaving by first-sample DTS:
	//   V@0ms, A@0ms (tie -> video first), V@200ms, V@400ms.
	type expect struct {
		track       int
		sampleStart int
		sampleEnd   int
	}
	wantOrder := []expect{
		{0, 0, 2}, {1, 0, 5}, {0, 2, 4}, {0, 4, 6},
	}
	videoRanges := [][2]int{{0, 2}, {2, 4}, {4, 6}}

	// Walk physical chunks in mdat order: stco of the two tracks interleaves.
	type physical struct {
		track int
		off   uint32
		sizes []int
	}
	var phys []physical
	vi, ai := 0, 0
	for {
		vOff, aOff := int64(-1), int64(-1)
		if vi < len(video.ChunkOffset.Entries) {
			vOff = int64(video.ChunkOffset.Entries[vi])
		}
		if ai < len(audio.ChunkOffset.Entries) {
			aOff = int64(audio.ChunkOffset.Entries[ai])
		}
		if vOff < 0 && aOff < 0 {
			break
		}
		if aOff < 0 || (vOff >= 0 && vOff < aOff) {
			r := videoRanges[vi]
			var sizes []int
			for s := r[0]; s < r[1]; s++ {
				sizes = append(sizes, tracks[0].samples[s].size)
			}
			phys = append(phys, physical{0, video.ChunkOffset.Entries[vi], sizes})
			vi++
		} else {
			var sizes []int
			for s := 0; s < 5; s++ {
				sizes = append(sizes, tracks[1].samples[s].size)
			}
			phys = append(phys, physical{1, audio.ChunkOffset.Entries[ai], sizes})
			ai++
		}
	}
	if len(phys) != len(wantOrder) {
		t.Fatalf("physical chunk count = %d, want %d", len(phys), len(wantOrder))
	}

	// Every chunk must hold the exact sample bytes, sit inside the one mdat,
	// and sit immediately after the previous chunk (no source gap bytes).
	var cursor int64 = payloadStart
	for i, p := range phys {
		wo := wantOrder[i]
		if p.track != wo.track {
			t.Fatalf("chunk %d track = %d, want %d", i, p.track, wo.track)
		}
		if int64(p.off) != cursor {
			t.Fatalf("chunk %d offset %d, expected contiguous %d", i, p.off, cursor)
		}
		if int64(p.off) < payloadStart {
			t.Fatalf("chunk %d before mdat payload", i)
		}
		var chunkSize int64
		for _, n := range p.sizes {
			chunkSize += int64(n)
		}
		if int64(p.off)+chunkSize > payloadEnd {
			t.Fatalf("chunk %d runs past mdat end", i)
		}
		buf := make([]byte, chunkSize)
		if _, err := dst.ReadAt(buf, int64(p.off)); err != nil {
			t.Fatalf("read chunk %d: %v", i, err)
		}
		var want bytes.Buffer
		for s := wo.sampleStart; s < wo.sampleEnd; s++ {
			want.Write(sampleData(wo.track, s, tracks[wo.track].samples[s].size))
		}
		if !bytes.Equal(buf, want.Bytes()) {
			t.Fatalf("chunk %d payload mismatch\n got % x\nwant % x", i, buf, want.Bytes())
		}
		cursor += chunkSize
	}
	if cursor != payloadEnd {
		t.Fatalf("mdat payload end = %d, want %d (gap or trailing bytes)", payloadEnd, cursor)
	}

	// All samples, read one by one through stco+stsc+stsz, must match source.
	for ti, tr := range tracks {
		stbl := movie.Tracks[ti].Media.Info.Sample
		sampleIdx := 0
		runIdx := 0
		for ci, off := range stbl.ChunkOffset.Entries {
			if runIdx+1 < len(stbl.SampleToChunk.Entries) &&
				uint32(ci+1) == stbl.SampleToChunk.Entries[runIdx+1].FirstChunk && ci > 0 {
				runIdx++
			}
			n := int(stbl.SampleToChunk.Entries[runIdx].SamplesPerChunk)
			pos := int64(off)
			for j := 0; j < n; j++ {
				size := int(stbl.SampleSize.Entries[sampleIdx])
				if size != tr.samples[sampleIdx].size {
					t.Fatalf("track %d sample %d size changed", ti, sampleIdx)
				}
				got := make([]byte, size)
				if _, err := dst.ReadAt(got, pos); err != nil {
					t.Fatalf("read sample: %v", err)
				}
				want := sampleData(ti, sampleIdx, size)
				if !bytes.Equal(got, want) {
					t.Fatalf("track %d sample %d bytes mismatch", ti, sampleIdx)
				}
				pos += int64(size)
				sampleIdx++
			}
		}
		if sampleIdx != len(tr.samples) {
			t.Fatalf("track %d walked %d samples, want %d", ti, sampleIdx, len(tr.samples))
		}
	}

	// The original Demuxer must read the result. Rechunk is byte-preserving,
	// so the packet prefix it yields (it stops when the earliest-DTS track
	// ends) has to be identical, packet for packet, before and after.
	srcPkts := demuxAll(t, src)
	outPkts := demuxAll(t, dst)
	if len(srcPkts) == 0 {
		t.Fatal("source demuxer produced no packets")
	}
	if len(srcPkts) != len(outPkts) {
		t.Fatalf("demuxer packet count = %d, want %d", len(outPkts), len(srcPkts))
	}
	var lastTime time.Duration
	for i := range srcPkts {
		if !samePacket(srcPkts[i], outPkts[i]) {
			t.Fatalf("packet %d changed by rechunk:\n before %+v\n after  %+v", i, srcPkts[i], outPkts[i])
		}
		if i > 0 && outPkts[i].Time < lastTime {
			t.Fatalf("packet time went backwards at %d", i)
		}
		lastTime = outPkts[i].Time
	}
	// At least one packet from each track must be reachable in the prefix.
	seenTracks := map[int8]bool{}
	for _, p := range outPkts {
		seenTracks[p.Idx] = true
	}
	if len(seenTracks) != 2 {
		t.Fatalf("demuxer prefix only reached %d track(s)", len(seenTracks))
	}

	// Seeking needs a track that outlives the seek point, so check it on a
	// video-only file. The legacy Demuxer snaps back to the keyframe strictly
	// before 450ms, namely sample 3 (dts 300ms, stss entry 4).
	t.Run("Seek", func(t *testing.T) {
		videoOnly := []trackSpec{mainFixture()[0]}
		vSrc := buildInput(t, videoOnly, 2, nil)
		vDst := mustRechunk(t, vSrc, 200*time.Millisecond)
		if _, err := vDst.Seek(0, io.SeekStart); err != nil {
			t.Fatal(err)
		}
		d := NewDemuxer(vDst)
		if _, err := d.Streams(); err != nil {
			t.Fatalf("Streams: %v", err)
		}
		if err := d.SeekToTime(450 * time.Millisecond); err != nil {
			t.Fatalf("SeekToTime: %v", err)
		}
		var gotIdx []int
		for {
			pkt, err := d.ReadPacket()
			if err == io.EOF {
				break
			}
			if err != nil {
				t.Fatalf("ReadPacket after seek: %v", err)
			}
			// Identify the sample by its unique first byte via size+content.
			for si, s := range videoOnly[0].samples {
				if bytes.Equal(pkt.Data, sampleData(0, si, s.size)) {
					gotIdx = append(gotIdx, si)
					break
				}
			}
		}
		wantIdx := []int{3, 4, 5}
		if !reflect.DeepEqual(gotIdx, wantIdx) {
			t.Fatalf("post-seek samples = %v, want %v", gotIdx, wantIdx)
		}
	})

	// Edge cases on small single-track inputs.
	t.Run("ExactBudget", func(t *testing.T) { testExactBudget(t) })
	t.Run("OversizedSample", func(t *testing.T) { testOversizedSample(t) })
	t.Run("TrailingZeroDuration", func(t *testing.T) { testTrailingZero(t) })

	// moov must lead even when the source has no ftyp at all.
	t.Run("NoFtypMoovFirst", func(t *testing.T) {
		nf := withoutFtyp(t, 24, buildInput(t, mainFixture(), 3, nil))
		od := mustRechunk(t, nf, 200*time.Millisecond)
		root, _ := parseOutput(t, od)
		if len(root) != 2 || root[0].Tag() != mp4io.MOOV || root[1].Tag() != mp4io.MDAT {
			var tags []string
			for _, a := range root {
				tags = append(tags, a.Tag().String())
			}
			t.Fatalf("root boxes = %v, want [moov mdat]", tags)
		}
		// The original Demuxer must still read the moov-first source and output.
		if pkts := demuxAll(t, nf); len(pkts) == 0 {
			t.Fatal("demuxer produced no packets for moov-first source")
		}
		if pkts := demuxAll(t, od); len(pkts) == 0 {
			t.Fatal("demuxer produced no packets for moov-first file")
		}
	})

	// A moov ahead of the media, and an intervening free box, must be accepted
	// and produce the same fast-start output as the standard trailing-moov file.
	t.Run("MoovFirstAndFree", func(t *testing.T) {
		base := buildInput(t, mainFixture(), 3, nil)
		want := mustRechunk(t, base, 200*time.Millisecond)

		first := rebuildSource(t, base, 16, true) // leading free + moov-first
		gotFirst := mustRechunk(t, first, 200*time.Millisecond)
		if !bytes.Equal(gotFirst.b, want.b) {
			t.Fatal("moov-first/free rechunk differs from standard layout")
		}

		withFree := rebuildSource(t, base, 24, false) // free inserted, moov still last
		gotFree := mustRechunk(t, withFree, 200*time.Millisecond)
		if !bytes.Equal(gotFree.b, want.b) {
			t.Fatal("free-box rechunk differs from standard layout")
		}
	})

	// An ftyp, when present, must be reproduced verbatim at the front.
	t.Run("FtypPreserved", func(t *testing.T) {
		sf := buildInput(t, mainFixture(), 3, nil)
		sfAtoms, _ := parseOutput(t, sf)
		var srcFtyp []byte
		for _, a := range sfAtoms {
			if d, ok := a.(*mp4io.Dummy); ok && d.Tag() == boxFTYP {
				p, s := d.Pos()
				srcFtyp = make([]byte, s)
				if _, err := sf.ReadAt(srcFtyp, int64(p)); err != nil {
					t.Fatal(err)
				}
			}
		}
		if len(srcFtyp) == 0 {
			t.Fatal("source ftyp not found")
		}
		od := mustRechunk(t, sf, 200*time.Millisecond)
		if _, err := od.Seek(0, io.SeekStart); err != nil {
			t.Fatal(err)
		}
		got := make([]byte, len(srcFtyp))
		if _, err := io.ReadFull(od, got); err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(got, srcFtyp) {
			t.Fatalf("ftyp not preserved\n got % x\nwant % x", got, srcFtyp)
		}
	})
}

func testExactBudget(t *testing.T) {
	// 50ms frames at a 1000-unit base; 100ms budget is reached exactly.
	spec := []trackSpec{{
		isVideo:   true,
		timeScale: 1000,
		samples: []sampleSpec{
			{size: 3, duration: 50, key: true},
			{size: 4, duration: 50},
			{size: 5, duration: 50},
		},
		chunks: [][]int{{0}, {1}, {2}},
	}}
	src := buildInput(t, spec, 2, nil)
	dst := mustRechunk(t, src, 100*time.Millisecond)
	_, movie := parseOutput(t, dst)
	stbl := movie.Tracks[0].Media.Info.Sample
	want := []mp4io.SampleToChunkEntry{
		{FirstChunk: 1, SamplesPerChunk: 2, SampleDescId: 1},
		{FirstChunk: 2, SamplesPerChunk: 1, SampleDescId: 1},
	}
	if !reflect.DeepEqual(stbl.SampleToChunk.Entries, want) {
		t.Fatalf("stsc = %+v, want %+v", stbl.SampleToChunk.Entries, want)
	}
	if len(stbl.ChunkOffset.Entries) != 2 {
		t.Fatalf("chunks = %d, want 2", len(stbl.ChunkOffset.Entries))
	}
}

func testOversizedSample(t *testing.T) {
	// First sample alone lasts longer than the whole chunk budget.
	spec := []trackSpec{{
		isVideo:   true,
		timeScale: 1000,
		samples: []sampleSpec{
			{size: 8, duration: 300, key: true},
			{size: 2, duration: 50},
		},
		chunks: [][]int{{0}, {1}},
	}}
	src := buildInput(t, spec, 1, nil)
	dst := mustRechunk(t, src, 100*time.Millisecond)
	_, movie := parseOutput(t, dst)
	stbl := movie.Tracks[0].Media.Info.Sample
	// Both chunks hold one sample, so the adjacent equal runs coalesce into a
	// single stsc run while two distinct chunk offsets remain.
	want := []mp4io.SampleToChunkEntry{
		{FirstChunk: 1, SamplesPerChunk: 1, SampleDescId: 1},
	}
	if !reflect.DeepEqual(stbl.SampleToChunk.Entries, want) {
		t.Fatalf("stsc = %+v, want %+v", stbl.SampleToChunk.Entries, want)
	}
	if len(stbl.ChunkOffset.Entries) != 2 {
		t.Fatalf("chunks = %d, want 2", len(stbl.ChunkOffset.Entries))
	}
	// The oversized sample must be copied intact.
	off := stbl.ChunkOffset.Entries[0]
	got := make([]byte, 8)
	if _, err := dst.ReadAt(got, int64(off)); err != nil {
		t.Fatal(err)
	}
	if want := sampleData(0, 0, 8); !bytes.Equal(got, want) {
		t.Fatalf("oversized sample bytes = % x, want % x", got, want)
	}
}

func testTrailingZero(t *testing.T) {
	// Zero-duration final sample stays attached to the preceding chunk.
	spec := []trackSpec{{
		isVideo:   true,
		timeScale: 1000,
		samples: []sampleSpec{
			{size: 3, duration: 100, key: true},
			{size: 4, duration: 0},
		},
		chunks: [][]int{{0}, {1}},
	}}
	src := buildInput(t, spec, 1, nil)
	dst := mustRechunk(t, src, 100*time.Millisecond)
	_, movie := parseOutput(t, dst)
	stbl := movie.Tracks[0].Media.Info.Sample
	if len(stbl.ChunkOffset.Entries) != 1 {
		t.Fatalf("chunks = %d, want 1", len(stbl.ChunkOffset.Entries))
	}
	want := []mp4io.SampleToChunkEntry{{FirstChunk: 1, SamplesPerChunk: 2, SampleDescId: 1}}
	if !reflect.DeepEqual(stbl.SampleToChunk.Entries, want) {
		t.Fatalf("stsc = %+v, want %+v", stbl.SampleToChunk.Entries, want)
	}
	if got := len(stbl.SampleSize.Entries); got != 2 {
		t.Fatalf("stsz entries = %d, want 2", got)
	}
	off := stbl.ChunkOffset.Entries[0]
	got := make([]byte, 7)
	if _, err := dst.ReadAt(got, int64(off)); err != nil {
		t.Fatal(err)
	}
	var wantBuf bytes.Buffer
	wantBuf.Write(sampleData(0, 0, 3))
	wantBuf.Write(sampleData(0, 1, 4))
	if !bytes.Equal(got, wantBuf.Bytes()) {
		t.Fatalf("zero-tail chunk = % x, want % x", got, wantBuf.Bytes())
	}
}

func TestRechunkTables(t *testing.T) {
	specs := mainFixture()
	src := buildInput(t, specs, 3, nil)

	// Snapshot the on-disk bytes of every atom rechunk must leave untouched.
	// Marshaled bytes are compared, so time.Time locations cannot create noise.
	_, before := parseOutput(t, src)
	type trackSnapshot struct {
		tkhd    []byte
		mdhd    []byte
		stsd    []byte
		isVideo bool
		// For mp4a the repo's esds Unmarshal appends trailing descriptor bytes
		// to DecConfig, so its stsd is not marshal-idempotent even without
		// rechunk. Compare the codec identity semantically instead.
		aacConfig []byte
		stsz      []byte
		stts      []byte
		ctts      []byte
		stss      []byte
	}
	snaps := make([]trackSnapshot, len(before.Tracks))
	for ti, tr := range before.Tracks {
		stbl := tr.Media.Info.Sample
		s := trackSnapshot{
			tkhd:    marshalAtom(tr.Header),
			mdhd:    marshalAtom(tr.Media.Header),
			stsd:    marshalAtom(stbl.SampleDesc),
			isVideo: stbl.SampleDesc.AVC1Desc != nil,
			stsz:    marshalAtom(stbl.SampleSize),
			stts:    marshalAtom(stbl.TimeToSample),
			ctts:    marshalAtom(stbl.CompositionOffset),
			stss:    marshalAtom(stbl.SyncSample),
		}
		if d := stbl.SampleDesc.MP4ADesc; d != nil {
			// Snapshot the clean, pre-parse config rather than the parsed
			// DecConfig, which the esds parser extends with trailing bytes.
			s.aacConfig = append([]byte(nil), testAACConfig...)
		}
		snaps[ti] = s
	}
	mvhdBefore := marshalAtom(before.Header)

	dst := mustRechunk(t, src, 150*time.Millisecond)
	_, after := parseOutput(t, dst)

	if got := marshalAtom(after.Header); !bytes.Equal(got, mvhdBefore) {
		t.Fatalf("mvhd bytes changed by rechunk\n before % x\n after  % x", mvhdBefore, got)
	}
	for ti, tr := range after.Tracks {
		stbl := tr.Media.Info.Sample
		want := snaps[ti]
		if got := marshalAtom(tr.Header); !bytes.Equal(got, want.tkhd) {
			t.Fatalf("track %d tkhd changed", ti)
		}
		if got := marshalAtom(tr.Media.Header); !bytes.Equal(got, want.mdhd) {
			t.Fatalf("track %d mdhd changed", ti)
		}
		if want.isVideo {
			// avc1/avcC stores raw decoder-config bytes and round-trips exactly.
			if got := marshalAtom(stbl.SampleDesc); !bytes.Equal(got, want.stsd) {
				t.Fatalf("track %d avc1 stsd changed", ti)
			}
		} else {
			// The original MPEG4AudioConfig prefix must survive and still parse
			// to the same codec, despite the esds parser's trailing-byte quirk.
			got := stbl.SampleDesc.MP4ADesc.Conf.DecConfig
			if !bytes.HasPrefix(got, want.aacConfig) {
				t.Fatalf("track %d aac config prefix changed: got % x want prefix % x", ti, got, want.aacConfig)
			}
			c0, err0 := aacparser.NewCodecDataFromMPEG4AudioConfigBytes(want.aacConfig)
			c1, err1 := aacparser.NewCodecDataFromMPEG4AudioConfigBytes(got)
			if err0 != nil || err1 != nil ||
				c0.SampleRate() != c1.SampleRate() ||
				c0.ChannelLayout() != c1.ChannelLayout() {
				t.Fatalf("track %d aac codec identity changed", ti)
			}
		}
		if got := marshalAtom(stbl.SampleSize); !bytes.Equal(got, want.stsz) {
			t.Fatalf("track %d stsz changed", ti)
		}
		if got := marshalAtom(stbl.TimeToSample); !bytes.Equal(got, want.stts) {
			t.Fatalf("track %d stts changed", ti)
		}
		if got := marshalAtom(stbl.CompositionOffset); !bytes.Equal(got, want.ctts) {
			t.Fatalf("track %d ctts changed (nil-ness or bytes)", ti)
		}
		if got := marshalAtom(stbl.SyncSample); !bytes.Equal(got, want.stss) {
			t.Fatalf("track %d stss changed (nil-ness or bytes)", ti)
		}
	}

	// stsc runs must be coalesced and every stco entry must be valid, unique
	// per track, 32-bit, and point at contiguous payload inside the mdat.
	if _, err := dst.Seek(0, io.SeekStart); err != nil {
		t.Fatal(err)
	}
	atoms, _ := parseOutput(t, dst)
	var mdat *mp4io.Dummy
	for _, a := range atoms {
		if d, ok := a.(*mp4io.Dummy); ok && d.Tag() == mp4io.MDAT {
			mdat = d
		}
	}
	if mdat == nil {
		t.Fatal("no mdat in output")
	}
	mPos, mSize := mdat.Pos()
	for ti, tr := range after.Tracks {
		stbl := tr.Media.Info.Sample
		entries := stbl.SampleToChunk.Entries
		for i := 1; i < len(entries); i++ {
			if entries[i].FirstChunk <= entries[i-1].FirstChunk {
				t.Fatalf("track %d stsc not increasing", ti)
			}
			if entries[i].SamplesPerChunk == entries[i-1].SamplesPerChunk {
				t.Fatalf("track %d stsc runs not coalesced at %d", ti, i)
			}
			if entries[i].SampleDescId != 1 {
				t.Fatalf("track %d stsc desc id = %d, want 1", ti, entries[i].SampleDescId)
			}
		}
		seen := map[uint32]bool{}
		for _, off := range stbl.ChunkOffset.Entries {
			if seen[off] {
				t.Fatalf("track %d duplicate stco %d", ti, off)
			}
			seen[off] = true
			if int64(off) < int64(mPos)+8 || int64(off) >= int64(mPos)+int64(mSize) {
				t.Fatalf("track %d offset %d outside mdat payload", ti, off)
			}
		}
	}

	// Cross-track tie: both tracks start at DTS 0, trak order must put video first.
	vOff := after.Tracks[0].Media.Info.Sample.ChunkOffset.Entries[0]
	aOff := after.Tracks[1].Media.Info.Sample.ChunkOffset.Entries[0]
	if !(vOff < aOff) {
		t.Fatalf("DTS tie not resolved by trak order: video %d audio %d", vOff, aOff)
	}
}

func TestRechunkInvalid(t *testing.T) {
	// Every failure must leave dst completely untouched.
	assertRejected := func(t *testing.T, src *memFile, d time.Duration) {
		t.Helper()
		dst := newMemFile()
		err := Rechunk(src, dst, d)
		if err == nil {
			t.Fatal("expected error, got nil")
		}
		if dst.Len() != 0 {
			t.Fatalf("dst was written on error: %d bytes", dst.Len())
		}
	}

	t.Run("NonPositiveDuration", func(t *testing.T) {
		src := buildInput(t, mainFixture(), 3, nil)
		assertRejected(t, src, 0)
		assertRejected(t, src, -time.Millisecond)
	})

	t.Run("BadRange", func(t *testing.T) {
		src := buildInput(t, mainFixture(), 3, func(m *mp4io.Movie) {
			// Point the first video chunk at the ftyp area: outside any mdat.
			m.Tracks[0].Media.Info.Sample.ChunkOffset.Entries[0] = 4
		})
		assertRejected(t, src, 150*time.Millisecond)
	})

	t.Run("UnknownTopLevelBox", func(t *testing.T) {
		src := buildInput(t, mainFixture(), 3, nil)
		raw := append(append([]byte(nil), src.b...), rawBox(mp4io.Tag(0x736b6970), []byte("x"))...) // 'skip'
		assertRejected(t, &memFile{b: raw}, 150*time.Millisecond)
	})

	t.Run("Co64", func(t *testing.T) {
		src := buildInput(t, mainFixture(), 3, nil)
		raw := append([]byte(nil), src.b...)
		idx := bytes.Index(raw, []byte("stco"))
		if idx < 0 {
			t.Fatal("stco not found")
		}
		copy(raw[idx:], []byte("co64"))
		assertRejected(t, &memFile{b: raw}, 150*time.Millisecond)
	})

	t.Run("ExtendedLength", func(t *testing.T) {
		raw := make([]byte, 16)
		pio.PutU32BE(raw[0:], 1) // size 1 => 64-bit largesize follows
		copy(raw[4:], []byte("ftyp"))
		assertRejected(t, &memFile{b: raw}, 150*time.Millisecond)
	})

	t.Run("Truncated", func(t *testing.T) {
		src := buildInput(t, mainFixture(), 3, nil)
		raw := append([]byte(nil), src.b[:len(src.b)-12]...)
		assertRejected(t, &memFile{b: raw}, 150*time.Millisecond)
	})

	t.Run("Fragmented", func(t *testing.T) {
		src := buildInput(t, mainFixture(), 3, func(m *mp4io.Movie) {
			m.MovieExtend = &mp4io.MovieExtend{}
		})
		assertRejected(t, src, 150*time.Millisecond)
	})

	t.Run("EditList", func(t *testing.T) {
		src := buildInput(t, mainFixture(), 3, func(m *mp4io.Movie) {
			// A well-formed edts box (header plus an elst child). A second box
			// follows so edts is not the exact-trailing child this parser drops.
			edts := make([]byte, 16)
			pio.PutU32BE(edts[0:], 16)
			copy(edts[4:], []byte("edts"))
			copy(edts[8:], []byte("elst"))
			trailing := make([]byte, 8)
			pio.PutU32BE(trailing[0:], 8)
			copy(trailing[4:], []byte("free"))
			m.Tracks[0].Unknowns = append(m.Tracks[0].Unknowns,
				&mp4io.Dummy{Tag_: boxEDTS, Data: edts},
				&mp4io.Dummy{Tag_: boxFREE, Data: trailing})
		})
		assertRejected(t, src, 150*time.Millisecond)
	})

	t.Run("StscNotFromChunkOne", func(t *testing.T) {
		src := buildInput(t, mainFixture(), 3, func(m *mp4io.Movie) {
			m.Tracks[0].Media.Info.Sample.SampleToChunk.Entries[0].FirstChunk = 2
		})
		assertRejected(t, src, 150*time.Millisecond)
	})

	t.Run("StscNotMonotonic", func(t *testing.T) {
		src := buildInput(t, mainFixture(), 3, func(m *mp4io.Movie) {
			e := m.Tracks[0].Media.Info.Sample.SampleToChunk.Entries
			e[1].FirstChunk, e[2].FirstChunk = e[2].FirstChunk, e[1].FirstChunk
		})
		assertRejected(t, src, 150*time.Millisecond)
	})

	t.Run("StscCoverageMismatch", func(t *testing.T) {
		src := buildInput(t, mainFixture(), 3, func(m *mp4io.Movie) {
			// Last run claims one fewer sample per chunk, so coverage < stsz.
			m.Tracks[0].Media.Info.Sample.SampleToChunk.Entries[2].SamplesPerChunk = 2
		})
		assertRejected(t, src, 150*time.Millisecond)
	})

	t.Run("UniformStsz", func(t *testing.T) {
		src := buildInput(t, mainFixture(), 3, func(m *mp4io.Movie) {
			stbl := m.Tracks[1].Media.Info.Sample
			stbl.SampleSize.SampleSize = 4
			stbl.SampleSize.Entries = nil
		})
		assertRejected(t, src, 150*time.Millisecond)
	})

	t.Run("TwoSampleDescriptions", func(t *testing.T) {
		src := buildInput(t, mainFixture(), 3, func(m *mp4io.Movie) {
			stbl := m.Tracks[1].Media.Info.Sample
			// A second, non-avc1/mp4a description entry with a valid header,
			// plus a trailing box so the entry is not dropped at the boundary.
			extra := make([]byte, 8)
			pio.PutU32BE(extra[0:], 8)
			copy(extra[4:], []byte("xxxx"))
			trailing := make([]byte, 8)
			pio.PutU32BE(trailing[0:], 8)
			copy(trailing[4:], []byte("yyyy"))
			stbl.SampleDesc.Unknowns = append(stbl.SampleDesc.Unknowns,
				&mp4io.Dummy{Tag_: mp4io.Tag(0x78787878), Data: extra},
				&mp4io.Dummy{Tag_: mp4io.Tag(0x79797979), Data: trailing})
		})
		assertRejected(t, src, 150*time.Millisecond)
	})
}
