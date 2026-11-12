// Copyright 2017 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package prog

// A hint is basically a tuple consisting of a pointer to an argument
// in one of the syscalls of a program and a value, which should be
// assigned to that argument (we call it a replacer).

// A simplified version of hints workflow looks like this:
//		1. Fuzzer launches a program (we call it a hint seed) and collects all
// the comparisons' data for every syscall in the program.
//		2. Next it tries to match the obtained comparison operands' values
// vs. the input arguments' values.
//		3. For every such match the fuzzer mutates the program by
// replacing the pointed argument with the saved value.
//		4. If a valid program is obtained, then fuzzer launches it and
// checks if new coverage is obtained.
// For more insights on particular mutations please see prog/hints_test.go.

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"maps"
	"slices"
	"sync"

	"github.com/google/syzkaller/pkg/image"
)

// CompMap maps comparison operand that could come from the input to the second operand to the PC.
type CompMap map[uint64]map[uint64]map[uint64]bool

const (
	maxDataLength = 100
)

var specialIntsSet map[uint64]bool

func (m CompMap) Add(pc, arg1, arg2 uint64, isConst bool) {
	if _, ok := m[arg1]; !ok {
		m[arg1] = make(map[uint64]map[uint64]bool)
	}
	if _, ok := m[arg1][arg2]; !ok {
		m[arg1][arg2] = make(map[uint64]bool)
	}
	m[arg1][arg2][pc] = true
	if !isConst {
		// Both operands could come from the input.
		m.Add(pc, arg2, arg1, true)
	}
}

func (m CompMap) String() string {
	buf := new(bytes.Buffer)
	for v, comps := range m {
		if len(buf.Bytes()) != 0 {
			fmt.Fprintf(buf, ", ")
		}
		fmt.Fprintf(buf, "0x%x:", v)
		for c := range comps {
			fmt.Fprintf(buf, " 0x%x", c)
		}
	}
	return buf.String()
}

func (m CompMap) Len() int {
	var count int
	for _, nested := range m {
		for _, nested2 := range nested {
			count += len(nested2)
		}
	}
	return count
}

// InplaceIntersect only leaves the value pairs that are also present in other.
func (m CompMap) InplaceIntersect(other CompMap) {
	for val1, nested := range m {
		for val2, pcs := range nested {
			for pc := range pcs {
				if !other[val1][val2][pc] {
					delete(pcs, pc)
				}
			}
			if len(pcs) == 0 {
				delete(nested, val2)
			}
		}
		if len(nested) == 0 {
			delete(m, val1)
		}
	}
}

// MutateWithHints mutates the program using the comparison operands stored in compMaps.
// For each of the mutants executes the exec callback.
// The callback must return whether we should continue substitution (true)
// or abort the process (false). Once the callback has returned false,
// it is not called again for any argument type.
func (p *Prog) MutateWithHints(callIndex int, comps CompMap, exec func(p *Prog) bool) {
	p = p.Clone()
	c := p.Calls[callIndex]
	foreachValidHint(p, c, comps, new(hintState), func() bool {
		return exec(p)
	})
}

// foreachValidHint enumerates hint candidates for the call and invokes exec
// for every candidate that passes the standard validity checks.
// Candidates that fail the checks are silently skipped (they are not fixed
// by repairing arguments or re-selecting union options).
func foreachValidHint(p *Prog, c *Call, comps CompMap, st *hintState, exec func() bool) {
	foreachHint(c, comps, st, func() bool {
		// Don't try to fix the candidate program.
		// Assuming the original call was sanitized, we've got a bad call
		// as the result of hint substitution, so just throw it away.
		if p.Target.sanitize(c, false) != nil {
			return true
		}
		if p.checkConditions() != nil {
			// Patching unions that no longer satisfy conditions would
			// require much deeped changes to prog arguments than
			// the hint enumeration expects.
			// Let's just ignore such mutations.
			return true
		}
		p.debugValidate()
		return exec()
	})
}

// hintPos identifies a position in the hint candidate enumeration of a call:
// the argument ordinal in the ForeachArg traversal order, the byte offset
// within the argument's data, and the index in the sorted replacer list
// produced for that (argument, offset) pair.
type hintPos struct {
	arg      int
	offset   int
	replacer int
}

// hintState carries the enumeration state shared by the hint generators.
type hintState struct {
	from hintPos // candidates before this position are skipped
	cur  hintPos // position of the candidate being delivered
}

// foreachHint enumerates hint candidates for the call in the ForeachArg
// traversal order, skipping all candidates before st.from. For every
// candidate it applies the mutation to the call, records the candidate
// position in st.cur and calls exec. If exec returns false, the enumeration
// stops immediately and no further exec calls are made.
func foreachHint(c *Call, comps CompMap, st *hintState, exec func() bool) {
	ord := -1
	stop := false
	ForeachArg(c, func(arg Arg, ctx *ArgCtx) {
		if stop {
			ctx.Stop = true
			return
		}
		ord++
		if ord < st.from.arg {
			return
		}
		if !generateHintsAt(comps, arg, ctx.Field, ord, st, exec) {
			stop = true
		}
	})
}

func generateHints(compMap CompMap, arg Arg, field *Field, exec func() bool) {
	generateHintsAt(compMap, arg, field, 0, new(hintState), exec)
}

// generateHintsAt applies the type-based filters and dispatches to the
// per-argument-kind generators. It returns false iff exec requested a stop.
func generateHintsAt(compMap CompMap, arg Arg, field *Field, ord int, st *hintState, exec func() bool) bool {
	typ := arg.Type()
	if typ == nil || arg.Dir() == DirOut {
		return true
	}
	switch t := typ.(type) {
	case *ProcType:
		// Random proc will not pass validation.
		// We can mutate it, but only if the resulting value is within the legal range.
		return true
	case *ConstType:
		if IsPad(typ) {
			return true
		}
	case *CsumType:
		// Csum will not pass validation and is always computed.
		return true
	case *BufferType:
		switch t.Kind {
		case BufferFilename:
			// This can generate escaping paths and is probably not too useful anyway.
			return true
		case BufferString, BufferGlob:
			if len(t.Values) != 0 {
				// These are frequently file names or complete enumerations.
				// Mutating these may be useful iff we intercept strcmp
				// (and filter out file names).
				return true
			}
		}
	}

	switch a := arg.(type) {
	case *ConstArg:
		if arg.Type().TypeBitSize() <= 8 {
			// Very small arg, hopefully we can guess it w/o hints help.
			return true
		}
		return checkConstArgAt(a, field, compMap, ord, st, exec)
	case *DataArg:
		if arg.Size() <= 3 {
			// Let's assume it either does not contain anything interesting,
			// or we can guess everything eventually by brute force.
			return true
		}
		if typ.(*BufferType).Kind == BufferCompressed {
			return checkCompressedArgAt(a, compMap, ord, st, exec)
		}
		return checkDataArgAt(a, compMap, ord, st, exec)
	}
	return true
}

func checkConstArg(arg *ConstArg, field *Field, compMap CompMap, exec func() bool) {
	checkConstArgAt(arg, field, compMap, 0, new(hintState), exec)
}

func checkConstArgAt(arg *ConstArg, field *Field, compMap CompMap, ord int, st *hintState, exec func() bool) bool {
	original := arg.Val
	defer func() {
		arg.Val = original
	}()
	// Note: because shrinkExpand returns a map, order of programs is non-deterministic.
	// This can affect test coverage reports.
replacerLoop:
	for idx, replacer := range shrinkExpand(original, compMap, arg.Type().TypeBitSize(), false) {
		if ord == st.from.arg && idx < st.from.replacer {
			continue
		}
		if field != nil && len(field.relatedFields) != 0 {
			for related := range field.relatedFields {
				if related.(uselessHinter).uselessHint(replacer) {
					continue replacerLoop
				}
			}
		} else if arg.Type().(uselessHinter).uselessHint(replacer) {
			continue
		}
		arg.Val = replacer
		st.cur = hintPos{arg: ord, replacer: idx}
		if !exec() {
			return false
		}
	}
	return true
}

func checkDataArg(arg *DataArg, compMap CompMap, exec func() bool) {
	checkDataArgAt(arg, compMap, 0, new(hintState), exec)
}

func checkDataArgAt(arg *DataArg, compMap CompMap, ord int, st *hintState, exec func() bool) bool {
	bytes := make([]byte, 8)
	data := arg.Data()
	size := min(len(data), maxDataLength)
	for i := range size {
		if ord == st.from.arg && i < st.from.offset {
			continue
		}
		original := make([]byte, 8)
		copy(original, data[i:])
		val := binary.LittleEndian.Uint64(original)
		for idx, replacer := range shrinkExpand(val, compMap, 64, false) {
			if ord == st.from.arg && i == st.from.offset && idx < st.from.replacer {
				continue
			}
			binary.LittleEndian.PutUint64(bytes, replacer)
			copy(data[i:], bytes)
			st.cur = hintPos{arg: ord, offset: i, replacer: idx}
			if !exec() {
				copy(data[i:], original)
				return false
			}
		}
		copy(data[i:], original)
	}
	return true
}

func checkCompressedArg(arg *DataArg, compMap CompMap, exec func() bool) {
	checkCompressedArgAt(arg, compMap, 0, new(hintState), exec)
}

func checkCompressedArgAt(arg *DataArg, compMap CompMap, ord int, st *hintState, exec func() bool) bool {
	data0 := arg.Data()
	data, dtor := image.MustDecompress(data0)
	// Images are very large so the generic algorithm for data arguments
	// can produce too many mutants. For images we consider only
	// 4/8-byte aligned ints. This is enough to handle all magic
	// numbers and checksums. We also ignore 0 and ^uint64(0) source bytes,
	// because there are too many of these in lots of images.
	// On return the image is unmapped and the original data is restored.
	defer func() {
		dtor()
		arg.SetData(data0)
	}()
	bytes := make([]byte, 8)
	for i := 0; i < len(data); i += 4 {
		if ord == st.from.arg && i < st.from.offset {
			continue
		}
		original := make([]byte, 8)
		copy(original, data[i:])
		val := binary.LittleEndian.Uint64(original)
		for idx, replacer := range shrinkExpand(val, compMap, 64, true) {
			if ord == st.from.arg && i == st.from.offset && idx < st.from.replacer {
				continue
			}
			binary.LittleEndian.PutUint64(bytes, replacer)
			copy(data[i:], bytes)
			arg.SetData(image.Compress(data))
			// Unmap the image for the duration of the execution.
			// Execution can take a while and uncompressed images are large,
			// since hints jobs are executed round-robin, we can have thousands of them running.
			dtor()
			dtor = func() {}
			st.cur = hintPos{arg: ord, offset: i, replacer: idx}
			if !exec() {
				return false
			}
			data, dtor = image.MustDecompress(data0)
		}
		copy(data[i:], original)
	}
	return true
}

// Shrink and expand mutations model the cases when the syscall arguments
// are casted to narrower (and wider) integer types.
//
// Motivation for shrink:
//
//	void f(u16 x) {
//			u8 y = (u8)x;
//			if (y == 0xab) {...}
//	}
//
// If we call f(0x1234), then we'll see a comparison 0x34 vs 0xab and we'll
// be unable to match the argument 0x1234 with any of the comparison operands.
// Thus we shrink 0x1234 to 0x34 and try to match 0x34.
// If there's a match for the shrank value, then we replace the corresponding
// bytes of the input (in the given example we'll get 0x12ab).
// Sometimes the other comparison operand will be wider than the shrank value
// (in the example above consider comparison if (y == 0xdeadbeef) {...}).
// In this case we ignore such comparison because we couldn't come up with
// any valid code example that does similar things. To avoid such comparisons
// we check the sizes with leastSize().
//
// Motivation for expand:
//
//	void f(i8 x) {
//			i16 y = (i16)x;
//			if (y == -2) {...}
//	}
//
// Suppose we call f(-1), then we'll see a comparison 0xffff vs 0xfffe and be
// unable to match input vs any operands. Thus we sign extend the input and
// check the extension.
// As with shrink we ignore cases when the other operand is wider.
// Note that executor sign extends all the comparison operands to int64.
func shrinkExpand(v uint64, compMap CompMap, bitsize uint64, image bool) []uint64 {
	v = truncateToBitSize(v, bitsize)
	limit := uint64(1<<bitsize - 1)
	var replacers map[uint64]bool
	for _, iwidth := range []int{8, 4, 2, 1, -4, -2, -1} {
		var width int
		var size, mutant uint64
		if iwidth > 0 {
			width = iwidth
			size = uint64(width) * 8
			mutant = v & ((1 << size) - 1)
		} else {
			width = -iwidth
			size = min(uint64(width)*8, bitsize)
			if v&(1<<(size-1)) == 0 {
				continue
			}
			mutant = v | ^((1 << size) - 1)
		}
		if image {
			// For images we can produce too many mutants for small integers.
			if width < 4 {
				continue
			}
			if mutant == 0 || (mutant|^((1<<size)-1)) == ^uint64(0) {
				continue
			}
		}
		// Use big-endian match/replace for both blobs and ints.
		// Sometimes we have unmarked blobs (no little/big-endian info);
		// for ANYBLOBs we intentionally lose all marking;
		// but even for marked ints we may need this too.
		// Consider that kernel code does not convert the data
		// (i.e. not ntohs(pkt->proto) == ETH_P_BATMAN),
		// but instead converts the constant (i.e. pkt->proto == htons(ETH_P_BATMAN)).
		// In such case we will see dynamic operand that does not match what we have in the program.
		for _, bigendian := range []bool{false, true} {
			if bigendian {
				if width == 1 {
					continue
				}
				mutant = swapInt(mutant, width)
			}
			for newV := range compMap[mutant] {
				// Check the limit for negative numbers.
				if newV > limit && ((^(limit >> 1) & newV) != ^(limit >> 1)) {
					continue
				}
				mask := uint64(1<<size - 1)
				newHi := newV & ^mask
				newV = newV & mask
				if newHi != 0 && newHi^^mask != 0 {
					continue
				}
				if bigendian {
					newV = swapInt(newV, width)
				}
				// We insert special ints (like 0) with high probability,
				// so we don't try to replace to special ints them here.
				// Images are large so it's hard to guess even special
				// ints with random mutations.
				if !image && specialIntsSet[newV] {
					continue
				}
				// Replace size least significant bits of v with
				// corresponding bits of newV. Leave the rest of v as it was.
				replacer := (v &^ mask) | newV
				if replacer == v {
					continue
				}
				replacer = truncateToBitSize(replacer, bitsize)
				// TODO(dvyukov): should we try replacing with arg+/-1?
				// This could trigger some off-by-ones.
				if replacers == nil {
					replacers = make(map[uint64]bool)
				}
				replacers[replacer] = true
			}
		}
	}
	if replacers == nil {
		return nil
	}
	res := slices.Sorted(maps.Keys(replacers))
	return res
}

type HintsLimiter struct {
	mu       sync.Mutex
	attempts map[uint64]int // replacement attempts per PC
}

// Limit restricts hints to at most N replacement attempts per single kernel PC
// (globally, across all hints mutations for all programs).
// We are getting too many generated candidates, the fuzzer may not keep up
// with them at all (hints jobs keep growing infinitely). If a hint indeed came
// from the input w/o transformation, then we should guess it on the first
// attempt (or at least after few attempts). If it did not come from the input,
// or came with a non-trivial transformation, then any number of attempts won't
// help. So limit the total number of attempts (until the next restart).
func (limiter *HintsLimiter) Limit(comps CompMap) {
	const N = 10
	limiter.mu.Lock()
	defer limiter.mu.Unlock()
	if limiter.attempts == nil {
		limiter.attempts = make(map[uint64]int)
	}
	for op1, ops2 := range comps {
		for op2, pcs := range ops2 {
			for pc := range pcs {
				limiter.attempts[pc]++
				if limiter.attempts[pc] > N {
					delete(pcs, pc)
				}
			}
			if len(pcs) == 0 {
				delete(ops2, op2)
			}
		}
		if len(ops2) == 0 {
			delete(comps, op1)
		}
	}
}

func init() {
	specialIntsSet = make(map[uint64]bool)
	for _, v := range specialInts {
		specialIntsSet[v] = true
	}
}
