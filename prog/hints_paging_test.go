// Copyright 2026 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package prog

import (
	"bytes"
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"testing"

	"github.com/google/syzkaller/pkg/image"
	"github.com/stretchr/testify/require"
)

type hintsFixture struct {
	name  string
	prog  string
	call  int
	comps []hintComp
}

type hintComp struct {
	pc, op1, op2 uint64
}

func (f hintsFixture) build(t *testing.T, target *Target) (*Prog, CompMap) {
	p, err := target.Deserialize([]byte(f.prog), Strict)
	require.NoError(t, err)
	comps := make(CompMap)
	for _, c := range f.comps {
		comps.Add(c.pc, c.op1, c.op2, true)
	}
	return p, comps
}

func controlFixture() hintsFixture {
	return hintsFixture{
		name: "control",
		prog: `test$hint_data(&AUTO="0809101112131415")`,
		comps: []hintComp{
			{0x101, 0x12111009, 0x42},
			{0x102, 0x13121110, 0x43},
		},
	}
}

func scalarFixture() hintsFixture {
	return hintsFixture{
		name: "scalar",
		prog: `ioctl$1(0x0, 0x111, 0x0)`,
		comps: []hintComp{
			{0x103, 0x111, 0x666},
		},
	}
}

func constrainedFixture() hintsFixture {
	return hintsFixture{
		name: "constrained",
		prog: `socket$inet6(0x111, 0x222, 0x333)`,
		comps: []hintComp{
			{0x201, 0x111, 0x211},
			{0x202, 0x222, 0x1100},
			{0x202, 0x222, 0x1200},
			{0x202, 0x222, 0x1300},
			{0x203, 0x333, 0x10000},
			{0x203, 0x333, 0x10100},
			{0x203, 0x333, 0x10200},
			{0x203, 0x333, 0x10300},
		},
	}
}

func conditionalFixture() hintsFixture {
	return hintsFixture{
		name: "conditional",
		prog: `test$conditional_struct(&AUTO={0x0, @void, @void, @void})`,
		comps: []hintComp{
			// 0x12 sets mask bits that require non-void conditional fields,
			// so the candidate is rejected by the conditions check.
			{0x104, 0x0, 0x12},
			// 0x41 keeps all conditional fields void and stays valid.
			{0x105, 0x0, 0x41},
		},
	}
}

func compressedFixture() hintsFixture {
	raw := []byte{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77}
	return hintsFixture{
		name: "compressed",
		prog: fmt.Sprintf("serialize3(&AUTO=\"$%s\")", image.EncodeB64(image.Compress(raw))),
		comps: []hintComp{
			{0x104, 0x33221100, 0xaabbccdd},
			{0x105, 0x77665544, 0xaabbccdd},
		},
	}
}

func blobFixture() hintsFixture {
	return hintsFixture{
		name: "blob",
		prog: `test$hint_data(&AUTO="ababababababababab")`,
		comps: []hintComp{
			{0x10, 0xab, 0x11},
			{0x20, 0xabab, 0x2222},
			{0x30, 0xabababab, 0x33333333},
			{0x40, 0xabababababababab, 0x4444444444444444},
		},
	}
}

func resourceFixture() hintsFixture {
	return hintsFixture{
		name: "resource",
		prog: `r0 = mutate5(&(0x7f0000000000)='./file0\x00', 0x0)
mutate6(r0, &(0x7f0000000040)="0809101112131415", 0x8)`,
		call: 1,
		comps: []hintComp{
			{0x101, 0x12111009, 0x42},
			{0x102, 0x13121110, 0x43},
		},
	}
}

func mutateSequence(p *Prog, call int, comps CompMap) []string {
	var seq []string
	p.MutateWithHints(call, comps, func(m *Prog) bool {
		seq = append(seq, string(m.Serialize()))
		return true
	})
	return seq
}

// pageSequence collects the full enumeration in pages, cycling through the
// given limits. It also checks the page/cursor invariants.
func pageSequence(t *testing.T, p *Prog, call int, comps CompMap, limits ...int) []string {
	t.Helper()
	var seq []string
	var cursor []byte
	for i := 0; ; i++ {
		limit := limits[i%len(limits)]
		mutants, next, err := p.HintsPage(call, comps, cursor, limit)
		require.NoError(t, err)
		if len(mutants) == 0 {
			// An empty page is allowed only when the enumeration is exhausted.
			require.Empty(t, next, "empty page with a non-empty cursor")
			require.Empty(t, cursor, "empty page before the first page")
			break
		}
		require.LessOrEqual(t, len(mutants), limit)
		for _, m := range mutants {
			seq = append(seq, string(m.Serialize()))
		}
		if len(next) == 0 {
			break
		}
		require.LessOrEqual(t, len(next), hintsCursorMaxSize)
		cursor = next
	}
	return seq
}

func TestHintsPaging(t *testing.T) {
	target := initTargetTest(t, "test", "64")

	t.Run("control", func(t *testing.T) {
		fixture := controlFixture()
		p, comps := fixture.build(t, target)
		before := bytes.Clone(p.Serialize())
		compsBefore := hintsCompsHash(comps)

		mutants, next, err := p.HintsPage(0, comps, nil, 1)
		require.NoError(t, err)
		require.Len(t, mutants, 1)
		require.NotEmpty(t, next)
		require.LessOrEqual(t, len(next), 2048)
		data := mutants[0].Calls[0].Args[0].(*PointerArg).Res.(*DataArg).Data()
		require.Equal(t, "0842000000131415", hex.EncodeToString(data))

		cursor := bytes.Clone(next)
		mutants, next, err = p.HintsPage(0, comps, cursor, 1)
		require.NoError(t, err)
		require.Len(t, mutants, 1)
		require.Empty(t, next)
		data = mutants[0].Calls[0].Args[0].(*PointerArg).Res.(*DataArg).Data()
		require.Equal(t, "0809430000001415", hex.EncodeToString(data))

		// The source program, the comparison table, and the cursor
		// passed to HintsPage are all left unmodified.
		require.Equal(t, before, p.Serialize())
		require.Equal(t, compsBefore, hintsCompsHash(comps))
		savedCursor := bytes.Clone(cursor)
		_, _, err = p.HintsPage(0, comps, cursor, 1)
		require.NoError(t, err)
		require.Equal(t, savedCursor, cursor)
	})

	t.Run("stop_after_false", func(t *testing.T) {
		// Once the callback returns false, it must not be called again,
		// no matter the argument type.
		for _, fixture := range []hintsFixture{scalarFixture(), controlFixture(), compressedFixture()} {
			t.Run(fixture.name, func(t *testing.T) {
				p, comps := fixture.build(t, target)
				calls := 0
				p.MutateWithHints(fixture.call, comps, func(*Prog) bool {
					calls++
					return false
				})
				require.Equal(t, 1, calls)
			})
		}
	})

	t.Run("paging_matches_unpaged", func(t *testing.T) {
		fixtures := []hintsFixture{
			controlFixture(),
			scalarFixture(),
			constrainedFixture(),
			conditionalFixture(),
			compressedFixture(),
			blobFixture(),
			resourceFixture(),
		}
		for _, fixture := range fixtures {
			t.Run(fixture.name, func(t *testing.T) {
				p, comps := fixture.build(t, target)
				want := mutateSequence(p, fixture.call, comps)
				require.NotEmpty(t, want)
				for _, limits := range [][]int{{1}, {2}, {3, 1, 7}, {16}, {1000}} {
					got := pageSequence(t, p, fixture.call, comps, limits...)
					require.Equal(t, want, got, "limits %v", limits)
				}
			})
		}
		// The blob fixture must span several pages of 16.
		p, comps := blobFixture().build(t, target)
		require.Len(t, mutateSequence(p, 0, comps), 25)
	})

	t.Run("variable_limits_same_cursor", func(t *testing.T) {
		p, comps := blobFixture().build(t, target)
		_, cursor, err := p.HintsPage(0, comps, nil, 5)
		require.NoError(t, err)
		require.NotEmpty(t, cursor)

		// A page of 7 candidates must equal pages of 3 and 4 candidates
		// fetched with the same cursor.
		page7, next7, err := p.HintsPage(0, comps, cursor, 7)
		require.NoError(t, err)
		require.Len(t, page7, 7)
		page3, next3, err := p.HintsPage(0, comps, cursor, 3)
		require.NoError(t, err)
		require.Len(t, page3, 3)
		page4, _, err := p.HintsPage(0, comps, next3, 4)
		require.NoError(t, err)
		require.Len(t, page4, 4)
		joined := slices.Concat(page3, page4)
		require.Equal(t, serializeAll(page7), serializeAll(joined))
		// Both ways end up at the same position.
		pageRest, nextRest, err := p.HintsPage(0, comps, next7, 1000)
		require.NoError(t, err)
		require.Empty(t, nextRest)
		require.Len(t, pageRest, 25-5-7)

		// Retrying with the same cursor and limit yields the same page.
		retry, _, err := p.HintsPage(0, comps, cursor, 7)
		require.NoError(t, err)
		require.Equal(t, serializeAll(page7), serializeAll(retry))
	})

	t.Run("resume_across_processes", func(t *testing.T) {
		p, comps := blobFixture().build(t, target)
		page1, cursor, err := p.HintsPage(0, comps, nil, 10)
		require.NoError(t, err)
		require.Len(t, page1, 10)
		require.NotEmpty(t, cursor)

		// Persist the program and the cursor as if for another process.
		dir := t.TempDir()
		progFile := filepath.Join(dir, "prog.txt")
		cursorFile := filepath.Join(dir, "cursor.bin")
		require.NoError(t, os.WriteFile(progFile, p.Serialize(), 0o644))
		require.NoError(t, os.WriteFile(cursorFile, cursor, 0o644))

		// Continue in the same process to have a reference sequence.
		rest, final, err := p.HintsPage(0, comps, cursor, 1000)
		require.NoError(t, err)
		require.Empty(t, final)
		require.Len(t, rest, 15)

		// "Restart": reload the program and rebuild the comparison table
		// with a different insertion order.
		progData, err := os.ReadFile(progFile)
		require.NoError(t, err)
		p2, err := target.Deserialize(progData, Strict)
		require.NoError(t, err)
		require.Equal(t, string(p.Serialize()), string(p2.Serialize()))
		comps2 := make(CompMap)
		fixture := blobFixture()
		for i := len(fixture.comps) - 1; i >= 0; i-- {
			c := fixture.comps[i]
			comps2.Add(c.pc, c.op1, c.op2, true)
		}
		cursorData, err := os.ReadFile(cursorFile)
		require.NoError(t, err)
		rest2, final2, err := p2.HintsPage(0, comps2, cursorData, 1000)
		require.NoError(t, err)
		require.Empty(t, final2)
		require.Equal(t, serializeAll(rest), serializeAll(rest2))
	})

	t.Run("bad_cursors", func(t *testing.T) {
		p, comps := controlFixture().build(t, target)
		_, cursor, err := p.HintsPage(0, comps, nil, 1)
		require.NoError(t, err)
		require.NotEmpty(t, cursor)

		// A corrupted cursor.
		corrupt := bytes.Clone(cursor)
		corrupt[len(corrupt)-1] ^= 0xff
		// An unsupported version.
		badVersion := bytes.Clone(cursor)
		badVersion[3] = 0x7f
		// A truncated cursor.
		truncated := cursor[:len(cursor)-2]
		// An oversized cursor.
		oversized := make([]byte, 3000)

		// A cursor for a different program.
		pOther, err := target.Deserialize([]byte(`test$hint_data(&AUTO="0809101112131416")`), Strict)
		require.NoError(t, err)
		// A cursor for a different call index.
		pTwoCalls, err := target.Deserialize([]byte(`test$hint_data(&AUTO="0809101112131415")
test$hint_data(&AUTO="0809101112131415")`), Strict)
		require.NoError(t, err)
		_, cursorCall0, err := pTwoCalls.HintsPage(0, comps, nil, 1)
		require.NoError(t, err)
		require.NotEmpty(t, cursorCall0)
		// A cursor for different comparisons.
		compsOther := make(CompMap)
		compsOther.Add(0x101, 0x12111009, 0x42, true)
		// A cursor used with a different target OS/arch.
		target32 := initTargetTest(t, "test", "32")
		p32, err := target32.Deserialize([]byte(`test$hint_data(&AUTO="0809101112131415")`), Strict)
		require.NoError(t, err)

		for name, test := range map[string]struct {
			p         *Prog
			callIndex int
			comps     CompMap
			cursor    []byte
			limit     int
		}{
			"corrupted":        {p, 0, comps, corrupt, 1},
			"bad_version":      {p, 0, comps, badVersion, 1},
			"truncated":        {p, 0, comps, truncated, 1},
			"oversized":        {p, 0, comps, oversized, 1},
			"other_program":    {pOther, 0, comps, cursor, 1},
			"other_call_index": {pTwoCalls, 1, comps, cursorCall0, 1},
			"other_comps":      {p, 0, compsOther, cursor, 1},
			"other_target":     {p32, 0, comps, cursor, 1},
			"zero_limit":       {p, 0, comps, cursor, 0},
			"negative_limit":   {p, 0, comps, cursor, -1},
			"bad_call_index":   {p, 5, comps, cursor, 1},
			"negative_call":    {p, -1, comps, cursor, 1},
		} {
			t.Run(name, func(t *testing.T) {
				mutants, next, err := test.p.HintsPage(test.callIndex, test.comps, test.cursor, test.limit)
				require.Error(t, err)
				require.Nil(t, mutants, "error must not return a partial page")
				require.Nil(t, next)
			})
		}

		// The same comparisons in a different insertion order must still work.
		compsReordered := make(CompMap)
		fixture := controlFixture()
		for i := len(fixture.comps) - 1; i >= 0; i-- {
			c := fixture.comps[i]
			compsReordered.Add(c.pc, c.op1, c.op2, true)
		}
		mutants, _, err := p.HintsPage(0, compsReordered, cursor, 1)
		require.NoError(t, err)
		require.Len(t, mutants, 1)
	})

	t.Run("arg_tree_isolation", func(t *testing.T) {
		for _, fixture := range []hintsFixture{controlFixture(), resourceFixture()} {
			t.Run(fixture.name, func(t *testing.T) {
				p, comps := fixture.build(t, target)
				before := bytes.Clone(p.Serialize())
				page1, cursor, err := p.HintsPage(fixture.call, comps, nil, 1)
				require.NoError(t, err)
				require.Len(t, page1, 1)
				page2, _, err := p.HintsPage(fixture.call, comps, cursor, 1)
				require.NoError(t, err)
				require.Len(t, page2, 1)

				// No argument object is shared between the source program
				// and the candidates of the two pages.
				sourceArgs := collectArgs(p)
				for _, m := range slices.Concat(page1, page2) {
					for arg := range collectArgs(m) {
						require.False(t, sourceArgs[arg],
							"candidate shares an argument object with the source: %#v", arg)
					}
				}

				// Mutating a delivered candidate affects neither the source
				// program nor other candidates, including the following pages.
				mutant1Data := string(page1[0].Serialize())
				mutant2Data := string(page2[0].Serialize())
				ForeachArg(page1[0].Calls[fixture.call], func(arg Arg, _ *ArgCtx) {
					switch a := arg.(type) {
					case *DataArg:
						if data := a.Data(); len(data) != 0 {
							data[0] ^= 0xff
						}
					case *ConstArg:
						a.Val ^= 0xff
					case *ResultArg:
						a.Val ^= 0xff
					}
				})
				require.Equal(t, before, p.Serialize())
				require.Equal(t, mutant2Data, string(page2[0].Serialize()))
				require.NotEqual(t, mutant1Data, string(page1[0].Serialize()))
				// Re-fetching the same page yields the original candidate.
				refetch, _, err := p.HintsPage(fixture.call, comps, nil, 1)
				require.NoError(t, err)
				require.Equal(t, mutant1Data, string(refetch[0].Serialize()))
			})
		}
	})

	t.Run("rejected_candidates", func(t *testing.T) {
		p, comps := conditionalFixture().build(t, target)
		// The 0x12 candidate violates the conditional fields and is rejected;
		// the remaining five candidates are valid. The rejected candidate is
		// the first replacer at its position, but it must not occupy a page
		// slot: the first page of 2 already holds the first two valid
		// candidates.
		want := mutateSequence(p, 0, comps)
		require.Len(t, want, 5)
		for _, s := range want {
			require.NotContains(t, s, "{0x12}", "rejected candidate leaked")
		}
		require.Contains(t, want[0], "{0x41}")
		mutants, next, err := p.HintsPage(0, comps, nil, 2)
		require.NoError(t, err)
		require.Len(t, mutants, 2)
		require.NotEmpty(t, next)
		require.Equal(t, want[:2], serializeAll(mutants))
		// Paging one by one yields the same sequence without empty pages.
		require.Equal(t, want, pageSequence(t, p, 0, comps, 1))
	})

	t.Run("compressed_image_lifecycle", func(t *testing.T) {
		fixture := compressedFixture()
		p, comps := fixture.build(t, target)
		images0, memory0 := image.StatImages.Load(), image.StatMemory.Load()
		checkStats := func() {
			require.Equal(t, images0, image.StatImages.Load(), "image mappings leaked")
			require.Equal(t, memory0, image.StatMemory.Load(), "image memory leaked")
		}

		// Consecutive paged requests.
		var cursor []byte
		total := 0
		for {
			mutants, next, err := p.HintsPage(0, comps, cursor, 1)
			require.NoError(t, err)
			checkStats()
			total += len(mutants)
			if len(next) == 0 {
				break
			}
			cursor = next
		}
		require.Equal(t, 2, total)

		// Errors leave no mappings behind either.
		_, _, err := p.HintsPage(0, comps, []byte{0xde, 0xad}, 1)
		require.Error(t, err)
		checkStats()

		// Early stop of the legacy interface: no mapping is held when the
		// user callback runs, and everything is released on return.
		calls := 0
		p.MutateWithHints(0, comps, func(*Prog) bool {
			checkStats()
			calls++
			return false
		})
		require.Equal(t, 1, calls)
		checkStats()

		// The source argument still holds the original compressed data.
		arg := p.Calls[0].Args[0].(*PointerArg).Res.(*DataArg)
		raw, dtor := image.MustDecompress(arg.Data())
		require.Equal(t, []byte{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77}, raw)
		dtor()
		checkStats()
	})

	t.Run("empty_results", func(t *testing.T) {
		p, _ := controlFixture().build(t, target)
		comps := make(CompMap)
		comps.Add(0x101, 0xdeadbeef, 0x42, true)
		mutants, next, err := p.HintsPage(0, comps, nil, 16)
		require.NoError(t, err)
		require.Empty(t, mutants)
		require.Empty(t, next)
	})
}

func serializeAll(progs []*Prog) []string {
	var res []string
	for _, p := range progs {
		res = append(res, string(p.Serialize()))
	}
	return res
}

func collectArgs(p *Prog) map[Arg]bool {
	args := make(map[Arg]bool)
	for _, c := range p.Calls {
		ForeachArg(c, func(arg Arg, _ *ArgCtx) {
			args[arg] = true
		})
	}
	return args
}
