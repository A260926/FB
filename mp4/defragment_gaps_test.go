package mp4_test

import (
	"bytes"
	"fmt"
	"testing"

	"github.com/Eyevinn/mp4ff/mp4"
	"github.com/Eyevinn/mp4ff/task_support/taskcheck"
)

// ---- gap test fixture builder -------------------------------------------

// gapS is one fragment sample in the gap tests.
type gapS struct {
	dts     uint64
	dur     uint32
	cts     int32
	nonSync bool
	payload []byte // when nil, a unique payload is assigned from id
}

type gapBuilder struct {
	t           *testing.T
	init        *mp4.InitSegment
	buf         bytes.Buffer
	seq         uint32
	payload     int
	initWritten bool
}

// newGapBuilder makes an init with one track per given media timescale and
// the given movie timescale. The init box is encoded lazily, so addElst may
// be called before the first fragment.
func newGapBuilder(t *testing.T, movieTs uint32, trackTss ...uint32) *gapBuilder {
	t.Helper()
	init := mp4.CreateEmptyInit()
	init.Moov.Mvhd.Timescale = movieTs
	if len(trackTss) == 0 {
		trackTss = []uint32{1000, 1000}
	}
	for _, ts := range trackTss {
		init.AddEmptyTrack(ts, "video", "und")
	}
	return &gapBuilder{t: t, init: init, seq: 1}
}

func (b *gapBuilder) writeInit() {
	if b.initWritten {
		return
	}
	if err := b.init.Encode(&b.buf); err != nil {
		b.t.Fatalf("encode init: %v", err)
	}
	b.initWritten = true
}

func (b *gapBuilder) trak(id uint32) *mp4.TrakBox {
	for _, tr := range b.init.Moov.Traks {
		if tr.Tkhd.TrackID == id {
			return tr
		}
	}
	b.t.Fatalf("track %d not found", id)
	return nil
}

func (b *gapBuilder) addElst(id uint32, entries ...mp4.ElstEntry) {
	edts := &mp4.EdtsBox{}
	edts.AddChild(&mp4.ElstBox{Entries: entries})
	b.trak(id).AddChild(edts)
}

func (b *gapBuilder) uniquePayload() []byte {
	b.payload++
	id := b.payload
	return []byte{'G', 'A', 'P', byte(id), byte(id >> 8), byte(id >> 16), byte(id >> 24), 0x5A}
}

// frag appends one single-traf fragment with the given samples.
func (b *gapBuilder) frag(trackID uint32, ss ...gapS) *gapBuilder {
	b.t.Helper()
	b.writeInit()
	b.seq++
	frag, err := mp4.CreateFragment(b.seq, trackID)
	if err != nil {
		b.t.Fatalf("create fragment: %v", err)
	}
	for _, s := range ss {
		payload := s.payload
		if payload == nil {
			payload = b.uniquePayload()
		}
		flags := mp4.SampleFlags{SampleDependsOn: 2}.Encode()
		if s.nonSync {
			flags = mp4.SampleFlags{SampleDependsOn: 1, SampleIsNonSync: true}.Encode()
		}
		frag.AddFullSample(mp4.FullSample{
			Sample: mp4.Sample{
				Flags:                 flags,
				Dur:                   s.dur,
				Size:                  uint32(len(payload)),
				CompositionTimeOffset: s.cts,
			},
			DecodeTime: s.dts,
			Data:       payload,
		})
	}
	if err := frag.Encode(&b.buf); err != nil {
		b.t.Fatalf("encode fragment: %v", err)
	}
	return b
}

func (b *gapBuilder) bytes() []byte { b.writeInit(); return b.buf.Bytes() }

// ---- conversion / inspection helpers ------------------------------------

func runGapPreserve(data []byte, ids ...uint32) ([]byte, *mp4.File, error) {
	rs := bytes.NewReader(data)
	f, err := mp4.DecodeFile(rs, mp4.WithDecodeMode(mp4.DecModeLazyMdat))
	if err != nil {
		return nil, nil, fmt.Errorf("decode input: %w", err)
	}
	var out bytes.Buffer
	if err := mp4.DefragmentPreserveGaps(f, rs, &out, ids...); err != nil {
		return nil, nil, err
	}
	outBytes := out.Bytes()
	outFile, err := mp4.DecodeFile(bytes.NewReader(outBytes))
	if err != nil {
		return outBytes, nil, fmt.Errorf("decode output: %w", err)
	}
	return outBytes, outFile, nil
}

func gapTrak(f *mp4.File, id uint32) *mp4.TrakBox {
	for _, tr := range f.Moov.Traks {
		if tr.Tkhd.TrackID == id {
			return tr
		}
	}
	return nil
}

func gapDurs(trak *mp4.TrakBox) ([]uint32, error) {
	n := trak.GetNrSamples()
	ss, err := trak.GetSampleData(1, n)
	if err != nil {
		return nil, err
	}
	d := make([]uint32, len(ss))
	for i, s := range ss {
		d[i] = s.Dur
	}
	return d, nil
}

func gapCTS(trak *mp4.TrakBox) ([]int32, error) {
	n := trak.GetNrSamples()
	ss, err := trak.GetSampleData(1, n)
	if err != nil {
		return nil, err
	}
	c := make([]int32, len(ss))
	for i, s := range ss {
		c[i] = s.CompositionTimeOffset
	}
	return c, nil
}

func gapElst(trak *mp4.TrakBox) []mp4.ElstEntry {
	if trak == nil || trak.Edts == nil || len(trak.Edts.Elst) == 0 {
		return nil
	}
	return trak.Edts.Elst[0].Entries
}

// gapRawPayload reads sample bytes at the absolute stco/co64 addresses
// straight from the serialized output, checking physical chunk addresses.
func gapRawPayload(out []byte, trak *mp4.TrakBox) ([]byte, error) {
	n := trak.GetNrSamples()
	ranges, err := trak.GetRangesForSampleInterval(1, n)
	if err != nil {
		return nil, err
	}
	var data []byte
	for _, r := range ranges {
		if r.Offset > uint64(len(out)) || r.Size > uint64(len(out))-r.Offset {
			return nil, fmt.Errorf("range [%d,%d) outside %d-byte output", r.Offset, r.Offset+r.Size, len(out))
		}
		data = append(data, out[r.Offset:r.Offset+r.Size]...)
	}
	return data, nil
}

// gapMdat returns the decoded (single) mdat payload.
func gapMdat(f *mp4.File) []byte {
	if f.Mdat == nil {
		return nil
	}
	return f.Mdat.Data
}

type errWriter struct{ writes int }

func (w *errWriter) Write(p []byte) (int, error) {
	w.writes++
	return 0, fmt.Errorf("injected write failure")
}

// ---- TestGapIslands -----------------------------------------------------

func TestGapIslands(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Movie timescale 1000. Video media timescale 1000 with the prompt's
		// DTS 0,10,40,50 dur 10. Audio uses a different timescale (40000)
		// with equivalent positions, to check cross-timescale sync.
		b := newGapBuilder(t, 1000, 1000, 40000)
		vP := [][]byte{
			[]byte("VIDEO-A"), []byte("VIDEO-B"), []byte("VIDEO-C"), []byte("VIDEO-D"),
		}
		aP := [][]byte{
			[]byte("AUD0"), []byte("AUD1"), []byte("AUD2"), []byte("AUD3"),
		}
		b.frag(1,
			gapS{dts: 0, dur: 10, payload: vP[0]},
			gapS{dts: 10, dur: 10, payload: vP[1]}).
			frag(1,
				gapS{dts: 40, dur: 10, payload: vP[2]},
				gapS{dts: 50, dur: 10, payload: vP[3]}).
			frag(2,
				gapS{dts: 0, dur: 400, payload: aP[0]},
				gapS{dts: 400, dur: 400, payload: aP[1]}).
			frag(2,
				gapS{dts: 1600, dur: 400, payload: aP[2]},
				gapS{dts: 2000, dur: 400, payload: aP[3]})
		src := b.bytes()

		out, f, err := runGapPreserve(src)
		c.Check(err == nil, "DefragmentPreserveGaps dual-track: %v", err)
		if err != nil {
			return
		}
		c.Check(!f.IsFragmented(), "output must be progressive")
		c.Check(len(f.Moov.Traks) == 2, "want 2 tracks, got %d", len(f.Moov.Traks))

		v := gapTrak(f, 1)
		a := gapTrak(f, 2)
		// Real durations survive: no 30-tick stretched packet.
		vd, err := gapDurs(v)
		c.Check(err == nil, "video durs: %v", err)
		c.Check(fmt.Sprint(vd) == "[10 10 10 10]", "video real durations %v", vd)
		ad, err := gapDurs(a)
		c.Check(err == nil, "audio durs: %v", err)
		c.Check(fmt.Sprint(ad) == "[400 400 400 400]", "audio real durations %v", ad)
		c.Check(v.Mdia.Mdhd.Duration == 40, "video mdhd duration %d want 40 (compacted)", v.Mdia.Mdhd.Duration)
		c.Check(a.Mdia.Mdhd.Duration == 1600, "audio mdhd duration %d want 1600 (compacted)", a.Mdia.Mdhd.Duration)

		// Movie blank 20..40: media 0..20, empty 20, media at new time 20.
		wantVElst := []mp4.ElstEntry{
			{SegmentDuration: 20, MediaTime: 0, MediaRateInteger: 1},
			{SegmentDuration: 20, MediaTime: -1, MediaRateInteger: 1},
			{SegmentDuration: 20, MediaTime: 20, MediaRateInteger: 1},
		}
		c.Check(elstEqual(gapElst(v), wantVElst), "video elst %+v want %+v", gapElst(v), wantVElst)
		wantAElst := []mp4.ElstEntry{
			{SegmentDuration: 20, MediaTime: 0, MediaRateInteger: 1},
			{SegmentDuration: 20, MediaTime: -1, MediaRateInteger: 1},
			{SegmentDuration: 20, MediaTime: 800, MediaRateInteger: 1},
		}
		c.Check(elstEqual(gapElst(a), wantAElst), "audio elst %+v want %+v", gapElst(a), wantAElst)
		c.Check(v.Tkhd.Duration == 60 && a.Tkhd.Duration == 60,
			"track durations v=%d a=%d want 60/60", v.Tkhd.Duration, a.Tkhd.Duration)
		c.Check(f.Moov.Mvhd.Duration == 60, "mvhd duration %d want 60", f.Moov.Mvhd.Duration)

		// Payloads at the assigned physical addresses and write-once in mdat.
		wantV := bytes.Join(vP, nil)
		gotV, err := gapRawPayload(out, v)
		c.Check(err == nil && bytes.Equal(gotV, wantV), "video physical payload err=%v match=%v", err, bytes.Equal(gotV, wantV))
		wantA := bytes.Join(aP, nil)
		gotA, err := gapRawPayload(out, a)
		c.Check(err == nil && bytes.Equal(gotA, wantA), "audio physical payload err=%v match=%v", err, bytes.Equal(gotA, wantA))
		mdat := gapMdat(f)
		for _, p := range append(append([][]byte{}, vP...), aP...) {
			c.Check(bytes.Count(mdat, p) == 1, "payload %q written %d times, want 1", p, bytes.Count(mdat, p))
		}

		// Dropped track: keep track 2 only.
		out1, f1, err := runGapPreserve(src, 2)
		c.Check(err == nil, "preserve gaps track 2: %v", err)
		if err == nil {
			c.Check(len(f1.Moov.Traks) == 1 && f1.Moov.Traks[0].Tkhd.TrackID == 2,
				"dropped-track output traks %v", trackIDsOf(f1))
			a1 := gapTrak(f1, 2)
			c.Check(elstEqual(gapElst(a1), wantAElst), "kept audio elst %+v", gapElst(a1))
			got, err := gapRawPayload(out1, a1)
			c.Check(err == nil && bytes.Equal(got, wantA), "dropped-track audio payload ok=%v err=%v", bytes.Equal(got, wantA), err)
		}
		// Unknown track ID is an error.
		_, _, err = runGapPreserve(src, 99)
		c.Check(err != nil, "unknown track ID must error")

		// I/O failure from the writer is reported.
		rs := bytes.NewReader(src)
		ff, err := mp4.DecodeFile(rs, mp4.WithDecodeMode(mp4.DecModeLazyMdat))
		c.Check(err == nil, "decode for I/O test: %v", err)
		w := &errWriter{}
		err = mp4.DefragmentPreserveGaps(ff, rs, w)
		c.Check(err != nil && w.writes > 0, "failing writer must return error (err=%v writes=%d)", err, w.writes)
	})
}

func elstEqual(got, want []mp4.ElstEntry) bool {
	if len(got) != len(want) {
		return false
	}
	for i := range want {
		g, w := got[i], want[i]
		if g.SegmentDuration != w.SegmentDuration || g.MediaTime != w.MediaTime ||
			g.MediaRateInteger != w.MediaRateInteger || g.MediaRateFraction != w.MediaRateFraction {
			return false
		}
	}
	return true
}

func trackIDsOf(f *mp4.File) []uint32 {
	ids := make([]uint32, 0, len(f.Moov.Traks))
	for _, t := range f.Moov.Traks {
		ids = append(ids, t.Tkhd.TrackID)
	}
	return ids
}

// gapIslandSource builds the prompt's DTS 0,10,40,50 / dur 10 single-track
// source in the given timescales, attaching entries as its original elst
// when non-nil.
func gapIslandSource(t *testing.T, movieTs, mediaTs uint32, entries []mp4.ElstEntry) []byte {
	t.Helper()
	b := newGapBuilder(t, movieTs, mediaTs)
	if entries != nil {
		b.addElst(1, entries...)
	}
	b.frag(1,
		gapS{dts: 0, dur: 10, payload: []byte("AA")},
		gapS{dts: 10, dur: 10, payload: []byte("BB")}).
		frag(1,
			gapS{dts: 40, dur: 10, payload: []byte("CC")},
			gapS{dts: 50, dur: 10, payload: []byte("DD")})
	return b.bytes()
}

type countWriter struct{ writes, bytes int }

func (w *countWriter) Write(p []byte) (int, error) {
	w.writes++
	w.bytes += len(p)
	return len(p), nil
}

func TestGapEdits(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		r1 := func(md, mt uint64) mp4.ElstEntry {
			return mp4.ElstEntry{SegmentDuration: md, MediaTime: int64(mt), MediaRateInteger: 1}
		}
		empty := func(d uint64) mp4.ElstEntry {
			return mp4.ElstEntry{SegmentDuration: d, MediaTime: -1, MediaRateInteger: 1}
		}

		// One edit covering the whole source: media / gap-empty / media.
		src := gapIslandSource(t, 1000, 1000, []mp4.ElstEntry{r1(60, 0)})
		_, f, err := runGapPreserve(src)
		c.Check(err == nil, "whole-span edit: %v", err)
		if err == nil {
			got := gapElst(gapTrak(f, 1))
			want := []mp4.ElstEntry{r1(20, 0), empty(20), r1(20, 20)}
			c.Check(elstEqual(got, want), "whole-span elst %+v", got)
			c.Check(gapTrak(f, 1).Tkhd.Duration == 60 && f.Moov.Mvhd.Duration == 60,
				"durations tkhd=%d mvhd=%d want 60", gapTrak(f, 1).Tkhd.Duration, f.Moov.Mvhd.Duration)
		}

		// Duplicate and reverse-order references keep pointing at one media copy.
		src = gapIslandSource(t, 1000, 1000, []mp4.ElstEntry{
			r1(20, 40), r1(20, 0), r1(20, 40),
		})
		_, f, err = runGapPreserve(src)
		c.Check(err == nil, "reverse/duplicate edits: %v", err)
		if err == nil {
			got := gapElst(gapTrak(f, 1))
			want := []mp4.ElstEntry{r1(20, 20), r1(20, 0), r1(20, 20)}
			c.Check(elstEqual(got, want), "reverse/duplicate elst %+v", got)
			c.Check(gapTrak(f, 1).Tkhd.Duration == 60, "reverse total %d want 60", gapTrak(f, 1).Tkhd.Duration)
		}

		// Original empty edits are preserved verbatim alongside mapped media.
		src = gapIslandSource(t, 1000, 1000, []mp4.ElstEntry{empty(15), r1(20, 40)})
		_, f, err = runGapPreserve(src)
		c.Check(err == nil, "preserved empty edit: %v", err)
		if err == nil {
			got := gapElst(gapTrak(f, 1))
			want := []mp4.ElstEntry{empty(15), r1(20, 20)}
			c.Check(elstEqual(got, want), "empty-preserve elst %+v", got)
		}

		// Partial intersection: [10,40) gives 10 ticks of island one and a
		// 20-tick empty edit for the hole.
		src = gapIslandSource(t, 1000, 1000, []mp4.ElstEntry{r1(30, 10)})
		_, f, err = runGapPreserve(src)
		c.Check(err == nil, "partial edit: %v", err)
		if err == nil {
			got := gapElst(gapTrak(f, 1))
			want := []mp4.ElstEntry{r1(10, 10), empty(20)}
			c.Check(elstEqual(got, want), "partial elst %+v", got)
		}

		// An edit entirely inside the recording gap becomes one empty edit.
		src = gapIslandSource(t, 1000, 1000, []mp4.ElstEntry{r1(20, 20)})
		_, f, err = runGapPreserve(src)
		c.Check(err == nil, "hole-only edit: %v", err)
		if err == nil {
			got := gapElst(gapTrak(f, 1))
			want := []mp4.ElstEntry{empty(20)}
			c.Check(elstEqual(got, want), "hole-only elst %+v", got)
		}

		// Final duration-zero media edit expands to the source end first.
		src = gapIslandSource(t, 1000, 1000, []mp4.ElstEntry{r1(10, 0), r1(0, 40)})
		_, f, err = runGapPreserve(src)
		c.Check(err == nil, "zero-final expansion: %v", err)
		if err == nil {
			got := gapElst(gapTrak(f, 1))
			want := []mp4.ElstEntry{r1(10, 0), r1(20, 20)}
			c.Check(elstEqual(got, want), "zero-final elst %+v", got)
			c.Check(gapTrak(f, 1).Tkhd.Duration == 30, "expanded tkhd %d want 30", gapTrak(f, 1).Tkhd.Duration)
		}

		// Every non-empty input segment duration survives as the sum of its pieces.
		src = gapIslandSource(t, 1000, 1000, []mp4.ElstEntry{r1(25, 5), empty(7), r1(13, 47)})
		_, f, err = runGapPreserve(src)
		c.Check(err == nil, "cumulative durations: %v", err)
		if err == nil {
			got := gapElst(gapTrak(f, 1))
			var sum uint64
			for _, e := range got {
				sum += e.SegmentDuration
			}
			c.Check(sum == 45, "cumulative movie duration %d want 45 (25+7+13)", sum)
		}

		// Non-divisible island cut is rejected before any output write.
		b := newGapBuilder(t, 1000, 3000)
		b.addElst(1, r1(1000, 0))
		b.frag(1, gapS{dts: 0, dur: 1000}).
			frag(1, gapS{dts: 3000, dur: 1000})
		badCut := b.bytes()
		_, _, err = runGapPreserve(badCut)
		c.Check(err != nil, "non-divisible elst cut must error")
		rs := bytes.NewReader(badCut)
		ff, derr := mp4.DecodeFile(rs, mp4.WithDecodeMode(mp4.DecModeLazyMdat))
		c.Check(derr == nil, "decode bad-cut input: %v", derr)
		cw := &countWriter{}
		err = mp4.DefragmentPreserveGaps(ff, rs, cw)
		c.Check(err != nil && cw.writes == 0, "non-divisible cut must fail before writes (err=%v writes=%d)", err, cw.writes)

		// No-elst non-divisible common-zero conversion is also rejected.
		b = newGapBuilder(t, 1000, 3000)
		b.frag(1, gapS{dts: 0, dur: 1000}).
			frag(1, gapS{dts: 3000, dur: 1000})
		_, _, err = runGapPreserve(b.bytes())
		c.Check(err != nil, "non-divisible no-elst cut must error")

		// Non-unit rate is rejected with context.
		bad := gapIslandSource(t, 1000, 1000, []mp4.ElstEntry{
			{SegmentDuration: 60, MediaTime: 0, MediaRateInteger: 2},
		})
		_, _, err = runGapPreserve(bad)
		c.Check(err != nil, "non-unit rate must error")

		// Unsupported negative media time is rejected.
		bad = gapIslandSource(t, 1000, 1000, []mp4.ElstEntry{r1(10, 0), {SegmentDuration: 10, MediaTime: -2, MediaRateInteger: 1}})
		_, _, err = runGapPreserve(bad)
		c.Check(err != nil, "media_time -2 must error")

		// Media edit extending past the track source end is rejected.
		bad = gapIslandSource(t, 1000, 1000, []mp4.ElstEntry{r1(100, 0)})
		_, _, err = runGapPreserve(bad)
		c.Check(err != nil, "edit past source end must error")

		// Zero media timescale is rejected.
		b = newGapBuilder(t, 1000, 0)
		b.frag(1, gapS{dts: 0, dur: 10})
		_, _, err = runGapPreserve(b.bytes())
		c.Check(err != nil, "zero media timescale must error")

		// Mixed file: track 1 keeps a mapped original elst while track 2 has
		// none and gets synthesized blanks; both stay on one movie axis.
		b = newGapBuilder(t, 1000, 1000, 40000)
		b.addElst(1, r1(60, 0))
		b.frag(1, gapS{dts: 0, dur: 10}, gapS{dts: 10, dur: 10}).
			frag(1, gapS{dts: 40, dur: 10}, gapS{dts: 50, dur: 10}).
			frag(2, gapS{dts: 0, dur: 400}, gapS{dts: 400, dur: 400}).
			frag(2, gapS{dts: 1600, dur: 400}, gapS{dts: 2000, dur: 400})
		_, f, err = runGapPreserve(b.bytes())
		c.Check(err == nil, "mixed elst/no-elst tracks: %v", err)
		if err == nil {
			want1 := []mp4.ElstEntry{r1(20, 0), empty(20), r1(20, 20)}
			c.Check(elstEqual(gapElst(gapTrak(f, 1)), want1), "mixed track1 elst %+v", gapElst(gapTrak(f, 1)))
			want2 := []mp4.ElstEntry{r1(20, 0), empty(20), r1(20, 800)}
			c.Check(elstEqual(gapElst(gapTrak(f, 2)), want2), "mixed track2 elst %+v", gapElst(gapTrak(f, 2)))
			c.Check(gapTrak(f, 1).Tkhd.Duration == 60 && gapTrak(f, 2).Tkhd.Duration == 60 &&
				f.Moov.Mvhd.Duration == 60, "mixed durations %d/%d mvhd %d",
				gapTrak(f, 1).Tkhd.Duration, gapTrak(f, 2).Tkhd.Duration, f.Moov.Mvhd.Duration)
		}

		// Common movie zero: the later-starting track gets a leading empty
		// edit instead of being re-zeroed on its own timeline.
		b = newGapBuilder(t, 1000, 1000, 1000)
		b.frag(1, gapS{dts: 100, dur: 10}, gapS{dts: 110, dur: 10})
		b.frag(2, gapS{dts: 0, dur: 10}, gapS{dts: 10, dur: 10})
		_, f, err = runGapPreserve(b.bytes())
		c.Check(err == nil, "common-zero sync: %v", err)
		if err == nil {
			wantLater := []mp4.ElstEntry{empty(100), r1(20, 0)}
			c.Check(elstEqual(gapElst(gapTrak(f, 1)), wantLater), "later track elst %+v", gapElst(gapTrak(f, 1)))
			c.Check(gapElst(gapTrak(f, 2)) == nil, "earliest trivial track needs no elst")
			c.Check(gapTrak(f, 1).Tkhd.Duration == 120 && f.Moov.Mvhd.Duration == 120,
				"common-zero durations tkhd1=%d mvhd=%d", gapTrak(f, 1).Tkhd.Duration, f.Moov.Mvhd.Duration)
		}
	})
}

func gapFullSample(dts uint64, dur uint32, data []byte, nonSync bool) mp4.FullSample {
	flags := defragSyncFlags()
	if nonSync {
		flags = defragNonSyncFlags()
	}
	return mp4.FullSample{
		Sample:     mp4.Sample{Flags: flags, Dur: dur, Size: uint32(len(data))},
		DecodeTime: dts,
		Data:       data,
	}
}

func TestGapRetransmit(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// A later retransmission starts in the hole and re-declares through
		// the old fragment end (the coverage rule), so the hole is fully
		// filled: the earlier C sample is superseded and no empty edit remains.
		b := newGapBuilder(t, 1000, 1000)
		pA, pB := []byte("sA"), []byte("sB")
		pC := []byte("sC")
		pX, pY, pZ := []byte("sX"), []byte("sY"), []byte("sZ")
		b.frag(1, gapS{dts: 0, dur: 10, payload: pA}, gapS{dts: 10, dur: 10, payload: pB}).
			frag(1, gapS{dts: 40, dur: 10, payload: pC}).
			frag(1,
				gapS{dts: 20, dur: 10, payload: pX},
				gapS{dts: 30, dur: 10, payload: pY},
				gapS{dts: 40, dur: 10, payload: pZ})
		out, f, err := runGapPreserve(b.bytes())
		c.Check(err == nil, "full hole refill: %v", err)
		if err == nil {
			v := gapTrak(f, 1)
			d, derr := gapDurs(v)
			c.Check(derr == nil && fmt.Sprint(d) == "[10 10 10 10 10]", "refill durations %v err=%v", d, derr)
			c.Check(v.Mdia.Mdhd.Duration == 50, "refill mdhd %d want 50", v.Mdia.Mdhd.Duration)
			c.Check(gapElst(v) == nil, "fully refilled hole must leave no elst, got %+v", gapElst(v))
			got, perr := gapRawPayload(out, v)
			want := bytes.Join([][]byte{pA, pB, pX, pY, pZ}, nil)
			c.Check(perr == nil && bytes.Equal(got, want), "refill payload/addresses ok=%v err=%v", bytes.Equal(got, want), perr)
			mdat := gapMdat(f)
			for _, p := range [][]byte{pA, pB, pX, pY, pZ} {
				c.Check(bytes.Count(mdat, p) == 1, "survivor %q count %d", p, bytes.Count(mdat, p))
			}
			c.Check(bytes.Count(mdat, pC) == 0, "superseded %q must be absent", pC)
		}

		// A retransmission starting inside the hole leaves exactly one
		// residual gap [20,30) while covering the old [40,60) tail.
		b = newGapBuilder(t, 1000, 1000)
		qA, qB, qC, qD := []byte("qA"), []byte("qB"), []byte("qC"), []byte("qD")
		qX, qY, qZ := []byte("qX"), []byte("qY"), []byte("qZ")
		b.frag(1, gapS{dts: 0, dur: 10, payload: qA}, gapS{dts: 10, dur: 10, payload: qB}).
			frag(1, gapS{dts: 40, dur: 10, payload: qC}, gapS{dts: 50, dur: 10, payload: qD}).
			frag(1,
				gapS{dts: 30, dur: 10, nonSync: true, payload: qX},
				gapS{dts: 40, dur: 10, nonSync: true, payload: qY},
				gapS{dts: 50, dur: 10, nonSync: true, payload: qZ})
		out, f, err = runGapPreserve(b.bytes())
		c.Check(err == nil, "partial retransmit: %v", err)
		if err == nil {
			v := gapTrak(f, 1)
			d, _ := gapDurs(v)
			c.Check(fmt.Sprint(d) == "[10 10 10 10 10]", "partial real durations %v", d)
			wantElst := []mp4.ElstEntry{
				{SegmentDuration: 20, MediaTime: 0, MediaRateInteger: 1},
				{SegmentDuration: 10, MediaTime: -1, MediaRateInteger: 1},
				{SegmentDuration: 30, MediaTime: 20, MediaRateInteger: 1},
			}
			c.Check(elstEqual(gapElst(v), wantElst), "partial elst %+v", gapElst(v))
			if stss := v.Mdia.Minf.Stbl.Stss; stss != nil {
				c.Check(fmt.Sprint(stss.SampleNumber) == "[1 2]", "stss %v want [1 2]", stss.SampleNumber)
			} else {
				c.Check(false, "expected stss listing the two sync samples")
			}
			got, perr := gapRawPayload(out, v)
			want := bytes.Join([][]byte{qA, qB, qX, qY, qZ}, nil)
			c.Check(perr == nil && bytes.Equal(got, want), "partial payload ok=%v err=%v", bytes.Equal(got, want), perr)
			c.Check(bytes.Count(gapMdat(f), qC) == 0 && bytes.Count(gapMdat(f), qD) == 0,
				"superseded C/D bytes must be absent")
		}

		// Reordering that crosses the hole is explicitly rejected.
		b = newGapBuilder(t, 1000, 1000)
		b.frag(1, gapS{dts: 0, dur: 10}, gapS{dts: 10, dur: 10, cts: 15}).
			frag(1, gapS{dts: 40, dur: 10})
		_, _, err = runGapPreserve(b.bytes())
		c.Check(err != nil, "cross-hole PTS reorder must be rejected")

		// Ordinary in-island B-frame reordering is accepted and CTS survives.
		b = newGapBuilder(t, 1000, 1000)
		b.frag(1,
			gapS{dts: 0, dur: 10, cts: 20},
			gapS{dts: 10, dur: 10, cts: -10, nonSync: true},
			gapS{dts: 20, dur: 10, cts: -10, nonSync: true},
			gapS{dts: 30, dur: 10, cts: 0})
		_, f, err = runGapPreserve(b.bytes())
		c.Check(err == nil, "in-island B frames: %v", err)
		if err == nil {
			cts, cerr := gapCTS(gapTrak(f, 1))
			c.Check(cerr == nil && fmt.Sprint(cts) == "[20 -10 -10 0]", "CTS preserved %v err=%v", cts, cerr)
			c.Check(gapElst(gapTrak(f, 1)) == nil, "single B-frame island needs no elst")
		}

		// Mixed input with an existing progressive MP4 prefix: gap after the
		// prefix, then a retransmission that fills it.
		pb := newGapBuilder(t, 1000, 1000)
		preA := []byte("preA01")
		preB := []byte("preB02")
		pb.frag(1, gapS{dts: 0, dur: 10, payload: preA}, gapS{dts: 10, dur: 10, payload: preB})
		progressive, perr := defragment(t, pb.bytes())
		c.Check(perr == nil, "old defrag prefix: %v", perr)
		if perr == nil {
			hybrid := bytes.NewBuffer(append([]byte{}, progressive.Bytes()...))
			addDefragFragment(t, hybrid, 2, 1, []mp4.FullSample{
				gapFullSample(40, 10, []byte("hyG"), false),
				gapFullSample(50, 10, []byte("hyH"), false),
			})
			addDefragFragment(t, hybrid, 3, 1, []mp4.FullSample{
				gapFullSample(20, 10, []byte("hyX"), false),
				gapFullSample(30, 10, []byte("hyY"), false),
				gapFullSample(40, 10, []byte("hyZ"), false),
				gapFullSample(50, 10, []byte("hyW"), false),
			})
			out, hf, herr := runGapPreserve(hybrid.Bytes())
			c.Check(herr == nil, "hybrid preserve-gaps: %v", herr)
			if herr == nil {
				v := gapTrak(hf, 1)
				d, _ := gapDurs(v)
				c.Check(fmt.Sprint(d) == "[10 10 10 10 10 10]", "hybrid durations %v", d)
				c.Check(gapElst(v) == nil, "filled hybrid gap needs no elst, got %+v", gapElst(v))
				got, rerr := gapRawPayload(out, v)
				want := bytes.Join([][]byte{preA, preB, []byte("hyX"), []byte("hyY"), []byte("hyZ"), []byte("hyW")}, nil)
				c.Check(rerr == nil && bytes.Equal(got, want), "hybrid physical payload ok=%v err=%v",
					bytes.Equal(got, want), rerr)
				c.Check(bytes.Count(gapMdat(hf), []byte("hyG")) == 0, "superseded hybrid G absent")
			}
		}

		// Unresolvable overlap (later fragment starts inside a real sample)
		// is still rejected through the shared collector.
		b = newGapBuilder(t, 1000, 1000)
		b.frag(1, gapS{dts: 0, dur: 10}, gapS{dts: 10, dur: 10}).
			frag(1, gapS{dts: 5, dur: 10})
		_, _, err = runGapPreserve(b.bytes())
		c.Check(err != nil, "inside-sample overlap must error")
	})
}
