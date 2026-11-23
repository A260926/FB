// edit_compact_test.go exercises mp4.CompactEditedFile through three task
// groups: TestCompactReachability, TestCompactRoundTrip and
// TestCompactReject. The dual-track inputs are assembled independently in
// this file, with byte markers in every sample payload so that the identity
// of each sample can be checked after re-demuxing the compacted output.

package mp4_test

import (
	"bytes"
	"crypto/sha256"
	"fmt"
	"os"
	"sort"
	"testing"

	"github.com/Eyevinn/mp4ff/mp4"
	"github.com/Eyevinn/mp4ff/task_support/taskcheck"
)

// ---------------------------------------------------------------------------
// MP4 assembly for tests
// ---------------------------------------------------------------------------

// sampleSpec describes one media sample. The payload is generated from the
// sample number, so its identity can be verified after re-demuxing.
type sampleSpec struct {
	dur  uint32
	cto  int32
	sync bool
	size int
}

// chunkSpec describes one source chunk: count consecutive samples that share
// one sample description.
type chunkSpec struct {
	count int
	sdid  uint32
}

// trackSpec describes one track to assemble.
type trackSpec struct {
	handler   string // "vide" or "soun"
	timescale uint32
	trackID   uint32
	samples   []sampleSpec
	chunks    []chunkSpec
	nrStsd    int
	hasElst   bool
	elstVer   byte
	entries   []mp4.ElstEntry
	mdhdVer   byte
	tkhdVer   byte
	tkhdDur   uint64
	width     uint32
	height    uint32
	volume    uint16

	// corruption knobs for rejection tests
	sttsShort   bool // stts covers one sample less than stsz declares
	cttsShort   bool // ctts covers one sample less than stsz declares
	stscExtra   bool // stsc refers to a chunk beyond the stco table
	stssBeyond  bool // stss refers to a sample beyond the stsz count
	sdidBeyond  bool // first stsc entry refers to a missing sample description
	addSdtp     bool // add an sdtp per-sample table to stbl
	externalRef bool // reference sample data through an external url entry
	emptyStss   bool // include an stss box with no entries
	useCo64     bool // write the chunk offsets as a co64 box
}

// fileSpec describes one MP4 file to assemble.
type fileSpec struct {
	movieTimescale uint32
	movieDuration  uint64
	movieVersion   byte
	tracks         []*trackSpec
	extraMdat      bool // append a second, empty mdat box
	addMoof        bool // append an empty moof box, making the file fragmented
}

// sampleTag returns the payload marker tag for a handler type.
func sampleTag(handler string) byte {
	if handler == "vide" {
		return 'V'
	}
	return 'A'
}

// markerBytes returns the deterministic payload of sample nr (1-based) of
// the track with the given tag: a readable header plus a fill pattern.
func markerBytes(tag byte, nr int, size int) []byte {
	b := make([]byte, size)
	hdr := fmt.Sprintf("%c%05d|", tag, nr)
	copy(b, hdr)
	for i := len(hdr); i < size; i++ {
		b[i] = byte(nr*7 + i*3)
	}
	return b
}

// markerOK reports whether data is exactly the marker payload of sample nr.
func markerOK(data []byte, tag byte, nr int) bool {
	return bytes.Equal(data, markerBytes(tag, nr, len(data)))
}

// markerID decodes the (tag, nr) identity of a marker payload.
func markerID(data []byte) (byte, int, bool) {
	if len(data) < 7 || data[6] != '|' {
		return 0, 0, false
	}
	nr := 0
	for i := 1; i <= 5; i++ {
		if data[i] < '0' || data[i] > '9' {
			return 0, 0, false
		}
		nr = nr*10 + int(data[i]-'0')
	}
	if !markerOK(data, data[0], nr) {
		return 0, 0, false
	}
	return data[0], nr, true
}

// mediaEntry is a rate-1 media segment for an edit list.
func mediaEntry(segDur uint64, mediaTime int64) mp4.ElstEntry {
	return mp4.ElstEntry{SegmentDuration: segDur, MediaTime: mediaTime, MediaRateInteger: 1}
}

// emptyEntry is an empty (media_time -1) segment for an edit list.
func emptyEntry(segDur uint64) mp4.ElstEntry {
	return mp4.ElstEntry{SegmentDuration: segDur, MediaTime: -1, MediaRateInteger: 1}
}

// buildFile assembles a progressive MP4: ftyp, then one mdat with the track
// chunks interleaved round-robin (so each track's chunks are non-contiguous),
// then the moov.
func buildFile(spec *fileSpec) []byte {
	ftyp := mp4.NewFtyp("isom", 1, []string{"isom", "iso6", "mp41"})

	// Per track: sample payloads, chunk payloads, tables.
	type built struct {
		trak       *mp4.TrakBox
		chunkDatas [][]byte
	}
	builts := make([]*built, len(spec.tracks))
	for ti, ts := range spec.tracks {
		tag := sampleTag(ts.handler)
		// Sample payloads.
		datas := make([][]byte, len(ts.samples))
		for i, s := range ts.samples {
			datas[i] = markerBytes(tag, i+1, s.size)
		}
		// Chunk payloads.
		var chunks []chunkSpec
		chunks = ts.chunks
		var chunkDatas [][]byte
		sampleIdx := 0
		for _, ch := range chunks {
			var data []byte
			for k := 0; k < ch.count; k++ {
				data = append(data, datas[sampleIdx]...)
				sampleIdx++
			}
			chunkDatas = append(chunkDatas, data)
		}
		if sampleIdx != len(ts.samples) {
			panic("chunk specs do not cover the samples")
		}
		builts[ti] = &built{chunkDatas: chunkDatas}
		builts[ti].trak = buildTrak(ts, datas)
	}

	// Interleave chunk payloads round-robin and record chunk offsets.
	mdatPayloadStart := ftyp.Size() + 8 // 8 = mdat header
	var payload []byte
	chunkOffsets := make([][]uint32, len(spec.tracks))
	off := mdatPayloadStart
	for round := 0; ; round++ {
		emitted := false
		for ti := range spec.tracks {
			if round < len(builts[ti].chunkDatas) {
				data := builts[ti].chunkDatas[round]
				chunkOffsets[ti] = append(chunkOffsets[ti], uint32(off))
				payload = append(payload, data...)
				off += uint64(len(data))
				emitted = true
			}
		}
		if !emitted {
			break
		}
	}
	for ti := range spec.tracks {
		stbl := builts[ti].trak.Mdia.Minf.Stbl
		if spec.tracks[ti].useCo64 {
			co64 := &mp4.Co64Box{}
			for _, o := range chunkOffsets[ti] {
				co64.ChunkOffset = append(co64.ChunkOffset, uint64(o))
			}
			stbl.AddChild(co64)
		} else {
			stbl.AddChild(&mp4.StcoBox{ChunkOffset: chunkOffsets[ti]})
		}
	}

	moov := &mp4.MoovBox{}
	moov.AddChild(&mp4.MvhdBox{
		Version:   spec.movieVersion,
		Timescale: spec.movieTimescale,
		Duration:  spec.movieDuration,
		Rate:      0x00010000,
		Volume:    0x0100,
		Matrix:    mp4.UnityMatrix(),
	})
	for _, b := range builts {
		moov.AddChild(b.trak)
	}

	var buf bytes.Buffer
	if err := ftyp.Encode(&buf); err != nil {
		panic(err)
	}
	mdat := &mp4.MdatBox{}
	mdat.AddSampleData(payload)
	if err := mdat.Encode(&buf); err != nil {
		panic(err)
	}
	if err := moov.Encode(&buf); err != nil {
		panic(err)
	}
	if spec.extraMdat {
		if err := (&mp4.MdatBox{}).Encode(&buf); err != nil {
			panic(err)
		}
	}
	if spec.addMoof {
		if err := (&mp4.MoofBox{}).Encode(&buf); err != nil {
			panic(err)
		}
	}
	return buf.Bytes()
}

// buildTrak assembles one trak box (without stco, which the caller adds once
// the chunk offsets are known).
func buildTrak(ts *trackSpec, datas [][]byte) *mp4.TrakBox {
	n := uint32(len(ts.samples))

	tkhd := &mp4.TkhdBox{
		Version:  ts.tkhdVer,
		Flags:    0x000007,
		TrackID:  ts.trackID,
		Duration: ts.tkhdDur,
		Matrix:   mp4.UnityMatrix(),
		Width:    mp4.Fixed32(ts.width << 16),
		Height:   mp4.Fixed32(ts.height << 16),
		Volume:   mp4.Fixed16(ts.volume),
	}

	var totalDur uint64
	for _, s := range ts.samples {
		totalDur += uint64(s.dur)
	}
	mdhd := &mp4.MdhdBox{Version: ts.mdhdVer, Timescale: ts.timescale, Duration: totalDur}
	hdlr := &mp4.HdlrBox{HandlerType: ts.handler, Name: "test"}

	stsd := mp4.NewStsdBox()
	for i := 0; i < ts.nrStsd; i++ {
		if ts.handler == "vide" {
			stsd.AddChild(mp4.CreateVisualSampleEntryBox("avc1", 320, 240, nil))
		} else {
			stsd.AddChild(mp4.CreateAudioSampleEntryBox("mp4a", 2, 16, 48000, nil))
		}
	}

	// stts: run-length encoded durations.
	var sttsCounts, sttsDeltas []uint32
	for _, s := range ts.samples {
		if len(sttsDeltas) > 0 && s.dur == sttsDeltas[len(sttsDeltas)-1] {
			sttsCounts[len(sttsCounts)-1]++
		} else {
			sttsCounts = append(sttsCounts, 1)
			sttsDeltas = append(sttsDeltas, s.dur)
		}
	}
	if ts.sttsShort && len(sttsCounts) > 0 {
		sttsCounts[len(sttsCounts)-1]--
	}
	stts := &mp4.SttsBox{SampleCount: sttsCounts, SampleTimeDelta: sttsDeltas}

	// ctts: present when any sample has a non-zero composition offset.
	var ctts *mp4.CttsBox
	anyCto, anyNeg := false, false
	for _, s := range ts.samples {
		if s.cto != 0 {
			anyCto = true
		}
		if s.cto < 0 {
			anyNeg = true
		}
	}
	if anyCto {
		var counts []uint32
		var offsets []int32
		limit := n
		if ts.cttsShort {
			limit = n - 1
		}
		for i := uint32(0); i < limit; i++ {
			o := ts.samples[i].cto
			if len(offsets) > 0 && o == offsets[len(offsets)-1] {
				counts[len(counts)-1]++
			} else {
				counts = append(counts, 1)
				offsets = append(offsets, o)
			}
		}
		version := byte(0)
		if anyNeg {
			version = 1
		}
		ctts = &mp4.CttsBox{Version: version}
		if err := ctts.AddSampleCountsAndOffset(counts, offsets); err != nil {
			panic(err)
		}
	}

	// stss: present when not all samples are sync, or when forced.
	var stss *mp4.StssBox
	anyNonSync := false
	for _, s := range ts.samples {
		if !s.sync {
			anyNonSync = true
		}
	}
	switch {
	case ts.emptyStss:
		stss = &mp4.StssBox{}
	case anyNonSync:
		stss = &mp4.StssBox{}
		for i, s := range ts.samples {
			if s.sync {
				stss.SampleNumber = append(stss.SampleNumber, uint32(i+1))
			}
		}
		if ts.stssBeyond {
			stss.SampleNumber = append(stss.SampleNumber, n+5)
		}
	}

	stsz := &mp4.StszBox{SampleNumber: n}
	for _, d := range datas {
		stsz.SampleSize = append(stsz.SampleSize, uint32(len(d)))
	}

	stsc := &mp4.StscBox{}
	for i, ch := range ts.chunks {
		sdid := uint32(ch.sdid)
		if ts.sdidBeyond && i == 0 {
			sdid = uint32(ts.nrStsd) + 5
		}
		if err := stsc.AddEntry(uint32(i+1), uint32(ch.count), sdid); err != nil {
			panic(err)
		}
	}
	if ts.stscExtra {
		if err := stsc.AddEntry(uint32(len(ts.chunks)+1), 1, 1); err != nil {
			panic(err)
		}
	}

	stbl := mp4.NewStblBox()
	stbl.AddChild(stsd)
	stbl.AddChild(stts)
	if ctts != nil {
		stbl.AddChild(ctts)
	}
	if stss != nil {
		stbl.AddChild(stss)
	}
	stbl.AddChild(stsc)
	stbl.AddChild(stsz)
	if ts.addSdtp {
		stbl.AddChild(&mp4.SdtpBox{})
	}

	dinf := &mp4.DinfBox{}
	if ts.externalRef {
		dref := &mp4.DrefBox{}
		dref.AddChild(&mp4.URLBox{Location: "http://example.com/media.mp4"})
		dinf.AddChild(dref)
	} else {
		dinf.AddChild(mp4.CreateDref())
	}

	minf := &mp4.MinfBox{}
	if ts.handler == "vide" {
		minf.AddChild(mp4.CreateVmhd())
	} else {
		minf.AddChild(mp4.CreateSmhd())
	}
	minf.AddChild(dinf)
	minf.AddChild(stbl)

	mdia := &mp4.MdiaBox{}
	mdia.AddChild(mdhd)
	mdia.AddChild(hdlr)
	mdia.AddChild(minf)

	trak := &mp4.TrakBox{}
	trak.AddChild(tkhd)
	if ts.hasElst {
		edts := &mp4.EdtsBox{}
		elst := &mp4.ElstBox{Version: ts.elstVer, Entries: ts.entries}
		edts.Elst = []*mp4.ElstBox{elst}
		edts.AddChild(elst)
		trak.AddChild(edts)
	}
	trak.AddChild(mdia)
	return trak
}

// ---------------------------------------------------------------------------
// Independent demux of assembled or compacted files
// ---------------------------------------------------------------------------

// demuxedSample holds one sample's timing, identity and payload, read back
// from the file bytes through the sample tables alone.
type demuxedSample struct {
	dts    uint64
	dur    uint32
	cto    int32
	sync   bool
	sdid   uint32
	offset uint64
	data   []byte
}

// demuxedFile is a decoded file plus the demuxed samples of each track.
type demuxedFile struct {
	file   *mp4.File
	tracks [][]demuxedSample
}

// demux decodes fileBytes and resolves every sample of every track through
// stts/ctts/stss/stsc/stsz/stco, reading the payload from the file bytes.
func demux(c *taskcheck.C, fileBytes []byte) *demuxedFile {
	f, err := mp4.DecodeFile(bytes.NewReader(fileBytes), mp4.WithDecodeMode(mp4.DecModeLazyMdat))
	c.Check(err == nil, "demux: decode failed: %v", err)
	if err != nil {
		return &demuxedFile{file: f}
	}
	d := &demuxedFile{file: f}
	for _, trak := range f.Moov.Traks {
		d.tracks = append(d.tracks, demuxTrackSamples(c, trak, fileBytes))
	}
	return d
}

// demuxTrackSamples walks the sample tables of trak and reads each sample's
// payload from fileBytes.
func demuxTrackSamples(c *taskcheck.C, trak *mp4.TrakBox, fileBytes []byte) []demuxedSample {
	stbl := trak.Mdia.Minf.Stbl
	n := stbl.Stsz.GetNrSamples()
	var chunkOffsets []uint64
	if stbl.Stco != nil {
		for _, o := range stbl.Stco.ChunkOffset {
			chunkOffsets = append(chunkOffsets, uint64(o))
		}
	} else {
		chunkOffsets = append(chunkOffsets, stbl.Co64.ChunkOffset...)
	}
	// Per-sample chunk and sample description, from stsc.
	sdidOf := make([]uint32, n+1)
	chunkOf := make([]uint32, n+1)
	sample := uint32(1)
	entries := stbl.Stsc.Entries
	for i, e := range entries {
		nextFirst := uint32(len(chunkOffsets)) + 1
		if i+1 < len(entries) {
			nextFirst = entries[i+1].FirstChunk
		}
		sdid := stbl.Stsc.GetSampleDescriptionID(int(e.FirstChunk))
		for chunkNr := e.FirstChunk; chunkNr < nextFirst && sample <= n; chunkNr++ {
			for s := uint32(0); s < e.SamplesPerChunk && sample <= n; s++ {
				chunkOf[sample] = chunkNr
				sdidOf[sample] = sdid
				sample++
			}
		}
	}
	c.Check(sample == n+1, "demux: stsc covers %d samples, stsz declares %d", sample-1, n)

	// Absolute offset of each sample inside its chunk.
	offsetOf := make([]uint64, n+1)
	chunkCursor := make(map[uint32]uint64)
	for nr := uint32(1); nr <= n; nr++ {
		chunkNr := chunkOf[nr]
		cursor, ok := chunkCursor[chunkNr]
		if !ok {
			cursor = chunkOffsets[chunkNr-1]
		}
		offsetOf[nr] = cursor
		chunkCursor[chunkNr] = cursor + uint64(stbl.Stsz.GetSampleSize(int(nr)))
	}

	out := make([]demuxedSample, 0, n)
	dts := uint64(0)
	for nr := uint32(1); nr <= n; nr++ {
		dur := stbl.Stts.GetDur(nr)
		var cto int32
		if stbl.Ctts != nil {
			cto = stbl.Ctts.GetCompositionTimeOffset(nr)
		}
		sync := stbl.Stss == nil || stbl.Stss.IsSyncSample(nr)
		size := uint64(stbl.Stsz.GetSampleSize(int(nr)))
		offset := offsetOf[nr]
		end := offset + size
		c.Check(end <= uint64(len(fileBytes)), "demux: sample %d range [%d,%d) beyond file size %d",
			nr, offset, end, len(fileBytes))
		var data []byte
		if end <= uint64(len(fileBytes)) {
			data = append([]byte(nil), fileBytes[offset:end]...)
		}
		out = append(out, demuxedSample{
			dts:    dts,
			dur:    dur,
			cto:    cto,
			sync:   sync,
			sdid:   sdidOf[nr],
			offset: offset,
			data:   data,
		})
		dts += uint64(dur)
	}
	return out
}

// elstEntries returns the first elst box entries of trak, or nil.
func elstEntries(trak *mp4.TrakBox) []mp4.ElstEntry {
	if trak.Edts == nil || len(trak.Edts.Elst) == 0 {
		return nil
	}
	return trak.Edts.Elst[0].Entries
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

// videoSamples builds the 16-sample closed-GOP video sample list of the main
// fixture: GOPs of 4 with two B frames, composition offsets 6000/12000/3000
// in decode order, sync at samples 1, 5, 9 and 13.
func videoSamples() []sampleSpec {
	ctos := []int32{6000, 12000, 3000, 3000}
	var out []sampleSpec
	for i := 0; i < 16; i++ {
		sync := i%4 == 0
		size := 64 + (i*37)%120
		if sync {
			size += 400
		}
		out = append(out, sampleSpec{dur: 3000, cto: ctos[i%4], sync: sync, size: size})
	}
	return out
}

// audioSamples builds the 20-sample audio sample list of the main fixture.
func audioSamples() []sampleSpec {
	var out []sampleSpec
	for i := 0; i < 20; i++ {
		out = append(out, sampleSpec{dur: 480, sync: true, size: 32 + (i*11)%40})
	}
	return out
}

// mainSpec is the principal fixture: a 90000 Hz video track and a 48000 Hz
// audio track in a 1000 Hz movie, with B frames, overlapping, repeated and
// empty edits, non-contiguous interleaved chunks and description switches.
func mainSpec() *fileSpec {
	return &fileSpec{
		movieTimescale: 1000,
		movieDuration:  550,
		tracks: []*trackSpec{
			{
				handler:   "vide",
				timescale: 90000,
				trackID:   1,
				samples:   videoSamples(),
				chunks:    []chunkSpec{{4, 1}, {8, 2}, {4, 1}},
				nrStsd:    2,
				hasElst:   true,
				entries: []mp4.ElstEntry{
					mediaEntry(200, 30000), // media [30000, 48000)
					emptyEntry(50),
					mediaEntry(100, 9000),  // media [9000, 18000)
					mediaEntry(100, 15000), // media [15000, 24000), overlaps previous
					mediaEntry(100, 9000),  // repeated reference to [9000, 18000)
				},
				tkhdDur: 550,
				width:   320,
				height:  240,
			},
			{
				handler:   "soun",
				timescale: 48000,
				trackID:   2,
				samples:   audioSamples(),
				chunks:    []chunkSpec{{6, 1}, {14, 2}},
				nrStsd:    2,
				hasElst:   true,
				entries: []mp4.ElstEntry{
					mediaEntry(80, 960), // media [960, 4800)
					emptyEntry(25),
					mediaEntry(80, 960), // repeated reference
				},
				tkhdDur: 185,
				volume:  0x0100,
			},
		},
	}
}

// mainVideoKept is the 1-based source sample numbers the video track keeps.
var mainVideoKept = []int{1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15}

// mainAudioKept is the 1-based source sample numbers the audio track keeps.
var mainAudioKept = []int{3, 4, 5, 6, 7, 8, 9, 10}

// compactToBytes compacts input and returns the output bytes.
func compactToBytes(c *taskcheck.C, input []byte) []byte {
	var buf bytes.Buffer
	err := mp4.CompactEditedFile(bytes.NewReader(input), &buf)
	c.Check(err == nil, "CompactEditedFile failed: %v", err)
	return buf.Bytes()
}

// ---------------------------------------------------------------------------
// TestCompactReachability
// ---------------------------------------------------------------------------

func TestCompactReachability(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		spec := mainSpec()
		input := buildFile(spec)
		inputHash := sha256.Sum256(input)

		output := compactToBytes(c, input)

		// The input bytes and the input box tree are unchanged.
		c.Check(sha256.Sum256(input) == inputHash, "input bytes changed during compaction")
		before := demux(c, input)
		c.Check(len(before.tracks) == 2, "input: want 2 tracks, got %d", len(before.tracks))
		c.Check(len(before.tracks[0]) == 16, "input video: want 16 samples, got %d", len(before.tracks[0]))
		c.Check(len(before.tracks[1]) == 20, "input audio: want 20 samples, got %d", len(before.tracks[1]))
		c.Check(len(elstEntries(before.file.Moov.Traks[0])) == 5, "input video: want 5 elst entries, got %d",
			len(elstEntries(before.file.Moov.Traks[0])))

		// The output is smaller: unreferenced media is gone.
		c.Check(len(output) < len(input), "output (%d bytes) not smaller than input (%d bytes)", len(output), len(input))

		out := demux(c, output)
		c.Check(len(out.tracks) == 2, "output: want 2 tracks, got %d", len(out.tracks))

		// Every output sample is byte-identical to exactly one source sample,
		// each source sample appears at most once, and dropped samples are
		// absent. Repeated edits reference the same single copy.
		keptSets := [][]int{mainVideoKept, mainAudioKept}
		for ti, kept := range keptSets {
			tag := sampleTag(spec.tracks[ti].handler)
			c.Check(len(out.tracks[ti]) == len(kept), "track %d: want %d samples, got %d",
				ti+1, len(kept), len(out.tracks[ti]))
			seen := make(map[int]int)
			for i, s := range out.tracks[ti] {
				st, nr, ok := markerID(s.data)
				c.Check(ok && st == tag, "track %d sample %d: payload is not a %c marker", ti+1, i+1, tag)
				if !ok {
					continue
				}
				c.Check(seen[nr] == 0, "track %d: source sample %d appears more than once", ti+1, nr)
				seen[nr]++
				want := 0
				if i < len(kept) {
					want = kept[i]
				}
				c.Check(nr == want, "track %d sample %d: want source sample %d, got %d", ti+1, i+1, want, nr)
				c.Check(markerOK(s.data, tag, nr), "track %d sample %d: payload corrupted", ti+1, i+1)
			}
			// Dropped source samples are absent.
			for nr := 1; nr <= len(spec.tracks[ti].samples); nr++ {
				_, present := seen[nr]
				isKept := false
				for _, k := range kept {
					if k == nr {
						isKept = true
					}
				}
				c.Check(present == isKept, "track %d: source sample %d kept=%v, want kept=%v", ti+1, nr, present, isKept)
			}
		}

		// The sample addresses are real and tile the mdat payload exactly:
		// no byte is left over and no byte is referenced twice.
		mdatStart := out.file.Mdat.StartPos + out.file.Mdat.HeaderSize()
		mdatEnd := mdatStart + out.file.Mdat.GetLazyDataSize()
		type addrRange struct{ start, end uint64 }
		var ranges []addrRange
		var totalSize uint64
		for _, samples := range out.tracks {
			for _, s := range samples {
				ranges = append(ranges, addrRange{s.offset, s.offset + uint64(len(s.data))})
				totalSize += uint64(len(s.data))
			}
		}
		sort.Slice(ranges, func(i, j int) bool { return ranges[i].start < ranges[j].start })
		c.Check(uint64(len(ranges)) > 0, "output has no samples")
		cursor := mdatStart
		tiled := true
		for _, r := range ranges {
			if r.start != cursor {
				tiled = false
			}
			cursor = r.end
		}
		c.Check(tiled && cursor == mdatEnd, "sample ranges do not tile the mdat payload [%d, %d)", mdatStart, mdatEnd)
		c.Check(totalSize == mdatEnd-mdatStart, "sample sizes %d != mdat payload %d", totalSize, mdatEnd-mdatStart)

		// The chunk offsets are real addresses inside the output mdat.
		for _, r := range ranges {
			c.Check(r.start >= mdatStart && r.end <= mdatEnd,
				"sample range [%d, %d) outside mdat payload [%d, %d)", r.start, r.end, mdatStart, mdatEnd)
		}
	})
}

// ---------------------------------------------------------------------------
// TestCompactRoundTrip
// ---------------------------------------------------------------------------

func TestCompactRoundTrip(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		checkMainFixtureRoundTrip(c)
		checkNoElstRoundTrip(c)
		checkAllEmptyTrack(c)
		checkSignedCtts(c)
		checkV1TimeFields(c)
		checkProvidedFile(c)
	})
}

// checkMainFixtureRoundTrip compacts the main fixture and verifies the
// rebuilt tables, the output timeline and the rewritten edit list against
// independently computed expectations.
func checkMainFixtureRoundTrip(c *taskcheck.C) {
	spec := mainSpec()
	input := buildFile(spec)
	output := compactToBytes(c, input)
	out := demux(c, output)
	if len(out.tracks) != 2 {
		c.Check(false, "output: want 2 tracks, got %d", len(out.tracks))
		return
	}

	// Movie and track headers are preserved.
	mvhd := out.file.Moov.Mvhd
	c.Check(mvhd.Timescale == 1000, "mvhd timescale: want 1000, got %d", mvhd.Timescale)
	c.Check(mvhd.Duration == 550, "mvhd duration: want 550, got %d", mvhd.Duration)
	vTrak := out.file.Moov.Traks[0]
	aTrak := out.file.Moov.Traks[1]
	c.Check(vTrak.Tkhd.TrackID == 1 && aTrak.Tkhd.TrackID == 2, "track IDs not preserved: %d, %d",
		vTrak.Tkhd.TrackID, aTrak.Tkhd.TrackID)
	c.Check(vTrak.Tkhd.Duration == 550, "video tkhd duration: want 550, got %d", vTrak.Tkhd.Duration)
	c.Check(aTrak.Tkhd.Duration == 185, "audio tkhd duration: want 185, got %d", aTrak.Tkhd.Duration)
	c.Check(vTrak.Tkhd.Width == mp4.Fixed32(320<<16), "video tkhd width not preserved: %d", vTrak.Tkhd.Width)
	c.Check(aTrak.Tkhd.Volume == mp4.Fixed16(0x0100), "audio tkhd volume not preserved: %d", aTrak.Tkhd.Volume)
	c.Check(vTrak.Mdia.Mdhd.Timescale == 90000, "video mdhd timescale: want 90000, got %d", vTrak.Mdia.Mdhd.Timescale)
	c.Check(aTrak.Mdia.Mdhd.Timescale == 48000, "audio mdhd timescale: want 48000, got %d", aTrak.Mdia.Mdhd.Timescale)
	c.Check(len(vTrak.Mdia.Minf.Stbl.Stsd.Children) == 2, "video stsd: want 2 descriptions, got %d",
		len(vTrak.Mdia.Minf.Stbl.Stsd.Children))
	c.Check(len(aTrak.Mdia.Minf.Stbl.Stsd.Children) == 2, "audio stsd: want 2 descriptions, got %d",
		len(aTrak.Mdia.Minf.Stbl.Stsd.Children))

	// Video samples: source samples 1-7 and 9-15, decode time from zero,
	// durations, composition offsets, sync flags and descriptions preserved.
	video := out.tracks[0]
	wantCto := []int32{6000, 12000, 3000, 3000, 6000, 12000, 3000, 6000, 12000, 3000, 3000, 6000, 12000, 3000}
	wantSdid := []uint32{1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1}
	wantSync := map[int]bool{1: true, 5: true, 8: true, 12: true}
	c.Check(len(video) == len(mainVideoKept), "video: want %d samples, got %d", len(mainVideoKept), len(video))
	for i, s := range video {
		wantDts := uint64(i) * 3000
		c.Check(s.dts == wantDts, "video sample %d: want dts %d, got %d", i+1, wantDts, s.dts)
		c.Check(s.dur == 3000, "video sample %d: want dur 3000, got %d", i+1, s.dur)
		c.Check(s.cto == wantCto[i], "video sample %d: want cto %d, got %d", i+1, wantCto[i], s.cto)
		c.Check(s.sync == wantSync[i+1], "video sample %d: want sync=%v, got %v", i+1, wantSync[i+1], s.sync)
		c.Check(s.sdid == wantSdid[i], "video sample %d: want sdid %d, got %d", i+1, wantSdid[i], s.sdid)
		c.Check(markerOK(s.data, 'V', mainVideoKept[i]), "video sample %d: not source sample V%d",
			i+1, mainVideoKept[i])
	}
	c.Check(vTrak.Mdia.Mdhd.Duration == 42000, "video mdhd duration: want 42000, got %d", vTrak.Mdia.Mdhd.Duration)

	// Video edit list: durations and order preserved, media times shifted by
	// the owning component, empty edit untouched.
	vEntries := elstEntries(vTrak)
	wantVEntries := []mp4.ElstEntry{
		mediaEntry(200, 27000), // component [9,15] shifted by -3000
		emptyEntry(50),
		mediaEntry(100, 9000), // component [1,7] shifted by 0
		mediaEntry(100, 15000),
		mediaEntry(100, 9000), // repeated edit references the same samples
	}
	c.Check(len(vEntries) == len(wantVEntries), "video elst: want %d entries, got %d", len(wantVEntries), len(vEntries))
	for i, want := range wantVEntries {
		if i >= len(vEntries) {
			break
		}
		got := vEntries[i]
		c.Check(got.SegmentDuration == want.SegmentDuration && got.MediaTime == want.MediaTime &&
			got.MediaRateInteger == 1 && got.MediaRateFraction == 0,
			"video elst[%d]: want {%d, %d}, got {%d, %d, rate %d:%d}",
			i, want.SegmentDuration, want.MediaTime, got.SegmentDuration, got.MediaTime,
			got.MediaRateInteger, got.MediaRateFraction)
	}

	// The video edit playback intervals on the output media timeline.
	vSegs := [][2]int64{{27000, 45000}, {}, {9000, 18000}, {15000, 24000}, {9000, 18000}}
	for i, want := range vSegs {
		if i >= len(vEntries) || vEntries[i].MediaTime == -1 {
			continue
		}
		start := vEntries[i].MediaTime
		end := start + int64(vEntries[i].SegmentDuration)*90000/1000
		c.Check(start == want[0] && end == want[1], "video edit %d plays [%d, %d), want [%d, %d)",
			i, start, end, want[0], want[1])
	}

	// Audio samples: source samples 3-10, decode time from zero.
	audio := out.tracks[1]
	wantASdid := []uint32{1, 1, 1, 1, 2, 2, 2, 2}
	c.Check(len(audio) == len(mainAudioKept), "audio: want %d samples, got %d", len(mainAudioKept), len(audio))
	for i, s := range audio {
		wantDts := uint64(i) * 480
		c.Check(s.dts == wantDts, "audio sample %d: want dts %d, got %d", i+1, wantDts, s.dts)
		c.Check(s.dur == 480, "audio sample %d: want dur 480, got %d", i+1, s.dur)
		c.Check(s.sdid == wantASdid[i], "audio sample %d: want sdid %d, got %d", i+1, wantASdid[i], s.sdid)
		c.Check(markerOK(s.data, 'A', mainAudioKept[i]), "audio sample %d: not source sample A%d",
			i+1, mainAudioKept[i])
	}
	c.Check(aTrak.Mdia.Mdhd.Duration == 3840, "audio mdhd duration: want 3840, got %d", aTrak.Mdia.Mdhd.Duration)
	aEntries := elstEntries(aTrak)
	wantAEntries := []mp4.ElstEntry{mediaEntry(80, 0), emptyEntry(25), mediaEntry(80, 0)}
	c.Check(len(aEntries) == len(wantAEntries), "audio elst: want %d entries, got %d", len(wantAEntries), len(aEntries))
	for i, want := range wantAEntries {
		if i >= len(aEntries) {
			break
		}
		got := aEntries[i]
		c.Check(got.SegmentDuration == want.SegmentDuration && got.MediaTime == want.MediaTime,
			"audio elst[%d]: want {%d, %d}, got {%d, %d}",
			i, want.SegmentDuration, want.MediaTime, got.SegmentDuration, got.MediaTime)
	}

	// The audio edit playback interval on the output media timeline.
	if len(aEntries) > 0 && aEntries[0].MediaTime != -1 {
		start := aEntries[0].MediaTime
		end := start + int64(aEntries[0].SegmentDuration)*48000/1000
		c.Check(start == 0 && end == 3840, "audio edit 0 plays [%d, %d), want [0, 3840)", start, end)
	}

	// Chunk layout: one chunk per component/description run.
	vStsc := vTrak.Mdia.Minf.Stbl.Stsc.Entries
	type stscKey struct{ firstChunk, spc uint32 }
	var gotStsc []stscKey
	for _, e := range vStsc {
		gotStsc = append(gotStsc, stscKey{e.FirstChunk, e.SamplesPerChunk})
	}
	wantStsc := []stscKey{{1, 4}, {2, 3}, {3, 4}, {4, 3}}
	c.Check(fmt.Sprint(gotStsc) == fmt.Sprint(wantStsc), "video stsc: want %v, got %v", wantStsc, gotStsc)
	vStco := vTrak.Mdia.Minf.Stbl.Stco
	c.Check(vStco != nil && len(vStco.ChunkOffset) == 4, "video stco: want 4 chunks, got %v",
		vStco != nil && len(vStco.ChunkOffset) > 0)
}

// noElstSpec is a fixture without edit lists: everything is kept and the
// compacted output is a compaction fixpoint.
func noElstSpec() *fileSpec {
	return &fileSpec{
		movieTimescale: 1000,
		movieDuration:  480,
		tracks: []*trackSpec{
			{
				handler:   "vide",
				timescale: 90000,
				trackID:   1,
				samples:   videoSamples()[:8],
				chunks:    []chunkSpec{{4, 1}, {4, 1}},
				nrStsd:    1,
				tkhdDur:   480,
			},
			{
				handler:   "soun",
				timescale: 48000,
				trackID:   2,
				samples:   audioSamples()[:10],
				chunks:    []chunkSpec{{10, 1}},
				nrStsd:    1,
				tkhdDur:   480,
			},
		},
	}
}

// checkNoElstRoundTrip compacts a file without edit lists: all samples are
// kept, the presentation timeline is unchanged and no edts box appears.
func checkNoElstRoundTrip(c *taskcheck.C) {
	spec := noElstSpec()
	input := buildFile(spec)
	before := demux(c, input)
	output := compactToBytes(c, input)
	out := demux(c, output)
	if len(out.tracks) != 2 {
		c.Check(false, "no-elst output: want 2 tracks, got %d", len(out.tracks))
		return
	}
	for ti := range out.tracks {
		c.Check(len(out.tracks[ti]) == len(before.tracks[ti]), "no-elst track %d: want %d samples, got %d",
			ti+1, len(before.tracks[ti]), len(out.tracks[ti]))
		for i, s := range out.tracks[ti] {
			b := before.tracks[ti][i]
			c.Check(s.dts == b.dts && s.cto == b.cto && s.dur == b.dur && s.sync == b.sync && s.sdid == b.sdid,
				"no-elst track %d sample %d: timing or description changed", ti+1, i+1)
			c.Check(bytes.Equal(s.data, b.data), "no-elst track %d sample %d: payload changed", ti+1, i+1)
		}
		trak := out.file.Moov.Traks[ti]
		c.Check(trak.Edts == nil, "no-elst track %d: unexpected edts box", ti+1)
		c.Check(trak.Mdia.Mdhd.Duration == before.file.Moov.Traks[ti].Mdia.Mdhd.Duration,
			"no-elst track %d: mdhd duration changed", ti+1)
	}
	// Compacting again is a fixpoint.
	output2 := compactToBytes(c, output)
	c.Check(bytes.Equal(output, output2), "no-elst: second compaction not identical")
}

// checkAllEmptyTrack compacts a file whose video track has only empty edits:
// the track keeps zero samples and the file still reopens.
func checkAllEmptyTrack(c *taskcheck.C) {
	spec := &fileSpec{
		movieTimescale: 1000,
		movieDuration:  600,
		tracks: []*trackSpec{
			{
				handler:   "soun",
				timescale: 48000,
				trackID:   1,
				samples:   audioSamples()[:10],
				chunks:    []chunkSpec{{10, 1}},
				nrStsd:    1,
				hasElst:   true,
				entries:   []mp4.ElstEntry{mediaEntry(100, 0)},
				tkhdDur:   100,
			},
			{
				handler:   "vide",
				timescale: 90000,
				trackID:   2,
				samples:   videoSamples()[:6],
				chunks:    []chunkSpec{{6, 1}},
				nrStsd:    1,
				hasElst:   true,
				entries:   []mp4.ElstEntry{emptyEntry(300), emptyEntry(300)},
				tkhdDur:   600,
			},
		},
	}
	input := buildFile(spec)
	output := compactToBytes(c, input)
	out := demux(c, output)
	if len(out.tracks) != 2 {
		c.Check(false, "all-empty output: want 2 tracks, got %d", len(out.tracks))
		return
	}
	// The audio track is intact.
	c.Check(len(out.tracks[0]) == 10, "all-empty: audio want 10 samples, got %d", len(out.tracks[0]))
	// The video track has zero samples but consistent, empty tables.
	vTrak := out.file.Moov.Traks[1]
	c.Check(len(out.tracks[1]) == 0, "all-empty: video want 0 samples, got %d", len(out.tracks[1]))
	c.Check(vTrak.GetNrSamples() == 0, "all-empty: GetNrSamples want 0, got %d", vTrak.GetNrSamples())
	stbl := vTrak.Mdia.Minf.Stbl
	c.Check(len(stbl.Stts.SampleCount) == 0, "all-empty: stts not empty")
	c.Check(len(stbl.Stsc.Entries) == 0, "all-empty: stsc not empty")
	c.Check(len(stbl.Stco.ChunkOffset) == 0, "all-empty: stco not empty")
	c.Check(vTrak.Mdia.Mdhd.Duration == 0, "all-empty: mdhd duration want 0, got %d", vTrak.Mdia.Mdhd.Duration)
	vEntries := elstEntries(vTrak)
	c.Check(len(vEntries) == 2 && vEntries[0].MediaTime == -1 && vEntries[1].MediaTime == -1 &&
		vEntries[0].SegmentDuration == 300 && vEntries[1].SegmentDuration == 300,
		"all-empty: elst not preserved: %+v", vEntries)
	// The file reopens and the empty track reports no samples.
	reopened, err := mp4.DecodeFile(bytes.NewReader(output))
	c.Check(err == nil, "all-empty: reopen failed: %v", err)
	if err == nil {
		c.Check(reopened.Moov.Traks[1].GetNrSamples() == 0, "all-empty: reopened sample count not 0")
		c.Check(reopened.Moov.Traks[0].GetNrSamples() == 10, "all-empty: reopened audio sample count not 10")
	}
}

// checkSignedCtts compacts a video track with negative composition offsets:
// the offsets are preserved and the ctts box keeps a signed version.
func checkSignedCtts(c *taskcheck.C) {
	samples := []sampleSpec{
		{dur: 100, cto: -50, sync: true, size: 50},
		{dur: 100, cto: -50, size: 40},
		{dur: 100, cto: -50, size: 40},
		{dur: 100, cto: -50, size: 40},
	}
	spec := &fileSpec{
		movieTimescale: 1000,
		movieDuration:  300,
		tracks: []*trackSpec{
			{
				handler:   "vide",
				timescale: 1000,
				trackID:   1,
				samples:   samples,
				chunks:    []chunkSpec{{4, 1}},
				nrStsd:    1,
				hasElst:   true,
				entries:   []mp4.ElstEntry{mediaEntry(300, 50)}, // media [50, 350)
				tkhdDur:   300,
			},
		},
	}
	input := buildFile(spec)
	output := compactToBytes(c, input)
	out := demux(c, output)
	if len(out.tracks) != 1 {
		c.Check(false, "signed-ctts output: want 1 track, got %d", len(out.tracks))
		return
	}
	video := out.tracks[0]
	c.Check(len(video) == 4, "signed-ctts: want 4 samples, got %d", len(video))
	for i, s := range video {
		c.Check(s.dts == uint64(i)*100, "signed-ctts sample %d: want dts %d, got %d", i+1, i*100, s.dts)
		c.Check(s.cto == -50, "signed-ctts sample %d: want cto -50, got %d", i+1, s.cto)
	}
	vTrak := out.file.Moov.Traks[0]
	ctts := vTrak.Mdia.Minf.Stbl.Ctts
	c.Check(ctts != nil && ctts.Version == 1, "signed-ctts: want ctts version 1, got %+v", ctts)
	entries := elstEntries(vTrak)
	c.Check(len(entries) == 1 && entries[0].MediaTime == 50 && entries[0].SegmentDuration == 300,
		"signed-ctts: want elst [{300, 50}], got %+v", entries)
	c.Check(vTrak.Mdia.Mdhd.Duration == 400, "signed-ctts: want mdhd duration 400, got %d",
		vTrak.Mdia.Mdhd.Duration)
}

// checkV1TimeFields compacts a track whose media timeline exceeds 32 bits:
// elst and mdhd get version 1, and the v1 movie and track headers are
// preserved.
func checkV1TimeFields(c *taskcheck.C) {
	const bigDur = 2_150_000_000
	samples := []sampleSpec{
		{dur: bigDur, sync: true, size: 50},
		{dur: bigDur, size: 40},
		{dur: 3000, size: 40},
	}
	spec := &fileSpec{
		movieTimescale: 1000,
		movieDuration:  3000,
		movieVersion:   1,
		tracks: []*trackSpec{
			{
				handler:   "vide",
				timescale: 1000,
				trackID:   1,
				samples:   samples,
				chunks:    []chunkSpec{{3, 1}},
				nrStsd:    1,
				hasElst:   true,
				elstVer:   1, // 64-bit media time needs version 1 already in the source
				mdhdVer:   0, // the duration outgrows 32 bits, so mdhd must be bumped to v1
				tkhdVer:   1,
				useCo64:   true,
				entries:   []mp4.ElstEntry{mediaEntry(3000, 2*bigDur)}, // media [4.3e9, 4.3e9+3000)
				tkhdDur:   3000,
			},
		},
	}
	input := buildFile(spec)
	output := compactToBytes(c, input)
	out := demux(c, output)
	if len(out.tracks) != 1 {
		c.Check(false, "v1 output: want 1 track, got %d", len(out.tracks))
		return
	}
	vTrak := out.file.Moov.Traks[0]
	c.Check(len(out.tracks[0]) == 3, "v1: want 3 samples, got %d", len(out.tracks[0]))
	c.Check(vTrak.Mdia.Mdhd.Version == 1, "v1: want mdhd version 1, got %d", vTrak.Mdia.Mdhd.Version)
	c.Check(vTrak.Mdia.Mdhd.Duration == 2*bigDur+3000, "v1: want mdhd duration %d, got %d",
		2*bigDur+3000, vTrak.Mdia.Mdhd.Duration)
	entries := elstEntries(vTrak)
	c.Check(len(entries) == 1 && entries[0].MediaTime == 2*bigDur && entries[0].SegmentDuration == 3000,
		"v1: want elst [{3000, %d}], got %+v", 2*bigDur, entries)
	if vTrak.Edts != nil && len(vTrak.Edts.Elst) > 0 {
		c.Check(vTrak.Edts.Elst[0].Version == 1, "v1: want elst version 1, got %d", vTrak.Edts.Elst[0].Version)
	}
	c.Check(out.file.Moov.Mvhd.Version == 1, "v1: want mvhd version 1, got %d", out.file.Moov.Mvhd.Version)
	c.Check(vTrak.Tkhd.Version == 1, "v1: want tkhd version 1, got %d", vTrak.Tkhd.Version)
}

// checkProvidedFile compacts task_support/edited_tracks.mp4 and verifies the
// result, and independently verifies the file the editcompact example
// generated from it at .task-build/compact.mp4.
func checkProvidedFile(c *taskcheck.C) {
	srcBytes, err := os.ReadFile("../task_support/edited_tracks.mp4")
	c.Check(err == nil, "provided file: read failed: %v", err)
	if err != nil {
		return
	}
	output := compactToBytes(c, srcBytes)
	src := demux(c, srcBytes)
	out := demux(c, output)
	if len(out.tracks) != 2 || len(src.tracks) != 2 {
		c.Check(false, "provided file: unexpected track count")
		return
	}

	// The video keeps source samples 1-24, the audio keeps source samples
	// 2-45, payloads and composition offsets identical to the source.
	video, audio := out.tracks[0], out.tracks[1]
	c.Check(len(video) == 24, "provided: want 24 video samples, got %d", len(video))
	c.Check(len(audio) == 44, "provided: want 44 audio samples, got %d", len(audio))
	for i, s := range video {
		if i >= len(src.tracks[0]) {
			break
		}
		b := src.tracks[0][i]
		c.Check(bytes.Equal(s.data, b.data), "provided: video sample %d payload differs", i+1)
		c.Check(s.cto == b.cto && s.dur == b.dur && s.sync == b.sync,
			"provided: video sample %d timing differs", i+1)
		c.Check(s.dts == uint64(i)*512, "provided: video sample %d want dts %d, got %d", i+1, i*512, s.dts)
	}
	for i, s := range audio {
		if i+1 >= len(src.tracks[1]) {
			break
		}
		b := src.tracks[1][i+1]
		c.Check(bytes.Equal(s.data, b.data), "provided: audio sample %d payload differs", i+2)
		c.Check(s.dur == b.dur, "provided: audio sample %d duration differs", i+2)
		c.Check(s.dts == uint64(i)*1024, "provided: audio sample %d want dts %d, got %d", i+1, i*1024, s.dts)
	}

	// Headers and edit lists.
	vTrak, aTrak := out.file.Moov.Traks[0], out.file.Moov.Traks[1]
	c.Check(out.file.Moov.Mvhd.Timescale == 1000 && out.file.Moov.Mvhd.Duration == 1000,
		"provided: mvhd not preserved")
	c.Check(vTrak.Tkhd.Duration == 1000 && aTrak.Tkhd.Duration == 1000, "provided: tkhd durations not preserved")
	c.Check(vTrak.Mdia.Mdhd.Duration == 12288, "provided: want video mdhd 12288, got %d", vTrak.Mdia.Mdhd.Duration)
	c.Check(aTrak.Mdia.Mdhd.Duration == 45056, "provided: want audio mdhd 45056, got %d", aTrak.Mdia.Mdhd.Duration)
	vEntries, aEntries := elstEntries(vTrak), elstEntries(aTrak)
	c.Check(len(vEntries) == 1 && vEntries[0].SegmentDuration == 1000 && vEntries[0].MediaTime == 1024,
		"provided: video elst want [{1000, 1024}], got %+v", vEntries)
	c.Check(len(aEntries) == 1 && aEntries[0].SegmentDuration == 1000 && aEntries[0].MediaTime == 0,
		"provided: audio elst want [{1000, 0}], got %+v", aEntries)

	// The mdat holds exactly the kept samples.
	mdatSize := out.file.Mdat.GetLazyDataSize()
	var sampleBytes uint64
	for _, samples := range out.tracks {
		for _, s := range samples {
			sampleBytes += uint64(len(s.data))
		}
	}
	c.Check(sampleBytes == mdatSize, "provided: sample bytes %d != mdat payload %d", sampleBytes, mdatSize)
	c.Check(len(output) < len(srcBytes), "provided: output not smaller than input")

	// Compacting the result again is a fixpoint.
	output2 := compactToBytes(c, output)
	c.Check(bytes.Equal(output, output2), "provided: second compaction not identical")

	// The file the example generated in this run is the same compaction.
	// When the tests run without task_support/run.sh, the artifact does not
	// exist yet; produce it the way the example does so that it can be
	// checked anyway.
	examplePath := "../.task-build/compact.mp4"
	if _, statErr := os.Stat(examplePath); statErr != nil {
		if mkErr := os.MkdirAll("../.task-build", 0o755); mkErr == nil {
			var exBuf bytes.Buffer
			if mp4.CompactEditedFile(bytes.NewReader(srcBytes), &exBuf) == nil {
				_ = os.WriteFile(examplePath, exBuf.Bytes(), 0o644)
			}
		}
	}
	exampleBytes, err := os.ReadFile(examplePath)
	c.Check(err == nil, "example output: read failed: %v", err)
	if err != nil {
		return
	}
	c.Check(bytes.Equal(exampleBytes, output), "example output differs from in-test compaction")
	ex := demux(c, exampleBytes)
	if len(ex.tracks) != 2 {
		c.Check(false, "example output: unexpected track count")
		return
	}
	c.Check(len(ex.tracks[0]) == 24 && len(ex.tracks[1]) == 44,
		"example output: want 24+44 samples, got %d+%d", len(ex.tracks[0]), len(ex.tracks[1]))
	for i, s := range ex.tracks[0] {
		c.Check(bytes.Equal(s.data, src.tracks[0][i].data), "example output: video sample %d payload differs", i+1)
	}
	for i, s := range ex.tracks[1] {
		c.Check(bytes.Equal(s.data, src.tracks[1][i+1].data), "example output: audio sample %d payload differs", i+2)
	}
	exVEntries := elstEntries(ex.file.Moov.Traks[0])
	exAEntries := elstEntries(ex.file.Moov.Traks[1])
	c.Check(len(exVEntries) == 1 && exVEntries[0].MediaTime == 1024 && exVEntries[0].SegmentDuration == 1000,
		"example output: video elst wrong: %+v", exVEntries)
	c.Check(len(exAEntries) == 1 && exAEntries[0].MediaTime == 0 && exAEntries[0].SegmentDuration == 1000,
		"example output: audio elst wrong: %+v", exAEntries)
}

// ---------------------------------------------------------------------------
// TestCompactReject
// ---------------------------------------------------------------------------

func TestCompactReject(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Base valid spec mutators.
		cases := []struct {
			name      string
			spec      func() *fileSpec
			raw       func() []byte // alternative to spec
			wantEmpty bool          // output must be empty: validation before writing
			errSub    string
		}{}
		add := func(name string, mutate func(*fileSpec), errSub string) {
			cases = append(cases, struct {
				name      string
				spec      func() *fileSpec
				raw       func() []byte
				wantEmpty bool
				errSub    string
			}{name, func() *fileSpec {
				s := mainSpec()
				mutate(s)
				return s
			}, nil, true, errSub})
		}

		add("zero movie timescale", func(s *fileSpec) { s.movieTimescale = 0 }, "timescale is zero")
		add("zero media timescale", func(s *fileSpec) { s.tracks[0].timescale = 0 }, "timescale is zero")
		add("non-unit media rate", func(s *fileSpec) {
			s.tracks[0].entries[0].MediaRateInteger = 2
		}, "non-unit media rate")
		add("non-divisible segment duration", func(s *fileSpec) {
			s.tracks[1].timescale = 44100
			s.tracks[1].entries[0].SegmentDuration = 1
		}, "whole media ticks")
		add("stts coverage mismatch", func(s *fileSpec) { s.tracks[0].sttsShort = true }, "inconsistent stts")
		add("ctts coverage mismatch", func(s *fileSpec) { s.tracks[0].cttsShort = true }, "inconsistent ctts")
		add("stsc beyond stco", func(s *fileSpec) { s.tracks[0].stscExtra = true }, "stsc")
		add("stss beyond sample count", func(s *fileSpec) { s.tracks[0].stssBeyond = true }, "stss")
		add("sample description out of range", func(s *fileSpec) { s.tracks[0].sdidBeyond = true }, "sample description")
		add("unsupported per-sample table", func(s *fileSpec) { s.tracks[0].addSdtp = true }, "unsupported per-sample table")
		add("external data reference", func(s *fileSpec) { s.tracks[0].externalRef = true }, "outside this file")
		add("no sync sample", func(s *fileSpec) { s.tracks[0].emptyStss = true }, "no sync sample")
		add("media interval out of range", func(s *fileSpec) {
			s.tracks[0].entries[0].MediaTime = 10_000_000
		}, "outside media timeline")
		add("media interval end out of range", func(s *fileSpec) {
			s.tracks[0].entries[0].MediaTime = 47000 // [47000, 65000) beyond 51000+3000
		}, "outside media timeline")
		add("invalid negative media time", func(s *fileSpec) {
			s.tracks[0].entries[0].MediaTime = -2
		}, "invalid media_time")
		add("zero-duration media segment", func(s *fileSpec) {
			s.tracks[0].entries[0].SegmentDuration = 0
		}, "zero duration")
		add("edit not covered by presentation intervals", func(s *fileSpec) {
			s.tracks[0].entries[0].MediaTime = 9100 // not a presentation boundary
		}, "exactly cover")
		add("segment duration overflow", func(s *fileSpec) {
			s.tracks[0].elstVer = 1 // 64-bit fields keep the huge duration
			s.tracks[0].entries[0].SegmentDuration = 1 << 62
		}, "overflow")
		add("second mdat box", func(s *fileSpec) { s.extraMdat = true }, "exactly one mdat")
		add("fragmented file", func(s *fileSpec) { s.addMoof = true }, "fragmented")
		add("unknown handler type", func(s *fileSpec) { s.tracks[0].handler = "subt" }, "unsupported handler")

		// A video edit whose shifted media_time would go negative: the first
		// visible sample presents before its component's decode start.
		negShift := &fileSpec{
			movieTimescale: 1000,
			movieDuration:  1000,
			tracks: []*trackSpec{
				{
					handler:   "vide",
					timescale: 1000,
					trackID:   1,
					samples: []sampleSpec{
						{dur: 1000, cto: 2000, size: 40},
						{dur: 1000, cto: 2000, size: 40},
						{dur: 1000, cto: 2000, size: 40},
						{dur: 1000, cto: 2000, size: 40},
						{dur: 1000, cto: 2000, sync: true, size: 60},
						{dur: 1000, cto: -4500, size: 40},
					},
					chunks:  []chunkSpec{{6, 1}},
					nrStsd:  1,
					hasElst: true,
					entries: []mp4.ElstEntry{mediaEntry(1000, 500)}, // media [500, 1500)
					tkhdDur: 1000,
				},
			},
		}
		cases = append(cases, struct {
			name      string
			spec      func() *fileSpec
			raw       func() []byte
			wantEmpty bool
			errSub    string
		}{"negative shifted media time", func() *fileSpec { return negShift }, nil, true, "negative"})

		// Truncated inputs.
		good := buildFile(mainSpec())
		cases = append(cases, struct {
			name      string
			spec      func() *fileSpec
			raw       func() []byte
			wantEmpty bool
			errSub    string
		}{"truncated in mdat before moov", nil, func() []byte {
			return good[:len(good)*6/10]
		}, true, ""})
		// A compacted (moov-first) file truncated inside the mdat payload
		// fails while copying, after part of the output was written.
		compacted := compactToBytes(c, buildFile(noElstSpec()))
		cases = append(cases, struct {
			name      string
			spec      func() *fileSpec
			raw       func() []byte
			wantEmpty bool
			errSub    string
		}{"truncated in mdat payload", nil, func() []byte {
			return compacted[:len(compacted)-3]
		}, false, ""})

		for _, tc := range cases {
			var input []byte
			if tc.raw != nil {
				input = tc.raw()
			} else {
				input = buildFile(tc.spec())
			}
			var buf bytes.Buffer
			err := mp4.CompactEditedFile(bytes.NewReader(input), &buf)
			c.Check(err != nil, "%s: want error, got nil", tc.name)
			if err != nil && tc.errSub != "" {
				c.Check(bytes.Contains([]byte(err.Error()), []byte(tc.errSub)),
					"%s: error %q does not mention %q", tc.name, err, tc.errSub)
			}
			if tc.wantEmpty {
				c.Check(buf.Len() == 0, "%s: want no output before validation error, wrote %d bytes", tc.name, buf.Len())
			} else {
				c.Check(buf.Len() > 0, "%s: want partial output after I/O error (err=%v, written=%d)", tc.name, err, buf.Len())
			}
		}
	})
}
