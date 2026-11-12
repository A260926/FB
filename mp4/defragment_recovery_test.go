package mp4_test

import (
	"bytes"
	"encoding/binary"
	"strings"
	"testing"

	"github.com/Eyevinn/mp4ff/mp4"
)

// recoveryExpectation is one output sample's group state: hasGroup is false
// for sbgp index 0 (no group), which is distinct from a real distance of 0.
type recoveryExpectation struct {
	hasGroup bool
	distance int16
}

// recoveryInit builds an init segment with nr empty AAC tracks.
func recoveryInit(t *testing.T, nr int) *mp4.InitSegment {
	t.Helper()
	init := mp4.CreateEmptyInit()
	for n := 0; n < nr; n++ {
		tr := init.AddEmptyTrack(48000, "audio", "und")
		if err := tr.SetAACDescriptor(2, 48000); err != nil {
			t.Fatal(err)
		}
	}
	return init
}

// recoverySample is one byte-sized sample on the 48000-tick audio timeline.
func recoverySample(payload byte, decodeTime uint64) mp4.FullSample {
	return mp4.FullSample{
		Sample:     mp4.NewSample(0, 1024, 1, 0),
		DecodeTime: decodeTime,
		Data:       []byte{payload},
	}
}

// recoveryFragment builds a single-trun fragment of the given payload bytes.
func recoveryFragment(t *testing.T, seqNr, trackID uint32, start uint64, payload string) *mp4.Fragment {
	t.Helper()
	frag, err := mp4.CreateFragment(seqNr, trackID)
	if err != nil {
		t.Fatal(err)
	}
	for n := range []byte(payload) {
		frag.AddFullSample(recoverySample(payload[n], start+uint64(n)*1024))
	}
	return frag
}

// twoRunFragment builds a fragment whose traf has two truns, with the samples
// split runSamples[0] and runSamples[1] in decode order.
func twoRunFragment(t *testing.T, seqNr, trackID uint32, start uint64, payload string,
	runSamples int) *mp4.Fragment {
	t.Helper()
	frag, err := mp4.CreateFragment(seqNr, trackID)
	if err != nil {
		t.Fatal(err)
	}
	traf := frag.Moof.Traf
	for n := 0; n < runSamples; n++ {
		frag.AddFullSample(recoverySample(payload[n], start+uint64(n)*1024))
	}
	secondTrun := mp4.CreateTrun(1)
	for n := runSamples; n < len(payload); n++ {
		secondTrun.AddSample(mp4.NewSample(0, 1024, 1, 0))
		frag.Mdat.AddSampleData([]byte{payload[n]})
	}
	if err := traf.AddChild(secondTrun); err != nil {
		t.Fatal(err)
	}
	return frag
}

// addRecoveryGroup appends a local sgpd/sbgp pair to a traf. The sgpd carries
// the distances in order (local index 1, 2, ...) and sbgp gives one
// group_description_index per sample, counted across every trun of the traf.
func addRecoveryGroup(t *testing.T, traf *mp4.TrafBox, kind string, distances []int16, indices []uint32) {
	t.Helper()
	entries := make([]mp4.SampleGroupEntry, 0, len(distances))
	for _, distance := range distances {
		entries = append(entries, recoveryEntry(kind, distance))
	}
	if err := traf.AddChild(&mp4.SgpdBox{
		Version: 1, GroupingType: kind, DefaultLength: 2, SampleGroupEntries: entries,
	}); err != nil {
		t.Fatal(err)
	}
	counts := make([]uint32, len(indices))
	for n := range counts {
		counts[n] = 1
	}
	if err := traf.AddChild(&mp4.SbgpBox{
		GroupingType: kind, SampleCounts: counts, GroupDescriptionIndices: indices,
	}); err != nil {
		t.Fatal(err)
	}
}

// addGlobalRecoverySgpd adds a track-level sgpd without an sbgp: fragment sbgp
// boxes address its descriptions with indices 1..65535.
func addGlobalRecoverySgpd(t *testing.T, stbl *mp4.StblBox, kind string, distances []int16) {
	t.Helper()
	entries := make([]mp4.SampleGroupEntry, 0, len(distances))
	for _, distance := range distances {
		entries = append(entries, recoveryEntry(kind, distance))
	}
	stbl.AddChild(&mp4.SgpdBox{
		Version: 1, GroupingType: kind, DefaultLength: 2, SampleGroupEntries: entries,
	})
}

// addGlobalRecoveryGroup appends a track-level sgpd/sbgp pair to a stbl.
func addGlobalRecoveryGroup(t *testing.T, stbl *mp4.StblBox, kind string, distances []int16, indices []uint32) {
	t.Helper()
	entries := make([]mp4.SampleGroupEntry, 0, len(distances))
	for _, distance := range distances {
		entries = append(entries, recoveryEntry(kind, distance))
	}
	stbl.AddChild(&mp4.SgpdBox{
		Version: 1, GroupingType: kind, DefaultLength: 2, SampleGroupEntries: entries,
	})
	counts := make([]uint32, len(indices))
	for n := range counts {
		counts[n] = 1
	}
	stbl.AddChild(&mp4.SbgpBox{
		GroupingType: kind, SampleCounts: counts, GroupDescriptionIndices: indices,
	})
}

// recoveryEntry builds the two-byte sample group entry of one recovery type.
func recoveryEntry(kind string, distance int16) mp4.SampleGroupEntry {
	if kind == "roll" {
		return &mp4.RollSampleGroupEntry{RollDistance: distance}
	}
	return &mp4.UnknownSampleGroupEntry{
		Name: kind,
		Data: []byte{byte(uint16(distance) >> 8), byte(distance)},
	}
}

// unknownBox wraps a hand-written box payload, so that a test can feed bytes
// that this library never produced into a real encode/DecodeFile round trip.
func unknownBox(boxType string, payload []byte) *mp4.UnknownBox {
	return mp4.CreateUnknownBox(boxType, uint64(8+len(payload)), payload)
}

func sgpdPayload(kind string, version byte, defaultIndex uint32, distances ...int16) []byte {
	payload := []byte{version, 0, 0, 0}
	payload = append(payload, kind...)
	payload = binary.BigEndian.AppendUint32(payload, 2) // default_length
	if version >= 2 {
		payload = binary.BigEndian.AppendUint32(payload, defaultIndex)
	}
	payload = binary.BigEndian.AppendUint32(payload, uint32(len(distances)))
	for _, distance := range distances {
		payload = binary.BigEndian.AppendUint16(payload, uint16(distance))
	}
	return payload
}

func sbgpPayload(kind string, version byte, parameter uint32, runs [][2]uint32) []byte {
	payload := []byte{version, 0, 0, 0}
	payload = append(payload, kind...)
	if version >= 1 {
		payload = binary.BigEndian.AppendUint32(payload, parameter)
	}
	payload = binary.BigEndian.AppendUint32(payload, uint32(len(runs)))
	for _, run := range runs {
		payload = binary.BigEndian.AppendUint32(payload, run[0]) // sample_count
		payload = binary.BigEndian.AppendUint32(payload, run[1]) // group_description_index
	}
	return payload
}

// runRecovery encodes init and the fragments, decodes the file the way the
// service does, runs the public defragmentation entry, and decodes the
// progressive output. The buffer also reports whether the writer got bytes.
func runRecovery(t *testing.T, init *mp4.InitSegment, fragments []*mp4.Fragment,
	trackIDs ...uint32) (*mp4.File, *bytes.Buffer, error) {
	t.Helper()
	var in bytes.Buffer
	if err := init.Encode(&in); err != nil {
		t.Fatal(err)
	}
	for _, frag := range fragments {
		if err := frag.Encode(&in); err != nil {
			t.Fatal(err)
		}
	}
	rs := bytes.NewReader(in.Bytes())
	f, err := mp4.DecodeFile(rs, mp4.WithDecodeMode(mp4.DecModeLazyMdat))
	if err != nil {
		t.Fatal(err)
	}
	var out bytes.Buffer
	if len(trackIDs) == 0 {
		err = mp4.Defragment(f, rs, &out)
	} else {
		err = mp4.DefragmentTracks(f, rs, &out, trackIDs...)
	}
	if err != nil {
		return nil, &out, err
	}
	converted, derr := mp4.DecodeFile(bytes.NewReader(out.Bytes()))
	if derr != nil {
		t.Fatal(derr)
	}
	if converted.IsFragmented() {
		t.Fatal("expected progressive output")
	}
	return converted, &out, nil
}

// readRecoveryGroup independently reads one recovery group back from the
// output stbl: the sgpd dictionary as signed distances, the sbgp runs, and the
// per-sample state after applying the runs. present is false when the type has
// no box pair, which must mean it has no members.
func readRecoveryGroup(t *testing.T, f *mp4.File, trackID uint32, kind string,
	nrSamples int) (dictionary []int16, counts, indices []uint32, groups []recoveryExpectation, present bool) {
	t.Helper()
	var stbl *mp4.StblBox
	for _, trak := range f.Moov.Traks {
		if trak.Tkhd.TrackID == trackID {
			stbl = trak.Mdia.Minf.Stbl
		}
	}
	if stbl == nil {
		t.Fatalf("track %d missing", trackID)
	}
	var sgpd *mp4.SgpdBox
	for _, candidate := range stbl.Sgpds {
		if candidate.GroupingType == kind {
			sgpd = candidate
		}
	}
	var sbgp *mp4.SbgpBox
	for _, candidate := range stbl.Sbgps {
		if candidate.GroupingType == kind {
			sbgp = candidate
		}
	}
	if (sgpd == nil) != (sbgp == nil) {
		t.Fatalf("track %d %s: sgpd and sbgp must be written together", trackID, kind)
	}
	if sgpd == nil {
		return nil, nil, nil, nil, false
	}
	if sgpd.Version != 1 || sgpd.DefaultLength != 2 {
		t.Errorf("track %d %s sgpd: want version 1 default_length 2, got version %d length %d",
			trackID, kind, sgpd.Version, sgpd.DefaultLength)
	}
	if sbgp.Version != 0 {
		t.Errorf("track %d %s sbgp: want version 0, got %d", trackID, kind, sbgp.Version)
	}
	for i, entry := range sgpd.SampleGroupEntries {
		switch e := entry.(type) {
		case *mp4.RollSampleGroupEntry:
			if kind != "roll" {
				t.Fatalf("track %d %s description %d has roll entry", trackID, kind, i+1)
			}
			dictionary = append(dictionary, e.RollDistance)
		case *mp4.UnknownSampleGroupEntry:
			if len(e.Data) != 2 {
				t.Fatalf("track %d %s description %d is %d bytes, want 2", trackID, kind, i+1, len(e.Data))
			}
			dictionary = append(dictionary, int16(binary.BigEndian.Uint16(e.Data)))
		default:
			t.Fatalf("track %d %s description %d has unexpected entry type %T", trackID, kind, i+1, entry)
		}
	}
	if len(sbgp.SampleCounts) != len(sbgp.GroupDescriptionIndices) {
		t.Fatalf("track %d %s: %d runs but %d indices", trackID, kind,
			len(sbgp.SampleCounts), len(sbgp.GroupDescriptionIndices))
	}
	total := uint32(0)
	for run, count := range sbgp.SampleCounts {
		if count == 0 {
			t.Fatalf("track %d %s run %d has zero count", trackID, kind, run+1)
		}
		total += count
		index := sbgp.GroupDescriptionIndices[run]
		counts = append(counts, count)
		indices = append(indices, index)
		for range count {
			if index == 0 {
				groups = append(groups, recoveryExpectation{})
				continue
			}
			if int(index) > len(dictionary) {
				t.Fatalf("track %d %s run %d refers to missing description %d", trackID, kind, run+1, index)
			}
			groups = append(groups, recoveryExpectation{hasGroup: true, distance: dictionary[index-1]})
		}
	}
	if int(total) != nrSamples {
		t.Fatalf("track %d %s runs count %d samples, want %d", trackID, kind, total, nrSamples)
	}
	return dictionary, counts, indices, groups, true
}

func wantGroups(values []int16, noGroup ...int) []recoveryExpectation {
	none := make(map[int]bool)
	for _, n := range noGroup {
		none[n] = true
	}
	groups := make([]recoveryExpectation, len(values))
	for i, distance := range values {
		if none[i] {
			continue
		}
		groups[i] = recoveryExpectation{hasGroup: true, distance: distance}
	}
	return groups
}

// TestDefragmentRecoveryGroups covers one traf with multiple truns counted
// continuously, global and fragment-local descriptions sharing an index with
// different values, both recovery types in one traf, no-group samples next to
// a real distance of 0, and a second track with its own groups.
func TestDefragmentRecoveryGroups(t *testing.T) {
	init := recoveryInit(t, 2)
	stbl := init.Moov.Traks[0].Mdia.Minf.Stbl
	addGlobalRecoverySgpd(t, stbl, "roll", []int16{-7})

	// Two truns of two samples each. The sbgp samples are counted across both.
	fragA := twoRunFragment(t, 1, 1, 0, "ABCD", 2)
	// roll: global -7, local -3, no group, global -7.
	addRecoveryGroup(t, fragA.Moof.Traf, "roll",
		[]int16{-3}, []uint32{1, 65537, 0, 1})
	// prol: real distance 0, no group, local distance -2, distance 0 again.
	addRecoveryGroup(t, fragA.Moof.Traf, "prol",
		[]int16{0, -2}, []uint32{65537, 0, 65538, 65537})

	fragB := recoveryFragment(t, 2, 2, 0, "xy")
	addRecoveryGroup(t, fragB.Moof.Traf, "roll",
		[]int16{-9}, []uint32{65537, 0})

	out, _, err := runRecovery(t, init, []*mp4.Fragment{fragA, fragB})
	if err != nil {
		t.Fatal(err)
	}
	if got := seedRecoveryBytes(t, out, 1); got != "ABCD" {
		t.Fatalf("track 1 payload %q", got)
	}
	if got := seedRecoveryBytes(t, out, 2); got != "xy" {
		t.Fatalf("track 2 payload %q", got)
	}

	rollDict, rollCounts, rollIndices, rollGroups, ok := readRecoveryGroup(t, out, 1, "roll", 4)
	if !ok {
		t.Fatal("track 1 roll group missing")
	}
	if want := []int16{-7, -3}; !equalInt16(rollDict, want) {
		t.Fatalf("roll dictionary %v, want %v", rollDict, want)
	}
	if want := wantGroups([]int16{-7, -3, 0, -7}, 2); !equalExpectation(rollGroups, want) {
		t.Fatalf("roll groups %v, want %v", rollGroups, want)
	}
	if want := [][2]uint32{{1, 1}, {1, 2}, {1, 0}, {1, 1}}; !equalRuns(rollCounts, rollIndices, want) {
		t.Fatalf("roll runs counts=%v indices=%v", rollCounts, rollIndices)
	}

	prolDict, prolCounts, prolIndices, prolGroups, ok := readRecoveryGroup(t, out, 1, "prol", 4)
	if !ok {
		t.Fatal("track 1 prol group missing")
	}
	if want := []int16{0, -2}; !equalInt16(prolDict, want) {
		t.Fatalf("prol dictionary %v, want %v (distance 0 is a real description)", prolDict, want)
	}
	if want := wantGroups([]int16{0, 0, -2, 0}, 1); !equalExpectation(prolGroups, want) {
		t.Fatalf("prol groups %v, want %v", prolGroups, want)
	}
	if want := [][2]uint32{{1, 1}, {1, 0}, {1, 2}, {1, 1}}; !equalRuns(prolCounts, prolIndices, want) {
		t.Fatalf("prol runs counts=%v indices=%v", prolCounts, prolIndices)
	}

	track2Dict, _, _, track2Groups, ok := readRecoveryGroup(t, out, 2, "roll", 2)
	if !ok {
		t.Fatal("track 2 roll group missing")
	}
	if want := []int16{-9}; !equalInt16(track2Dict, want) {
		t.Fatalf("track 2 dictionary %v, want %v", track2Dict, want)
	}
	if want := wantGroups([]int16{-9, 0}, 1); !equalExpectation(track2Groups, want) {
		t.Fatalf("track 2 groups %v, want %v", track2Groups, want)
	}
	if _, _, _, _, present := readRecoveryGroup(t, out, 2, "prol", 2); present {
		t.Fatal("track 2 has no prol members, so no prol boxes must be written")
	}
}

// TestDefragmentRecoveryOverlap pins that recovery groups follow the existing
// retransmission rules, including repeated resends, tfdt gap padding,
// unaffected tracks and independently hand-written group boxes.
func TestDefragmentRecoveryOverlap(t *testing.T) {
	t.Run("tail resend gives ABCD/EF example", func(t *testing.T) {
		init := recoveryInit(t, 2)
		first := recoveryFragment(t, 1, 1, 0, "ABCD")
		addRecoveryGroup(t, first.Moof.Traf, "roll",
			[]int16{-1}, []uint32{65537, 65537, 65537, 65537})
		other := recoveryFragment(t, 2, 2, 0, "xy")
		resend := recoveryFragment(t, 3, 1, 2048, "EF")
		addRecoveryGroup(t, resend.Moof.Traf, "roll",
			[]int16{-3}, []uint32{65537, 65537})

		out, _, err := runRecovery(t, init, []*mp4.Fragment{first, other, resend})
		if err != nil {
			t.Fatal(err)
		}
		if got := seedRecoveryBytes(t, out, 1); got != "ABEF" {
			t.Fatalf("payload %q, want ABEF", got)
		}
		if got := seedRecoveryBytes(t, out, 2); got != "xy" {
			t.Fatalf("other track payload %q", got)
		}
		dict, counts, indices, groups, ok := readRecoveryGroup(t, out, 1, "roll", 4)
		if !ok {
			t.Fatal("roll group missing after resend")
		}
		if want := []int16{-1, -3}; !equalInt16(dict, want) {
			t.Fatalf("dictionary %v, want %v", dict, want)
		}
		if want := wantGroups([]int16{-1, -1, -3, -3}); !equalExpectation(groups, want) {
			t.Fatalf("groups %v, want [-1 -1 -3 -3]", groups)
		}
		if want := [][2]uint32{{2, 1}, {2, 2}}; !equalRuns(counts, indices, want) {
			t.Fatalf("runs counts=%v indices=%v, want %v", counts, indices, want)
		}
		if _, _, _, _, present := readRecoveryGroup(t, out, 2, "roll", 2); present {
			t.Fatal("track without roll members must not get roll boxes")
		}
	})

	t.Run("consecutive resends keep the last declaration", func(t *testing.T) {
		init := recoveryInit(t, 1)
		first := recoveryFragment(t, 1, 1, 0, "ABCD")
		addRecoveryGroup(t, first.Moof.Traf, "roll",
			[]int16{-1}, []uint32{65537, 65537, 65537, 65537})
		second := recoveryFragment(t, 2, 1, 2048, "EF")
		addRecoveryGroup(t, second.Moof.Traf, "roll",
			[]int16{-3}, []uint32{65537, 65537})
		third := recoveryFragment(t, 3, 1, 2048, "IJ")
		addRecoveryGroup(t, third.Moof.Traf, "roll",
			[]int16{-5}, []uint32{65537, 65537})

		out, _, err := runRecovery(t, init, []*mp4.Fragment{first, second, third})
		if err != nil {
			t.Fatal(err)
		}
		if got := seedRecoveryBytes(t, out, 1); got != "ABIJ" {
			t.Fatalf("payload %q, want ABIJ", got)
		}
		dict, counts, indices, groups, ok := readRecoveryGroup(t, out, 1, "roll", 4)
		if !ok {
			t.Fatal("roll group missing")
		}
		if want := []int16{-1, -5}; !equalInt16(dict, want) {
			t.Fatalf("dictionary %v, want %v", dict, want)
		}
		if want := wantGroups([]int16{-1, -1, -5, -5}); !equalExpectation(groups, want) {
			t.Fatalf("groups %v", groups)
		}
		if want := [][2]uint32{{2, 1}, {2, 2}}; !equalRuns(counts, indices, want) {
			t.Fatalf("runs counts=%v indices=%v", counts, indices)
		}
	})

	t.Run("forward gap pads time but never groups", func(t *testing.T) {
		init := recoveryInit(t, 1)
		first := recoveryFragment(t, 1, 1, 0, "AB")
		addRecoveryGroup(t, first.Moof.Traf, "roll",
			[]int16{-1}, []uint32{65537, 65537})
		// Starts at 4096 while the first fragment ends at 2048: the previous
		// sample's duration is extended by 2048, no sample is inserted.
		gapped := recoveryFragment(t, 2, 1, 4096, "CD")
		addRecoveryGroup(t, gapped.Moof.Traf, "roll",
			[]int16{-3}, []uint32{65537, 65537})

		out, _, err := runRecovery(t, init, []*mp4.Fragment{first, gapped})
		if err != nil {
			t.Fatal(err)
		}
		stbl := out.Moov.Traks[0].Mdia.Minf.Stbl
		// RLE merges the final two 1024-tick samples; the gap is folded into
		// sample 2 (1024 + 2048), no sample is inserted for the empty time.
		if got, want := stbl.Stts.SampleCount, []uint32{1, 1, 2}; !equalUint32(got, want) {
			t.Fatalf("stts counts %v, want %v (four samples, no gap sample)", got, want)
		}
		if got, want := stbl.Stts.SampleTimeDelta, []uint32{1024, 3072, 1024}; !equalUint32(got, want) {
			t.Fatalf("stts deltas %v, want %v (gap folded into sample 2)", got, want)
		}
		_, counts, indices, groups, ok := readRecoveryGroup(t, out, 1, "roll", 4)
		if !ok {
			t.Fatal("roll group missing")
		}
		if want := wantGroups([]int16{-1, -1, -3, -3}); !equalExpectation(groups, want) {
			t.Fatalf("groups stay per sample: %v", groups)
		}
		if want := [][2]uint32{{2, 1}, {2, 2}}; !equalRuns(counts, indices, want) {
			t.Fatalf("runs counts=%v indices=%v", counts, indices)
		}
	})

	t.Run("hand-written group boxes round-trip", func(t *testing.T) {
		// Every group box here is assembled from raw bytes, not from this
		// library's encoders: a different writer must remain interoperable.
		// The input uses sgpd version 2 (default index 0) and sbgp version 1
		// (parameter 0), the highest supported input variants; the output is
		// still normalized to sgpd version 1 and sbgp version 0.
		init := recoveryInit(t, 1)
		frag := twoRunFragment(t, 1, 1, 0, "ABC", 2)
		traf := frag.Moof.Traf
		if err := traf.AddChild(unknownBox("sgpd", sgpdPayload("roll", 2, 0, -6))); err != nil {
			t.Fatal(err)
		}
		if err := traf.AddChild(unknownBox("sbgp", sbgpPayload("roll", 1, 0,
			[][2]uint32{{1, 65537}, {1, 0}, {1, 65537}}))); err != nil {
			t.Fatal(err)
		}
		out, _, err := runRecovery(t, init, []*mp4.Fragment{frag})
		if err != nil {
			t.Fatal(err)
		}
		if got := seedRecoveryBytes(t, out, 1); got != "ABC" {
			t.Fatalf("payload %q", got)
		}
		dict, counts, indices, groups, ok := readRecoveryGroup(t, out, 1, "roll", 3)
		if !ok {
			t.Fatal("hand-written roll group did not survive")
		}
		if want := []int16{-6}; !equalInt16(dict, want) {
			t.Fatalf("dictionary %v, want [-6]", dict)
		}
		if want := wantGroups([]int16{-6, 0, -6}, 1); !equalExpectation(groups, want) {
			t.Fatalf("groups %v", groups)
		}
		if want := [][2]uint32{{1, 1}, {1, 0}, {1, 1}}; !equalRuns(counts, indices, want) {
			t.Fatalf("runs counts=%v indices=%v", counts, indices)
		}
	})

	t.Run("runs cross truns and descriptions dedup across scopes", func(t *testing.T) {
		init := recoveryInit(t, 1)
		addGlobalRecoverySgpd(t, init.Moov.Traks[0].Mdia.Minf.Stbl, "roll", []int16{-1})
		// Two truns of two samples each. The local sgpd has a second
		// description that no run references.
		first := twoRunFragment(t, 1, 1, 0, "ABCD", 2)
		traf := first.Moof.Traf
		addRecoveryGroup(t, traf, "roll", []int16{-1, -9},
			[]uint32{65537, 65537, 0}) // one run of two across the trun boundary, then no group
		// Sample D is left without a covering run: the tail counts as no group.
		second := recoveryFragment(t, 2, 1, 4096, "EF")
		addRecoveryGroup(t, second.Moof.Traf, "roll", []int16{-1},
			[]uint32{1, 1}) // global description, same distance as the local one

		out, _, err := runRecovery(t, init, []*mp4.Fragment{first, second})
		if err != nil {
			t.Fatal(err)
		}
		if got := seedRecoveryBytes(t, out, 1); got != "ABCDEF" {
			t.Fatalf("payload %q, want ABCDEF", got)
		}
		dict, counts, indices, groups, ok := readRecoveryGroup(t, out, 1, "roll", 6)
		if !ok {
			t.Fatal("roll group missing")
		}
		// -9 was never referenced, and the local and global -1 merge.
		if want := []int16{-1}; !equalInt16(dict, want) {
			t.Fatalf("dictionary %v, want only the referenced [-1]", dict)
		}
		if want := wantGroups([]int16{-1, -1, 0, 0, -1, -1}, 2, 3); !equalExpectation(groups, want) {
			t.Fatalf("groups %v", groups)
		}
		if want := [][2]uint32{{2, 1}, {2, 0}, {2, 1}}; !equalRuns(counts, indices, want) {
			t.Fatalf("runs counts=%v indices=%v, want the no-group tail merged", counts, indices)
		}
	})

	t.Run("a type appearing only later and an all-no-group sbgp", func(t *testing.T) {
		init := recoveryInit(t, 1)
		// First fragment: prol sbgp exists but maps every sample to no group.
		first := recoveryFragment(t, 1, 1, 0, "AB")
		_ = first.Moof.Traf.AddChild(&mp4.SbgpBox{
			GroupingType: "prol", SampleCounts: []uint32{2}, GroupDescriptionIndices: []uint32{0},
		})
		// Second fragment is the first place where prol gets a real member.
		second := recoveryFragment(t, 2, 1, 2048, "CD")
		addRecoveryGroup(t, second.Moof.Traf, "prol", []int16{-4}, []uint32{65537, 65537})

		out, _, err := runRecovery(t, init, []*mp4.Fragment{first, second})
		if err != nil {
			t.Fatal(err)
		}
		dict, counts, indices, groups, ok := readRecoveryGroup(t, out, 1, "prol", 4)
		if !ok {
			t.Fatal("prol group missing even though later samples carry it")
		}
		if want := []int16{-4}; !equalInt16(dict, want) {
			t.Fatalf("dictionary %v, want [-4]", dict)
		}
		if want := wantGroups([]int16{0, 0, -4, -4}, 0, 1); !equalExpectation(groups, want) {
			t.Fatalf("groups %v", groups)
		}
		if want := [][2]uint32{{2, 0}, {2, 1}}; !equalRuns(counts, indices, want) {
			t.Fatalf("runs counts=%v indices=%v", counts, indices)
		}
		// roll never has a member anywhere, so no roll boxes are written.
		if _, _, _, _, present := readRecoveryGroup(t, out, 1, "roll", 4); present {
			t.Fatal("roll has no members, so no roll boxes must be written")
		}
	})

	t.Run("same local index in different fragments is a different description", func(t *testing.T) {
		init := recoveryInit(t, 1)
		first := recoveryFragment(t, 1, 1, 0, "AB")
		addRecoveryGroup(t, first.Moof.Traf, "roll", []int16{-1}, []uint32{65537, 65537})
		second := recoveryFragment(t, 2, 1, 2048, "CD")
		// A second traf reuses 65537, but its own sgpd gives another distance.
		addRecoveryGroup(t, second.Moof.Traf, "roll", []int16{-3}, []uint32{65537, 65537})

		out, _, err := runRecovery(t, init, []*mp4.Fragment{first, second})
		if err != nil {
			t.Fatal(err)
		}
		dict, _, _, groups, ok := readRecoveryGroup(t, out, 1, "roll", 4)
		if !ok {
			t.Fatal("roll group missing")
		}
		if want := []int16{-1, -3}; !equalInt16(dict, want) {
			t.Fatalf("dictionary %v, want the two fragment-local 65537 entries kept distinct", dict)
		}
		if want := wantGroups([]int16{-1, -1, -3, -3}); !equalExpectation(groups, want) {
			t.Fatalf("groups %v", groups)
		}
	})
}

// writeHybridRecoveryPrefix writes ftyp + moov with a four-sample progressive
// prefix (including roll and prol groups) and its mdat, ready for fragments to
// be appended.
func writeHybridRecoveryPrefix(t *testing.T) []byte {
	t.Helper()
	init := createDefragPlainAvInit(t)
	stbl := init.Moov.Traks[0].Mdia.Minf.Stbl
	stbl.Stts.SampleCount = []uint32{4}
	stbl.Stts.SampleTimeDelta = []uint32{512}
	stbl.Stsz.SampleNumber = 4
	stbl.Stsz.SampleSize = []uint32{1, 1, 1, 1}
	if err := stbl.Stsc.AddEntry(1, 2, 1); err != nil {
		t.Fatal(err)
	}
	stbl.Stco.ChunkOffset = []uint32{0, 0}
	addGlobalRecoveryGroup(t, stbl, "roll", []int16{-1}, []uint32{1, 1, 1, 1})
	// prol: no group, distance 2, distance 2, no group.
	addGlobalRecoveryGroup(t, stbl, "prol", []int16{2}, []uint32{0, 1, 1, 0})

	var measure bytes.Buffer
	if err := init.Encode(&measure); err != nil {
		t.Fatal(err)
	}
	payloadStart := uint32(measure.Len()) + 8
	stbl.Stco.ChunkOffset = []uint32{payloadStart, payloadStart + 2}

	var buf bytes.Buffer
	if err := init.Encode(&buf); err != nil {
		t.Fatal(err)
	}
	mdat := &mp4.MdatBox{Data: []byte("ABCD")}
	if err := mdat.Encode(&buf); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

// TestDefragmentRecoveryHybrid covers a progressive prefix whose tail is
// superseded by fragments: surviving prefix samples keep their stbl groups,
// superseded samples lose theirs, and new samples use fragment descriptions.
// It also exercises DefragmentTracks on the same input.
func TestDefragmentRecoveryHybrid(t *testing.T) {
	prefix := writeHybridRecoveryPrefix(t)
	var buf bytes.Buffer
	buf.Write(prefix)
	// The fragment re-declares [1024, 2048), superseding prefix samples 3-4.
	superseding := recoveryFragment(t, 1, 1, 1024, "EF")
	addRecoveryGroup(t, superseding.Moof.Traf, "roll",
		[]int16{-3}, []uint32{65537, 65537})
	addRecoveryGroup(t, superseding.Moof.Traf, "prol",
		[]int16{-4}, []uint32{65537, 65537})
	if err := superseding.Encode(&buf); err != nil {
		t.Fatal(err)
	}
	// The second track gets its own fragment so both tracks have samples.
	audio := recoveryFragment(t, 2, 2, 0, "xy")
	if err := audio.Encode(&buf); err != nil {
		t.Fatal(err)
	}

	rs := bytes.NewReader(buf.Bytes())
	f, err := mp4.DecodeFile(rs, mp4.WithDecodeMode(mp4.DecModeLazyMdat))
	if err != nil {
		t.Fatal(err)
	}
	var out bytes.Buffer
	if err := mp4.Defragment(f, rs, &out); err != nil {
		t.Fatal(err)
	}
	converted, err := mp4.DecodeFile(bytes.NewReader(out.Bytes()))
	if err != nil {
		t.Fatal(err)
	}
	if got := seedRecoveryBytes(t, converted, 1); got != "ABEF" {
		t.Fatalf("payload %q, want ABEF", got)
	}
	rollDict, _, _, rollGroups, ok := readRecoveryGroup(t, converted, 1, "roll", 4)
	if !ok {
		t.Fatal("roll group missing")
	}
	if want := []int16{-1, -3}; !equalInt16(rollDict, want) {
		t.Fatalf("roll dictionary %v, want %v", rollDict, want)
	}
	if want := wantGroups([]int16{-1, -1, -3, -3}); !equalExpectation(rollGroups, want) {
		t.Fatalf("roll groups %v, want %v", rollGroups, want)
	}
	prolDict, _, _, prolGroups, ok := readRecoveryGroup(t, converted, 1, "prol", 4)
	if !ok {
		t.Fatal("prol group missing")
	}
	if want := []int16{2, -4}; !equalInt16(prolDict, want) {
		t.Fatalf("prol dictionary %v, want %v", prolDict, want)
	}
	// Prefix sample 1 had no group; sample 2 kept distance 2; EF brings -4.
	if want := wantGroups([]int16{0, 2, -4, -4}, 0); !equalExpectation(prolGroups, want) {
		t.Fatalf("prol groups %v, want %v", prolGroups, want)
	}

	// DefragmentTracks validates and outputs only the kept track's groups.
	kept, keptBuf, err := func() (*mp4.File, *bytes.Buffer, error) {
		rs := bytes.NewReader(buf.Bytes())
		f, err := mp4.DecodeFile(rs, mp4.WithDecodeMode(mp4.DecModeLazyMdat))
		if err != nil {
			t.Fatal(err)
		}
		var out bytes.Buffer
		if err := mp4.DefragmentTracks(f, rs, &out, 1); err != nil {
			return nil, &out, err
		}
		converted, err := mp4.DecodeFile(bytes.NewReader(out.Bytes()))
		return converted, &out, err
	}()
	if err != nil {
		t.Fatal(err)
	}
	if keptBuf.Len() == 0 {
		t.Fatal("DefragmentTracks wrote nothing")
	}
	if len(kept.Moov.Traks) != 1 || kept.Moov.Traks[0].Tkhd.TrackID != 1 {
		t.Fatalf("got %d tracks, want only track 1", len(kept.Moov.Traks))
	}
	if _, _, _, groups, ok := readRecoveryGroup(t, kept, 1, "roll", 4); !ok ||
		!equalExpectation(groups, wantGroups([]int16{-1, -1, -3, -3})) {
		t.Fatalf("kept-track roll groups %v ok=%v", groups, ok)
	}
}

// rejectCase builds one malformed recovery input. mutate may alter the decoded
// file (for states that cannot arrive from the wire) before conversion.
type rejectCase struct {
	name      string
	build     func(t *testing.T, init *mp4.InitSegment, frag *mp4.Fragment)
	wantErr   string
	wantTrack string
	wantPos   string // "traf" or "stbl"
	mutate    func(t *testing.T, f *mp4.File)
}

func TestDefragmentRecoveryReject(t *testing.T) {
	cases := []rejectCase{
		{
			name: "sgpd version 0",
			build: func(t *testing.T, init *mp4.InitSegment, frag *mp4.Fragment) {
				addRecoveryGroup(t, frag.Moof.Traf, "roll",
					[]int16{-1}, []uint32{65537})
			},
			mutate: func(t *testing.T, f *mp4.File) {
				f.Segments[0].Fragments[0].Moof.Traf.Sgpd.Version = 0
			},
			wantErr: "unsupported version", wantTrack: "track 1", wantPos: "traf",
		},
		{
			name: "sgpd version 2 with nonzero default index",
			build: func(t *testing.T, init *mp4.InitSegment, frag *mp4.Fragment) {
				box := &mp4.SgpdBox{Version: 2, GroupingType: "roll", DefaultLength: 2,
					DefaultGroupDescriptionIndex: 3,
					SampleGroupEntries:           []mp4.SampleGroupEntry{&mp4.RollSampleGroupEntry{RollDistance: -1}}}
				_ = frag.Moof.Traf.AddChild(box)
				_ = frag.Moof.Traf.AddChild(&mp4.SbgpBox{GroupingType: "roll",
					SampleCounts: []uint32{1}, GroupDescriptionIndices: []uint32{65537}})
			},
			wantErr: "default_group_description_index", wantTrack: "track 1", wantPos: "traf",
		},
		{
			name: "sbgp version 1 with nonzero parameter",
			build: func(t *testing.T, init *mp4.InitSegment, frag *mp4.Fragment) {
				addRecoveryGroup(t, frag.Moof.Traf, "roll",
					[]int16{-1}, []uint32{65537})
				frag.Moof.Traf.Sbgp.Version = 1
				frag.Moof.Traf.Sbgp.GroupingTypeParameter = 7
			},
			wantErr: "grouping_type_parameter", wantTrack: "track 1", wantPos: "traf",
		},
		{
			name: "sbgp unsupported version",
			build: func(t *testing.T, init *mp4.InitSegment, frag *mp4.Fragment) {
				addRecoveryGroup(t, frag.Moof.Traf, "roll",
					[]int16{-1}, []uint32{65537})
				frag.Moof.Traf.Sbgp.Version = 3
			},
			wantErr: "unsupported version", wantTrack: "track 1", wantPos: "traf",
		},
		{
			name: "zero-length run",
			build: func(t *testing.T, init *mp4.InitSegment, frag *mp4.Fragment) {
				addRecoveryGroup(t, frag.Moof.Traf, "roll",
					[]int16{-1}, []uint32{65537})
				frag.Moof.Traf.Sbgp.SampleCounts[0] = 0
			},
			wantErr: "zero sample count", wantTrack: "track 1", wantPos: "traf",
		},
		{
			name: "run count exceeds scope",
			build: func(t *testing.T, init *mp4.InitSegment, frag *mp4.Fragment) {
				addRecoveryGroup(t, frag.Moof.Traf, "roll",
					[]int16{-1}, []uint32{65537})
				frag.Moof.Traf.Sbgp.SampleCounts[0] = 4
			},
			wantErr: "but its scope has", wantTrack: "track 1", wantPos: "traf",
		},
		{
			name: "count and index arrays disagree",
			build: func(t *testing.T, init *mp4.InitSegment, frag *mp4.Fragment) {
				addRecoveryGroup(t, frag.Moof.Traf, "roll",
					[]int16{-1}, []uint32{65537})
			},
			mutate: func(t *testing.T, f *mp4.File) {
				sbgp := f.Segments[0].Fragments[0].Moof.Traf.Sbgp
				sbgp.SampleCounts = append(sbgp.SampleCounts, 1)
			},
			wantErr: "runs but", wantTrack: "track 1", wantPos: "traf",
		},
		{
			name: "missing description",
			build: func(t *testing.T, init *mp4.InitSegment, frag *mp4.Fragment) {
				addRecoveryGroup(t, frag.Moof.Traf, "roll",
					[]int16{-1}, []uint32{65537})
				frag.Moof.Traf.Sbgp.GroupDescriptionIndices[0] = 65539
			},
			wantErr: "missing fragment description", wantTrack: "track 1", wantPos: "traf",
		},
		{
			name: "missing global description",
			build: func(t *testing.T, init *mp4.InitSegment, frag *mp4.Fragment) {
				_ = frag.Moof.Traf.AddChild(&mp4.SbgpBox{GroupingType: "roll",
					SampleCounts: []uint32{1}, GroupDescriptionIndices: []uint32{5}})
			},
			wantErr: "missing description 5", wantTrack: "track 1", wantPos: "traf",
		},
		{
			name: "duplicate sgpd in one scope",
			build: func(t *testing.T, init *mp4.InitSegment, frag *mp4.Fragment) {
				addRecoveryGroup(t, frag.Moof.Traf, "roll",
					[]int16{-1}, []uint32{65537})
				_ = frag.Moof.Traf.AddChild(&mp4.SgpdBox{Version: 1, GroupingType: "roll", DefaultLength: 2,
					SampleGroupEntries: []mp4.SampleGroupEntry{&mp4.RollSampleGroupEntry{RollDistance: -2}}})
			},
			wantErr: "duplicate sgpd", wantTrack: "track 1", wantPos: "traf",
		},
		{
			name: "duplicate sbgp in one scope",
			build: func(t *testing.T, init *mp4.InitSegment, frag *mp4.Fragment) {
				addRecoveryGroup(t, frag.Moof.Traf, "roll",
					[]int16{-1}, []uint32{65537})
				_ = frag.Moof.Traf.AddChild(&mp4.SbgpBox{GroupingType: "roll",
					SampleCounts: []uint32{1}, GroupDescriptionIndices: []uint32{0}})
			},
			wantErr: "duplicate sbgp", wantTrack: "track 1", wantPos: "traf",
		},
		{
			name: "recovery description not two bytes",
			build: func(t *testing.T, init *mp4.InitSegment, frag *mp4.Fragment) {
				addRecoveryGroup(t, frag.Moof.Traf, "roll",
					[]int16{-1}, []uint32{65537})
			},
			mutate: func(t *testing.T, f *mp4.File) {
				sgpd := f.Segments[0].Fragments[0].Moof.Traf.Sgpd
				sgpd.DefaultLength = 4
				sgpd.SampleGroupEntries[0] = &mp4.UnknownSampleGroupEntry{Name: "roll", Data: []byte{0, 0, 0, 0}}
			},
			wantErr: "must be 2 bytes", wantTrack: "track 1", wantPos: "traf",
		},
		{
			name: "reserved index 65536",
			build: func(t *testing.T, init *mp4.InitSegment, frag *mp4.Fragment) {
				addRecoveryGroup(t, frag.Moof.Traf, "roll",
					[]int16{-1}, []uint32{65537})
				frag.Moof.Traf.Sbgp.GroupDescriptionIndices[0] = 65536
			},
			wantErr: "reserved group_description_index 65536", wantTrack: "track 1", wantPos: "traf",
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			init := recoveryInit(t, 1)
			frag := recoveryFragment(t, 1, 1, 0, "A")
			tc.build(t, init, frag)
			var in bytes.Buffer
			if err := init.Encode(&in); err != nil {
				t.Fatal(err)
			}
			if err := frag.Encode(&in); err != nil {
				t.Fatal(err)
			}
			rs := bytes.NewReader(in.Bytes())
			f, err := mp4.DecodeFile(rs, mp4.WithDecodeMode(mp4.DecModeLazyMdat))
			if err != nil {
				t.Fatal(err)
			}
			if tc.mutate != nil {
				tc.mutate(t, f)
			}
			var out bytes.Buffer
			err = mp4.Defragment(f, rs, &out)
			if err == nil || !strings.Contains(err.Error(), tc.wantErr) {
				t.Fatalf("got %v, want error containing %q", err, tc.wantErr)
			}
			if !strings.Contains(err.Error(), tc.wantTrack) {
				t.Fatalf("error %q does not name %s", err.Error(), tc.wantTrack)
			}
			if !strings.Contains(err.Error(), tc.wantPos) {
				t.Fatalf("error %q does not identify the %s scope", err.Error(), tc.wantPos)
			}
			if out.Len() != 0 {
				t.Fatalf("writer received %d bytes before the error", out.Len())
			}
		})
	}

	t.Run("fragment-local index in a stbl is rejected", func(t *testing.T) {
		init := createDefragPlainAvInit(t)
		stbl := init.Moov.Traks[0].Mdia.Minf.Stbl
		stbl.Stts.SampleCount = []uint32{1}
		stbl.Stts.SampleTimeDelta = []uint32{512}
		stbl.Stsz.SampleNumber = 1
		stbl.Stsz.SampleSize = []uint32{1}
		if err := stbl.Stsc.AddEntry(1, 1, 1); err != nil {
			t.Fatal(err)
		}
		stbl.Stco.ChunkOffset = []uint32{0}
		stbl.AddChild(unknownBox("sbgp", sbgpPayload("roll", 0, 0, [][2]uint32{{1, 65537}})))
		var measure bytes.Buffer
		if err := init.Encode(&measure); err != nil {
			t.Fatal(err)
		}
		stbl.Stco.ChunkOffset = []uint32{uint32(measure.Len()) + 8}
		var buf bytes.Buffer
		if err := init.Encode(&buf); err != nil {
			t.Fatal(err)
		}
		if err := (&mp4.MdatBox{Data: []byte("A")}).Encode(&buf); err != nil {
			t.Fatal(err)
		}
		// A fragment is needed for the file to classify as fragmented.
		audio := recoveryFragment(t, 1, 2, 0, "x")
		if err := audio.Encode(&buf); err != nil {
			t.Fatal(err)
		}
		rs := bytes.NewReader(buf.Bytes())
		f, err := mp4.DecodeFile(rs, mp4.WithDecodeMode(mp4.DecModeLazyMdat))
		if err != nil {
			t.Fatal(err)
		}
		var out bytes.Buffer
		err = mp4.Defragment(f, rs, &out)
		if err == nil || !strings.Contains(err.Error(), "in a stbl") {
			t.Fatalf("got %v, want a stbl-scope index error", err)
		}
		if out.Len() != 0 {
			t.Fatalf("writer received %d bytes before the error", out.Len())
		}
	})

	t.Run("unsupported stbl group boxes are rejected", func(t *testing.T) {
		cases := []struct {
			name    string
			mutate  func(stbl *mp4.StblBox)
			wantErr string
		}{
			{name: "sgpd version 0", wantErr: "unsupported version",
				mutate: func(stbl *mp4.StblBox) { stbl.Sgpds[0].Version = 0 }},
			{name: "nonzero default index", wantErr: "default_group_description_index",
				mutate: func(stbl *mp4.StblBox) {
					stbl.Sgpds[0].Version = 2
					stbl.Sgpds[0].DefaultGroupDescriptionIndex = 1
				}},
			{name: "nonzero sbgp parameter", wantErr: "grouping_type_parameter",
				mutate: func(stbl *mp4.StblBox) {
					stbl.Sbgps[0].Version = 1
					stbl.Sbgps[0].GroupingTypeParameter = 9
				}},
			{name: "duplicate sgpd", wantErr: "duplicate sgpd",
				mutate: func(stbl *mp4.StblBox) {
					stbl.AddChild(&mp4.SgpdBox{Version: 1, GroupingType: "roll", DefaultLength: 2,
						SampleGroupEntries: []mp4.SampleGroupEntry{&mp4.RollSampleGroupEntry{RollDistance: -2}}})
				}},
		}
		for _, tc := range cases {
			t.Run(tc.name, func(t *testing.T) {
				prefix := writeHybridRecoveryPrefix(t)
				var buf bytes.Buffer
				buf.Write(prefix)
				audio := recoveryFragment(t, 1, 2, 0, "xy")
				if err := audio.Encode(&buf); err != nil {
					t.Fatal(err)
				}
				rs := bytes.NewReader(buf.Bytes())
				f, err := mp4.DecodeFile(rs, mp4.WithDecodeMode(mp4.DecModeLazyMdat))
				if err != nil {
					t.Fatal(err)
				}
				tc.mutate(f.Moov.Traks[0].Mdia.Minf.Stbl)
				var out bytes.Buffer
				err = mp4.Defragment(f, rs, &out)
				if err == nil || !strings.Contains(err.Error(), tc.wantErr) {
					t.Fatalf("got %v, want error containing %q", err, tc.wantErr)
				}
				if !strings.Contains(err.Error(), "track 1") || !strings.Contains(err.Error(), "stbl") {
					t.Fatalf("error %q must name track 1 and its stbl", err.Error())
				}
				if out.Len() != 0 {
					t.Fatalf("writer received %d bytes before the error", out.Len())
				}
			})
		}
	})

	t.Run("invalid group on a dropped track does not block the others", func(t *testing.T) {
		init := recoveryInit(t, 2)
		good := recoveryFragment(t, 1, 1, 0, "AB")
		addRecoveryGroup(t, good.Moof.Traf, "roll",
			[]int16{-1}, []uint32{65537, 65537})
		bad := recoveryFragment(t, 2, 2, 0, "xy")
		addRecoveryGroup(t, bad.Moof.Traf, "roll",
			[]int16{-8}, []uint32{65537, 65537})
		bad.Moof.Traf.Sbgp.SampleCounts[0] = 9 // exceeds the two samples

		// Full conversion must reject and name track 2.
		_, fullBuf, err := runRecovery(t, init, []*mp4.Fragment{good, bad})
		if err == nil || !strings.Contains(err.Error(), "track 2") {
			t.Fatalf("full conversion gave %v, want a track 2 error", err)
		}
		if fullBuf.Len() != 0 {
			t.Fatalf("writer received %d bytes before the error", fullBuf.Len())
		}
		// Keeping only track 1 must succeed, groups and bytes intact.
		out, _, err := runRecovery(t, init, []*mp4.Fragment{good, bad}, 1)
		if err != nil {
			t.Fatal(err)
		}
		if len(out.Moov.Traks) != 1 {
			t.Fatalf("got %d tracks, want 1", len(out.Moov.Traks))
		}
		if got := seedRecoveryBytes(t, out, 1); got != "AB" {
			t.Fatalf("payload %q, want AB", got)
		}
		_, _, _, groups, ok := readRecoveryGroup(t, out, 1, "roll", 2)
		if !ok || !equalExpectation(groups, wantGroups([]int16{-1, -1})) {
			t.Fatalf("kept-track groups %v ok=%v", groups, ok)
		}
	})
}

func equalInt16(got, want []int16) bool {
	if len(got) != len(want) {
		return false
	}
	for i := range got {
		if got[i] != want[i] {
			return false
		}
	}
	return true
}

func equalUint32(got, want []uint32) bool {
	if len(got) != len(want) {
		return false
	}
	for i := range got {
		if got[i] != want[i] {
			return false
		}
	}
	return true
}

func equalExpectation(got, want []recoveryExpectation) bool {
	if len(got) != len(want) {
		return false
	}
	for i := range got {
		if got[i] != want[i] {
			return false
		}
	}
	return true
}

func equalRuns(counts, indices []uint32, want [][2]uint32) bool {
	if len(counts) != len(want) {
		return false
	}
	for i, run := range want {
		if counts[i] != run[0] || indices[i] != run[1] {
			return false
		}
	}
	return true
}
