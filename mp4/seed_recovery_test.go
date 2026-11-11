package mp4_test

import (
	"bytes"
	"fmt"
	"testing"

	"github.com/Eyevinn/mp4ff/mp4"
)

// These witnesses describe the unchanged starting point, not the desired result.
func seedRecoveryInit(t *testing.T) *mp4.InitSegment {
	t.Helper()
	i := mp4.CreateEmptyInit()
	for n := 0; n < 2; n++ {
		tr := i.AddEmptyTrack(48000, "audio", "und")
		if err := tr.SetAACDescriptor(2, 48000); err != nil {
			t.Fatal(err)
		}
	}
	return i
}

func seedRecoveryDesc(kind string, distance int16) *mp4.SgpdBox {
	var entry mp4.SampleGroupEntry = &mp4.RollSampleGroupEntry{RollDistance: distance}
	if kind == "prol" {
		entry = &mp4.UnknownSampleGroupEntry{Name: kind, Data: []byte{byte(uint16(distance) >> 8), byte(distance)}}
	}
	return &mp4.SgpdBox{Version: 1, GroupingType: kind, DefaultLength: 2,
		SampleGroupEntries: []mp4.SampleGroupEntry{entry}}
}

func seedRecoveryFrag(t *testing.T, seq, track uint32, start uint64, payload string) *mp4.Fragment {
	t.Helper()
	f, err := mp4.CreateFragment(seq, track)
	if err != nil {
		t.Fatal(err)
	}
	for n := range []byte(payload) {
		f.AddFullSample(mp4.FullSample{Sample: mp4.NewSample(0, 1024, 1, 0),
			DecodeTime: start + uint64(n)*1024, Data: []byte{payload[n]}})
	}
	return f
}

func seedRecoveryGroup(t *testing.T, f *mp4.Fragment, kind string, distance int16, indices ...uint32) {
	t.Helper()
	if err := f.Moof.Traf.AddChild(seedRecoveryDesc(kind, distance)); err != nil {
		t.Fatal(err)
	}
	counts := make([]uint32, len(indices))
	for n := range counts {
		counts[n] = 1
	}
	if err := f.Moof.Traf.AddChild(&mp4.SbgpBox{GroupingType: kind, SampleCounts: counts,
		GroupDescriptionIndices: indices}); err != nil {
		t.Fatal(err)
	}
}

func seedRecoveryConvert(t *testing.T, i *mp4.InitSegment, fragments ...*mp4.Fragment) (*mp4.File, *mp4.File) {
	t.Helper()
	var in bytes.Buffer
	if err := i.Encode(&in); err != nil {
		t.Fatal(err)
	}
	for _, f := range fragments {
		if err := f.Encode(&in); err != nil {
			t.Fatal(err)
		}
	}
	r := bytes.NewReader(in.Bytes())
	f, err := mp4.DecodeFile(r, mp4.WithDecodeMode(mp4.DecModeLazyMdat))
	if err != nil {
		t.Fatal(err)
	}
	// Decode independently to retain the unmodified source metadata for observation.
	original, err := mp4.DecodeFile(bytes.NewReader(in.Bytes()))
	if err != nil {
		t.Fatal(err)
	}
	var out bytes.Buffer
	if err := mp4.Defragment(f, r, &out); err != nil {
		t.Fatal(err)
	}
	converted, err := mp4.DecodeFile(bytes.NewReader(out.Bytes()))
	if err != nil {
		t.Fatal(err)
	}
	if converted.IsFragmented() {
		t.Fatal("expected progressive output")
	}
	return original, converted
}

func seedRecoveryBytes(t *testing.T, f *mp4.File, track uint32) string {
	t.Helper()
	for _, tr := range f.Moov.Traks {
		if tr.Tkhd.TrackID != track {
			continue
		}
		ranges, err := tr.GetRangesForSampleInterval(1, tr.GetNrSamples())
		if err != nil {
			t.Fatal(err)
		}
		var data []byte
		for _, r := range ranges {
			start := r.Offset - f.Mdat.PayloadAbsoluteOffset()
			data = append(data, f.Mdat.Data[start:start+r.Size]...)
		}
		return string(data)
	}
	t.Fatalf("missing track %d", track)
	return ""
}

func TestSeedRecoveryGroupLoss(t *testing.T) {
	i := seedRecoveryInit(t)
	i.Moov.Traks[0].Mdia.Minf.Stbl.AddChild(seedRecoveryDesc("roll", -1))
	a := seedRecoveryFrag(t, 1, 1, 0, "AB")
	seedRecoveryGroup(t, a, "roll", -3, 1, 65537)
	seedRecoveryGroup(t, a, "prol", -2, 0, 65537)
	b := seedRecoveryFrag(t, 2, 2, 0, "xy")
	original, out := seedRecoveryConvert(t, i, a, b)
	if len(original.Moov.Traks[0].Mdia.Minf.Stbl.Sgpds) != 1 {
		t.Fatal("source global description missing")
	}
	groups := 0
	for _, box := range original.Segments[0].Fragments[0].Moof.Traf.Children {
		if box.Type() == "sbgp" {
			groups++
		}
	}
	stbl := out.Moov.Traks[0].Mdia.Minf.Stbl
	if groups != 2 || len(stbl.Sgpds) != 0 || len(stbl.Sbgps) != 0 {
		t.Fatalf("unexpected starting-point groups: input=%d output sgpd=%d sbgp=%d", groups, len(stbl.Sgpds), len(stbl.Sbgps))
	}
	if got := seedRecoveryBytes(t, out, 1); got != "AB" {
		t.Fatalf("payload %q", got)
	}
	if got := seedRecoveryBytes(t, out, 2); got != "xy" {
		t.Fatalf("second track payload %q", got)
	}
	fmt.Println("MP4FF_SEED_GROUPS input_global=1 input_local_types=2 output_groups=0 payload=AB/xy")
}

func TestSeedRecoveryGroupOverlap(t *testing.T) {
	i := seedRecoveryInit(t)
	a := seedRecoveryFrag(t, 1, 1, 0, "ABCD")
	seedRecoveryGroup(t, a, "roll", -1, 65537, 65537, 65537, 65537)
	b := seedRecoveryFrag(t, 2, 2, 0, "xy")
	c := seedRecoveryFrag(t, 3, 1, 2048, "EF")
	seedRecoveryGroup(t, c, "roll", -3, 65537, 65537)
	_, out := seedRecoveryConvert(t, i, a, b, c)
	if got := seedRecoveryBytes(t, out, 1); got != "ABEF" {
		t.Fatalf("winning sample bytes %q", got)
	}
	if got := seedRecoveryBytes(t, out, 2); got != "xy" {
		t.Fatalf("unrelated track bytes %q", got)
	}
	if len(out.Moov.Traks[0].Mdia.Minf.Stbl.Sbgps) != 0 {
		t.Fatal("starting point unexpectedly retained recovery groups")
	}
	fmt.Println("MP4FF_SEED_OVERLAP surviving_payload=ABEF other_track=xy output_groups=0")
}
