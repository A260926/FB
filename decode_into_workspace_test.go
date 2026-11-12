package vorbis

import (
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"math"
	"os"
	"testing"
)

// loadResidue2Fixture reads the hand-built type 2 residue fixture.
func loadResidue2Fixture(t *testing.T) (headers [][]byte, left, right []byte) {
	t.Helper()
	var f struct {
		Headers [][]byte
		Left    []byte
		Right   []byte
	}
	raw, err := os.ReadFile("harness/residue2.json")
	if err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(raw, &f); err != nil {
		t.Fatal(err)
	}
	return f.Headers, f.Left, f.Right
}

// readF32Reference reads an interleaved little-endian float32 reference.
func readF32Reference(t *testing.T, name string) []float32 {
	t.Helper()
	ref, err := os.ReadFile(name)
	if err != nil {
		t.Fatal(err)
	}
	out := make([]float32, len(ref)/4)
	for i := range out {
		out[i] = math.Float32frombits(binary.LittleEndian.Uint32(ref[4*i:]))
	}
	return out
}

func newTestDecoder(t *testing.T, headers [][]byte) *Decoder {
	t.Helper()
	var d Decoder
	for _, h := range headers {
		if err := d.ReadHeader(h); err != nil {
			t.Fatal(err)
		}
	}
	return &d
}

// workspaceStream is one entry of harness/workspace_streams.json.
type workspaceStream struct {
	Name      string   `json:"name"`
	Channels  int      `json:"channels"`
	Blocksize [2]int   `json:"blocksize"`
	Headers   [][]byte `json:"-"`
	Packets   [][]byte `json:"-"`
	RawH      []string `json:"headers"`
	RawP      []string `json:"packets"`
}

func loadWorkspaceStreams(t *testing.T) []workspaceStream {
	t.Helper()
	var doc struct {
		Streams []workspaceStream
	}
	raw, err := os.ReadFile("harness/workspace_streams.json")
	if err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(raw, &doc); err != nil {
		t.Fatal(err)
	}
	for i := range doc.Streams {
		s := &doc.Streams[i]
		s.Headers = make([][]byte, len(s.RawH))
		for j, h := range s.RawH {
			s.Headers[j], err = base64.StdEncoding.DecodeString(h)
			if err != nil {
				t.Fatal(err)
			}
		}
		s.Packets = make([][]byte, len(s.RawP))
		for j, p := range s.RawP {
			s.Packets[j], err = base64.StdEncoding.DecodeString(p)
			if err != nil {
				t.Fatal(err)
			}
		}
	}
	return doc.Streams
}

// decodeSequence decodes every packet with DecodeInto into a reused buffer
// and returns the concatenated, interleaved output.
func decodeSequence(t *testing.T, d *Decoder, packets [][]byte) []float32 {
	t.Helper()
	buf := make([]float32, d.BufferSize())
	var all []float32
	for i, p := range packets {
		out, err := d.DecodeInto(p, buf)
		if err != nil {
			t.Fatalf("packet %d: %v", i, err)
		}
		all = append(all, out...)
	}
	return all
}

func slicesEqual(a, b []float32) bool {
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

func TestDecodeIntoWorkspace(t *testing.T) {
	// --- Part 1: the type 2 residue regression, against independent
	// FFmpeg references, one channel at a time. ---------------------------
	t.Run("residue2 reference", func(t *testing.T) {
		headers, left, right := loadResidue2Fixture(t)
		for side, packet := range map[string][]byte{"left": left, "right": right} {
			ref := readF32Reference(t, "harness/"+side+".f32")
			if len(ref) != 64 {
				t.Fatalf("%s: reference has %d samples, expected 64", side, len(ref))
			}

			d := newTestDecoder(t, headers)
			buf := make([]float32, d.BufferSize())
			// priming packet produces no output
			if out, err := d.DecodeInto(packet, buf); err != nil || len(out) != 0 {
				t.Fatalf("%s priming: %v, %d samples", side, err, len(out))
			}
			out, err := d.DecodeInto(packet, buf)
			if err != nil || len(out) != 64 {
				t.Fatalf("%s audio: %v, %d samples", side, err, len(out))
			}
			energy := 0.0
			for i, want := range ref {
				got := out[i]
				energy += float64(got * got)
				if diff := math.Abs(float64(got - want)); diff > 1e-4 {
					t.Fatalf("%s sample %d: %g differs from reference %g (diff %g)", side, i, got, want, diff)
				}
			}
			if energy < 0.01 {
				t.Fatalf("%s: decoded channel is silent", side)
			}
		}
	})

	// Decode and DecodeInto must agree, including the valid output length.
	t.Run("Decode matches DecodeInto", func(t *testing.T) {
		headers, left, right := loadResidue2Fixture(t)
		for _, packet := range [][]byte{left, right} {
			dAlloc := newTestDecoder(t, headers)
			dReuse := newTestDecoder(t, headers)
			buf := make([]float32, dReuse.BufferSize())
			for step := 0; step < 2; step++ {
				a, err := dAlloc.Decode(packet)
				if err != nil {
					t.Fatal(err)
				}
				b, err := dReuse.DecodeInto(packet, buf)
				if err != nil {
					t.Fatal(err)
				}
				if len(a) != len(b) {
					t.Fatalf("lengths differ: %d != %d", len(a), len(b))
				}
				if !slicesEqual(a, b) {
					t.Fatalf("step %d: Decode and DecodeInto produced different samples", step)
				}
			}
		}
	})

	// --- Part 2: auditable packet streams covering shared floors, two
	// submaps (floor0 + floor1, residues 1 + 2), coupling and short/long
	// block alternation. --------------------------------------------------
	streams := loadWorkspaceStreams(t)
	if len(streams) != 3 {
		t.Fatalf("expected 3 workspace streams, got %d", len(streams))
	}
	for _, stream := range streams {
		stream := stream
		t.Run(stream.Name, func(t *testing.T) {
			// Every stream is decoded twice: once with allocating Decode,
			// once with DecodeInto into one reused output buffer.
			dAlloc := newTestDecoder(t, stream.Headers)
			dReuse := newTestDecoder(t, stream.Headers)
			buf := make([]float32, dReuse.BufferSize())
			for i, p := range stream.Packets {
				a, err := dAlloc.Decode(p)
				if err != nil {
					t.Fatalf("Decode packet %d: %v", i, err)
				}
				b, err := dReuse.DecodeInto(p, buf)
				if err != nil {
					t.Fatalf("DecodeInto packet %d: %v", i, err)
				}
				if len(a) != len(b) {
					t.Fatalf("packet %d: lengths differ %d != %d", i, len(a), len(b))
				}
				if !slicesEqual(a, b) {
					t.Fatalf("packet %d: Decode and DecodeInto disagree", i)
				}
			}

			// Zero heap allocations on the fully warmed-up path. The
			// decoder is primed first; AllocsPerRun then adds one more
			// warm-up run before measuring the steady state.
			d := newTestDecoder(t, stream.Headers)
			for _, p := range stream.Packets {
				if _, err := d.DecodeInto(p, buf); err != nil {
					t.Fatal(err)
				}
			}
			steady := testing.AllocsPerRun(100, func() {
				for _, p := range stream.Packets {
					if _, err := d.DecodeInto(p, buf); err != nil {
						panic(err)
					}
				}
			})
			if steady != 0 {
				t.Fatalf("stream %q: %v steady-state allocations, want 0", stream.Name, steady)
			}
		})
	}

	// Per-channel isolation of the reusable workspaces: a channel without a
	// floor must stay silent even if it still takes part in a type 2
	// residue, and stale values must never leak between packets.
	t.Run("no stale coefficients", func(t *testing.T) {
		presence := [][2]int{{1, 1}, {1, 0}, {0, 1}, {1, 1}, {0, 0}, {1, 1}, {1, 0}, {0, 1}}
		for _, stream := range streams {
			d := newTestDecoder(t, stream.Headers)
			buf := make([]float32, d.BufferSize())
			for i, p := range stream.Packets {
				if _, err := d.DecodeInto(p, buf); err != nil {
					t.Fatalf("%s packet %d: %v", stream.Name, i, err)
				}
				for ch := 0; ch < d.channels; ch++ {
					ws := &d.floorWorkspace[ch]
					if ws.active != (presence[i][ch] == 1) {
						t.Fatalf("%s packet %d ch %d: active=%v, want %v", stream.Name, i, ch, ws.active, presence[i][ch] == 1)
					}
					if !ws.active {
						// The IMDCT overwrites the whole raw buffer every
						// packet, so a channel without a floor must have an
						// all-zero raw buffer even when it still took part
						// in a type 2 residue. (The interleaved output may
						// still carry this channel's overlap from the
						// previous packet, which is correct.)
						for k, v := range d.rawBuffer[ch] {
							if v != 0 {
								t.Fatalf("%s packet %d ch %d: inactive channel has nonzero raw sample at %d (%g)", stream.Name, i, ch, k, v)
							}
						}
					}
				}
			}
		}
	})

	// Coupling forces residue participation but not floor application.
	t.Run("coupling flags", func(t *testing.T) {
		var coupling *workspaceStream
		for i := range streams {
			if streams[i].Name == "coupling" {
				coupling = &streams[i]
			}
		}
		if coupling == nil {
			t.Fatal("coupling stream missing")
		}
		d := newTestDecoder(t, coupling.Headers)
		buf := make([]float32, d.BufferSize())
		// packet 2: only the right channel has a floor
		for i := 0; i <= 2; i++ {
			if _, err := d.DecodeInto(coupling.Packets[i], buf); err != nil {
				t.Fatal(err)
			}
		}
		mag, ang := d.floorWorkspace[0], d.floorWorkspace[1]
		if mag.active || !ang.active {
			t.Fatalf("floor presence: mag active=%v ang active=%v", mag.active, ang.active)
		}
		if mag.noResidue || ang.noResidue {
			t.Fatalf("coupling must keep both residues participating: noResidue=%v/%v", mag.noResidue, ang.noResidue)
		}
	})

	// A floor object shared between channels must keep independent
	// per-channel results until the whole spectrum is restored.
	t.Run("shared floor independence", func(t *testing.T) {
		var shared *workspaceStream
		for i := range streams {
			if streams[i].Name == "sharedFloor" {
				shared = &streams[i]
			}
		}
		if shared == nil {
			t.Fatal("sharedFloor stream missing")
		}
		if len(newTestDecoder(t, shared.Headers).floors) != 1 {
			t.Fatal("fixture should use a single shared floor")
		}
		d := newTestDecoder(t, shared.Headers)
		buf := make([]float32, d.BufferSize())
		// packet 1: only the left channel has a floor
		if _, err := d.DecodeInto(shared.Packets[0], buf); err != nil {
			t.Fatal(err)
		}
		if _, err := d.DecodeInto(shared.Packets[1], buf); err != nil {
			t.Fatal(err)
		}
		if d.floorWorkspace[0].active == d.floorWorkspace[1].active {
			t.Fatal("per-channel floor results were not kept separate")
		}
	})

	// --- Part 4: two decoders must not share any workspace state. --------
	t.Run("independent decoders", func(t *testing.T) {
		for _, stream := range streams {
			ref := newTestDecoder(t, stream.Headers)
			want := decodeSequence(t, ref, stream.Packets)

			a, b := newTestDecoder(t, stream.Headers), newTestDecoder(t, stream.Headers)
			bufA := make([]float32, a.BufferSize())
			bufB := make([]float32, b.BufferSize())
			var gotA, gotB []float32
			for i, p := range stream.Packets {
				outA, err := a.DecodeInto(p, bufA)
				if err != nil {
					t.Fatal(err)
				}
				gotA = append(gotA, outA...)
				// interleave a call on the other decoder
				outB, err := b.DecodeInto(p, bufB)
				if err != nil {
					t.Fatal(err)
				}
				gotB = append(gotB, outB...)
				if len(outA) != len(outB) {
					t.Fatalf("%s packet %d: interleaved lengths differ", stream.Name, i)
				}
			}
			if !slicesEqual(want, gotA) || !slicesEqual(want, gotB) {
				t.Fatalf("%s: interleaved decoders diverged from a sequential decode", stream.Name)
			}
		}
	})

	// --- Part 5: Clear reuses capacity but restarts priming cleanly. -----
	t.Run("Clear restarts without stale overlap", func(t *testing.T) {
		for _, stream := range streams {
			ref := newTestDecoder(t, stream.Headers)
			want := decodeSequence(t, ref, stream.Packets)

			d := newTestDecoder(t, stream.Headers)
			buf := make([]float32, d.BufferSize())
			for _, p := range stream.Packets[:3] {
				if _, err := d.DecodeInto(p, buf); err != nil {
					t.Fatal(err)
				}
			}
			d.Clear()
			if d.hasOverlap {
				t.Fatal("Clear did not reset the overlap state")
			}
			// the first packet after Clear must re-prime: no output
			if out, err := d.DecodeInto(stream.Packets[0], buf); err != nil || len(out) != 0 {
				t.Fatalf("%s: re-priming gave %d samples, err %v", stream.Name, len(out), err)
			}
			var got []float32
			for _, p := range stream.Packets[1:] {
				out, err := d.DecodeInto(p, buf)
				if err != nil {
					t.Fatal(err)
				}
				got = append(got, out...)
			}
			if !slicesEqual(want, got) {
				t.Fatalf("%s: output after Clear differs from a fresh decode", stream.Name)
			}
			// capacity must still be reused after the restart
			allocs := testing.AllocsPerRun(50, func() {
				for _, p := range stream.Packets {
					if _, err := d.DecodeInto(p, buf); err != nil {
						panic(err)
					}
				}
			})
			if allocs != 0 {
				t.Fatalf("%s: %v allocations after Clear, want 0", stream.Name, allocs)
			}
		}
	})

	// --- Part 6: failed calls must report errors without consuming the
	// overlap history the next successful call needs. --------------------
	t.Run("errors preserve overlap history", func(t *testing.T) {
		headers, left, _ := loadResidue2Fixture(t)

		// missing headers
		var noHeaders Decoder
		if _, err := noHeaders.DecodeInto(left, make([]float32, 64)); err == nil {
			t.Fatal("expected an error when headers are missing")
		}
		if _, err := noHeaders.Decode(left); err == nil {
			t.Fatal("expected an error when headers are missing")
		}

		// reference sequence: prime packet then one audio packet
		ref := newTestDecoder(t, headers)
		refBuf := make([]float32, ref.BufferSize())
		if out, err := ref.DecodeInto(left, refBuf); err != nil || len(out) != 0 {
			t.Fatalf("reference priming: %v %d", err, len(out))
		}
		want, err := ref.DecodeInto(left, refBuf)
		if err != nil {
			t.Fatal(err)
		}

		// a short buffer is rejected before any decoder state changes
		d := newTestDecoder(t, headers)
		buf := make([]float32, d.BufferSize())
		if out, err := d.DecodeInto(left, buf); err != nil || len(out) != 0 {
			t.Fatalf("priming: %v %d", err, len(out))
		}
		if _, err := d.DecodeInto(left, buf[:1]); err == nil {
			t.Fatal("expected a buffer-too-short error")
		}
		got, err := d.DecodeInto(left, buf)
		if err != nil {
			t.Fatal(err)
		}
		if len(got) != len(want) || !slicesEqual(got, want) {
			t.Fatal("the rejected short-buffer call consumed overlap history")
		}

		// a corrupt packet (header flag set) is rejected likewise
		corrupt := append([]byte(nil), left...)
		corrupt[0] |= 1
		d2 := newTestDecoder(t, headers)
		buf2 := make([]float32, d2.BufferSize())
		if out, err := d2.DecodeInto(left, buf2); err != nil || len(out) != 0 {
			t.Fatalf("priming: %v %d", err, len(out))
		}
		if _, err := d2.DecodeInto(corrupt, buf2); err == nil {
			t.Fatal("expected an error for a corrupt packet")
		}
		got2, err := d2.DecodeInto(left, buf2)
		if err != nil {
			t.Fatal(err)
		}
		if !slicesEqual(got2, want) {
			t.Fatal("the rejected corrupt packet consumed overlap history")
		}
	})
}
