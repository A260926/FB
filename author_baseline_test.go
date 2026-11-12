package vorbis

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"testing"
)

func TestBaseline(t *testing.T) {
	var f struct {
		Headers [][]byte
		Left    []byte
		Right   []byte
	}
	raw, err := os.ReadFile("harness/residue2.json")
	if err != nil {
		t.Fatal(err)
	}
	if err = json.Unmarshal(raw, &f); err != nil {
		t.Fatal(err)
	}

	// After reading the three headers and warming the decoder up,
	// DecodeInto must not allocate at all.
	var warm Decoder
	for _, h := range f.Headers {
		if err = warm.ReadHeader(h); err != nil {
			t.Fatal(err)
		}
	}
	buf := make([]float32, warm.BufferSize())
	if _, err = warm.DecodeInto(f.Left, buf); err != nil {
		t.Fatal(err)
	}
	allocs := testing.AllocsPerRun(100, func() {
		if _, e := warm.DecodeInto(f.Left, buf); e != nil {
			panic(e)
		}
	})
	if allocs != 0 {
		t.Fatalf("DecodeInto allocates %.0f times per packet, want 0", allocs)
	}
	fmt.Printf("BASELINE allocations=%.0f\n", allocs)

	for side, packet := range map[string][]byte{"left": f.Left, "right": f.Right} {
		var d Decoder
		for _, h := range f.Headers {
			if err = d.ReadHeader(h); err != nil {
				t.Fatal(err)
			}
		}
		out, err := d.Decode(packet)
		if err != nil || len(out) != 0 {
			t.Fatalf("priming: %v %d", err, len(out))
		}
		out, err = d.Decode(packet)
		if err != nil || len(out) != 64 {
			t.Fatalf("audio: %v %d", err, len(out))
		}
		ref, err := os.ReadFile("harness/" + side + ".f32")
		if err != nil || len(ref) != 256 {
			t.Fatalf("ref: %v %d", err, len(ref))
		}
		var energy, want float64
		for i, v := range out {
			energy += float64(v * v)
			r := math.Float32frombits(binary.LittleEndian.Uint32(ref[4*i:]))
			want += float64(r * r)
			if math.Abs(float64(v-r)) > 1e-4 {
				t.Fatalf("%s diff at sample %d: %g != %g", side, i, v, r)
			}
		}
		if want < 0.01 {
			t.Fatal("silent independent reference")
		}
		// Both channels must reproduce the independent FFmpeg waveform;
		// the right channel used to come out fully silent.
		if energy < 0.01 {
			t.Fatalf("%s: decoded channel is silent", side)
		}
		fmt.Printf("BASELINE %s frames=32 actual_energy=%.6f reference_energy=%.6f\n", side, energy, want)
	}
}
