package main

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"

	"github.com/Eyevinn/mp4ff/mp4"
)

// TestDefragmenterCommand builds a fragmented file with a roll recovery
// group, runs the command, and checks that the progressive output keeps the
// group and the sample bytes.
func TestDefragmenterCommand(t *testing.T) {
	dir := t.TempDir()
	inPath := filepath.Join(dir, "in.mp4")
	outPath := filepath.Join(dir, "out.mp4")

	init := mp4.CreateEmptyInit()
	trak := init.AddEmptyTrack(48000, "audio", "und")
	if err := trak.SetAACDescriptor(2, 48000); err != nil {
		t.Fatal(err)
	}
	frag, err := mp4.CreateFragment(1, 1)
	if err != nil {
		t.Fatal(err)
	}
	for n, payload := range []byte("AB") {
		frag.AddFullSample(mp4.FullSample{
			Sample:     mp4.NewSample(0, 1024, 1, 0),
			DecodeTime: uint64(n) * 1024,
			Data:       []byte{payload},
		})
	}
	sgpd := &mp4.SgpdBox{Version: 1, GroupingType: "roll", DefaultLength: 2,
		SampleGroupEntries: []mp4.SampleGroupEntry{&mp4.RollSampleGroupEntry{RollDistance: -1}}}
	if err := frag.Moof.Traf.AddChild(sgpd); err != nil {
		t.Fatal(err)
	}
	sbgp := &mp4.SbgpBox{GroupingType: "roll", SampleCounts: []uint32{2},
		GroupDescriptionIndices: []uint32{65537}}
	if err := frag.Moof.Traf.AddChild(sbgp); err != nil {
		t.Fatal(err)
	}

	var in bytes.Buffer
	if err := init.Encode(&in); err != nil {
		t.Fatal(err)
	}
	if err := frag.Encode(&in); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(inPath, in.Bytes(), 0o600); err != nil {
		t.Fatal(err)
	}

	if err := run([]string{"defragmenter", inPath, outPath}); err != nil {
		t.Fatalf("run: %v", err)
	}
	out, err := os.ReadFile(outPath)
	if err != nil {
		t.Fatal(err)
	}
	parsed, err := mp4.DecodeFile(bytes.NewReader(out))
	if err != nil {
		t.Fatal(err)
	}
	if parsed.IsFragmented() {
		t.Fatal("output is still fragmented")
	}
	stbl := parsed.Moov.Traks[0].Mdia.Minf.Stbl
	if len(stbl.Sgpds) != 1 || stbl.Sgpds[0].GroupingType != "roll" {
		t.Fatalf("roll sgpd not retained: %d sgpd boxes", len(stbl.Sgpds))
	}
	if len(stbl.Sbgps) != 1 || stbl.Sbgps[0].GroupingType != "roll" {
		t.Fatalf("roll sbgp not retained: %d sbgp boxes", len(stbl.Sbgps))
	}
	if err := run([]string{"defragmenter", "-tracks", "1", inPath, filepath.Join(dir, "kept.mp4")}); err != nil {
		t.Fatalf("run with -tracks: %v", err)
	}
	if err := run([]string{"defragmenter"}); err == nil {
		t.Fatal("missing arguments must be an error")
	}
}
