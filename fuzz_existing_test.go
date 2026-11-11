package fuzz

import (
	"fmt"
	"math/rand"
	"reflect"
	"regexp"
	"strings"
	"testing"
	"time"
	"unsafe"
)

// fxCountingSource wraps a rand.Source and counts how many times the
// Fuzzer's random stream is tapped.
type fxCountingSource struct {
	inner rand.Source
	n     int64
}

func (s *fxCountingSource) Int63() int64    { s.n++; return s.inner.Int63() }
func (s *fxCountingSource) Seed(seed int64) { s.inner.Seed(seed) }

func fxCounter(seed int64) *fxCountingSource {
	return &fxCountingSource{inner: rand.NewSource(seed)}
}

type fxLeaf struct {
	Value uint64
	Tag   string
}

type fxNode struct {
	Name        string
	Left, Right *fxLeaf
	Next        *fxNode
	Spare       *fxLeaf
	Values      []uint16
	Lookup      map[string]*fxLeaf
	LookupAgain map[string]*fxLeaf
}

// fxSelfFuzzer implements fuzz.Interface; FuzzExisting must never call it.
type fxSelfFuzzer struct{ N uint64 }

func (s *fxSelfFuzzer) Fuzz(c Continue) { s.N = 424242 }

type fxIfaceBox struct{ Any interface{} }

type fxNamedStr string
type fxNamedInt int

func TestFuzzExistingContract(t *testing.T) {
	// ---------------------------------------------------------------
	// topology: shared nodes, cycles, nils, slice and map identity.
	// ---------------------------------------------------------------
	func() {
		shared := &fxLeaf{}
		cyclic := &fxNode{}
		cyclic.Next = cyclic
		mapLeaf := &fxLeaf{}
		m := map[string]*fxLeaf{"a": mapLeaf}
		values := make([]uint16, 3, 5)
		n := &fxNode{
			Left: shared, Right: shared,
			Next: cyclic, Values: values,
			Lookup: m, LookupAgain: m,
		}
		valuesPtr := &values[0]
		mapPtr := reflect.ValueOf(m).Pointer()

		if err := NewWithSeed(99).FuzzExisting(n); err != nil {
			t.Fatalf("topology: %v", err)
		}
		// Pointers are never re-pointed.
		if n.Left != shared || n.Right != shared || n.Left != n.Right {
			t.Fatal("topology: shared leaf pointer identity changed")
		}
		if n.Next != cyclic || cyclic.Next != cyclic {
			t.Fatal("topology: cycle was rebuilt")
		}
		if n.Spare != nil {
			t.Fatal("topology: nil pointer was replaced")
		}
		// Slice keeps length, capacity, and backing store.
		if len(n.Values) != 3 || cap(n.Values) != 5 || &n.Values[0] != valuesPtr {
			t.Fatal("topology: slice header or backing store changed")
		}
		// Map keeps identity, key set, and value object identity.
		if reflect.ValueOf(n.Lookup).Pointer() != mapPtr ||
			reflect.ValueOf(n.LookupAgain).Pointer() != mapPtr {
			t.Fatal("topology: map identity changed")
		}
		if len(n.Lookup) != 1 || n.Lookup["a"] != mapLeaf {
			t.Fatal("topology: map key set or value identity changed")
		}
		// A second call must generate again: traversal state is per-call.
		cs := fxCounter(7)
		if err := New().RandSource(cs).FuzzExisting(n); err != nil {
			t.Fatalf("topology second call: %v", err)
		}
		if cs.n == 0 {
			t.Fatal("topology: second call consumed no randomness; stale traversal state?")
		}

		// A shared leaf is generated exactly once.
		var x uint64
		pair := struct{ P, Q *uint64 }{&x, &x}
		csPair := fxCounter(5)
		if err := New().RandSource(csPair).FuzzExisting(&pair); err != nil {
			t.Fatalf("topology shared leaf: %v", err)
		}
		var y uint64
		single := struct{ P *uint64 }{&y}
		csSingle := fxCounter(5)
		if err := New().RandSource(csSingle).FuzzExisting(&single); err != nil {
			t.Fatalf("topology single leaf: %v", err)
		}
		if csPair.n != csSingle.n {
			t.Fatalf("topology: shared leaf consumed %d draws, want %d (once)", csPair.n, csSingle.n)
		}
	}()
	fmt.Println("PASS topology")

	// ---------------------------------------------------------------
	// interface_values: dynamic types are preserved; scalars and value
	// structs are written back; pointers/maps/slices are followed.
	// ---------------------------------------------------------------
	func() {
		// Scalar in an interface is written back with the same value a
		// plain field would get from the same seed.
		h := fxIfaceBox{Any: int64(0)}
		if err := NewWithSeed(31).FuzzExisting(&h); err != nil {
			t.Fatalf("interface int64: %v", err)
		}
		plain := struct{ V int64 }{}
		if err := NewWithSeed(31).FuzzExisting(&plain); err != nil {
			t.Fatalf("interface int64 plain: %v", err)
		}
		got, ok := h.Any.(int64)
		if !ok {
			t.Fatalf("interface_values: dynamic type changed to %T", h.Any)
		}
		if got != plain.V {
			t.Fatal("interface_values: int64 in interface not written back in place")
		}

		hs := fxIfaceBox{Any: "initial"}
		if err := NewWithSeed(32).FuzzExisting(&hs); err != nil {
			t.Fatalf("interface string: %v", err)
		}
		plainS := struct{ V string }{}
		if err := NewWithSeed(32).FuzzExisting(&plainS); err != nil {
			t.Fatalf("interface string plain: %v", err)
		}
		if s, ok := hs.Any.(string); !ok || s != plainS.V {
			t.Fatal("interface_values: string in interface not written back in place")
		}

		// Value struct in an interface is filled field by field and
		// written back, keeping its dynamic type.
		hv := fxIfaceBox{Any: fxLeaf{}}
		if err := NewWithSeed(33).FuzzExisting(&hv); err != nil {
			t.Fatalf("interface struct: %v", err)
		}
		plainL := struct{ V fxLeaf }{}
		if err := NewWithSeed(33).FuzzExisting(&plainL); err != nil {
			t.Fatalf("interface struct plain: %v", err)
		}
		if l, ok := hv.Any.(fxLeaf); !ok || l != plainL.V {
			t.Fatal("interface_values: value struct in interface not written back in place")
		}

		// Pointer inside an interface: the original object is followed,
		// never replaced.
		leaf := &fxLeaf{}
		hp := fxIfaceBox{Any: leaf}
		if err := NewWithSeed(34).FuzzExisting(&hp); err != nil {
			t.Fatalf("interface pointer: %v", err)
		}
		if hp.Any.(*fxLeaf) != leaf {
			t.Fatal("interface_values: pointer inside interface was replaced")
		}

		// Map inside an interface keeps its identity.
		m := map[string]uint64{"k": 0}
		mPtr := reflect.ValueOf(m).Pointer()
		hm := fxIfaceBox{Any: m}
		if err := NewWithSeed(35).FuzzExisting(&hm); err != nil {
			t.Fatalf("interface map: %v", err)
		}
		if reflect.ValueOf(hm.Any.(map[string]uint64)).Pointer() != mPtr {
			t.Fatal("interface_values: map inside interface was replaced")
		}

		// Slice inside an interface keeps its backing store.
		sl := []uint64{1, 2}
		slPtr := &sl[0]
		hsl := fxIfaceBox{Any: sl}
		if err := NewWithSeed(36).FuzzExisting(&hsl); err != nil {
			t.Fatalf("interface slice: %v", err)
		}
		if &hsl.Any.([]uint64)[0] != slPtr {
			t.Fatal("interface_values: slice inside interface was reallocated")
		}

		// A typed nil pointer inside an interface stays exactly that.
		var nilLeaf *fxLeaf
		hn := fxIfaceBox{Any: nilLeaf}
		if err := NewWithSeed(37).FuzzExisting(&hn); err != nil {
			t.Fatalf("interface typed nil: %v", err)
		}
		if hn.Any == nil {
			t.Fatal("interface_values: typed nil pointer was dropped from interface")
		}
		if hn.Any.(*fxLeaf) != nil {
			t.Fatal("interface_values: typed nil pointer was replaced with a new object")
		}

		// A nil interface stays nil.
		var he fxIfaceBox
		if err := NewWithSeed(38).FuzzExisting(&he); err != nil {
			t.Fatalf("interface nil: %v", err)
		}
		if he.Any != nil {
			t.Fatal("interface_values: nil interface was filled")
		}
	}()
	fmt.Println("PASS interface_values")

	// ---------------------------------------------------------------
	// overlapping_slices: shared elements are generated once, so all
	// views observe consistent values; the backing array is untouched.
	// ---------------------------------------------------------------
	func() {
		backing := make([]uint64, 6)
		g := &struct {
			A, B []uint64
			P    *uint64
		}{
			A: backing[0:4],
			B: backing[2:6],
			P: &backing[3],
		}
		if err := NewWithSeed(51).FuzzExisting(g); err != nil {
			t.Fatalf("overlapping slices: %v", err)
		}
		if &g.A[0] != &backing[0] || len(g.A) != 4 || len(g.B) != 4 {
			t.Fatal("overlapping_slices: backing array or lengths changed")
		}
		if g.A[2] != g.B[0] || g.A[3] != g.B[1] {
			t.Fatal("overlapping_slices: shared elements disagree")
		}
		if *g.P != g.A[3] || *g.P != g.B[1] {
			t.Fatal("overlapping_slices: pointer into slice disagrees with element")
		}
	}()
	fmt.Println("PASS overlapping_slices")

	// ---------------------------------------------------------------
	// map_order: same logical map built with different insertion orders
	// replays identically; a shared map is traversed once.
	// ---------------------------------------------------------------
	func() {
		type mapGraph struct {
			S map[string]uint64
			I map[int]string
			B map[bool]uint64
			N map[fxNamedStr]uint64
			K map[fxNamedInt]string
		}
		build := func(reverse bool) *mapGraph {
			g := &mapGraph{
				S: map[string]uint64{},
				I: map[int]string{},
				B: map[bool]uint64{},
				N: map[fxNamedStr]uint64{},
				K: map[fxNamedInt]string{},
			}
			strs := []string{"alpha", "beta", "gamma"}
			ints := []int{3, 1, 2}
			bools := []bool{true, false}
			if reverse {
				for i, j := 0, len(strs)-1; i < j; i, j = i+1, j-1 {
					strs[i], strs[j] = strs[j], strs[i]
				}
				for i, j := 0, len(ints)-1; i < j; i, j = i+1, j-1 {
					ints[i], ints[j] = ints[j], ints[i]
				}
				for i, j := 0, len(bools)-1; i < j; i, j = i+1, j-1 {
					bools[i], bools[j] = bools[j], bools[i]
				}
			}
			for _, s := range strs {
				g.S[s] = 0
				g.N[fxNamedStr(s)] = 0
			}
			for _, i := range ints {
				g.I[i] = ""
				g.K[fxNamedInt(i)] = ""
			}
			for _, b := range bools {
				g.B[b] = 0
			}
			return g
		}
		g1, g2 := build(false), build(true)
		cs := fxCounter(61)
		if err := New().RandSource(cs).FuzzExisting(g1); err != nil {
			t.Fatalf("map_order g1: %v", err)
		}
		if err := New().RandSource(rand.NewSource(61)).FuzzExisting(g2); err != nil {
			t.Fatalf("map_order g2: %v", err)
		}
		if !reflect.DeepEqual(g1, g2) {
			t.Fatal("map_order: insertion order changed the result")
		}
		if cs.n == 0 {
			t.Fatal("map_order: no randomness consumed; comparison is vacuous")
		}

		// A map shared by two fields is traversed exactly once.
		shared := map[string]uint64{"a": 0, "b": 0, "c": 0}
		two := struct{ M1, M2 map[string]uint64 }{shared, shared}
		csTwo := fxCounter(62)
		if err := New().RandSource(csTwo).FuzzExisting(&two); err != nil {
			t.Fatalf("map_order shared: %v", err)
		}
		one := struct{ M1 map[string]uint64 }{map[string]uint64{"a": 0, "b": 0, "c": 0}}
		csOne := fxCounter(62)
		if err := New().RandSource(csOne).FuzzExisting(&one); err != nil {
			t.Fatalf("map_order single: %v", err)
		}
		if csTwo.n != csOne.n {
			t.Fatalf("map_order: shared map traversed more than once (%d vs %d draws)", csTwo.n, csOne.n)
		}
	}()
	fmt.Println("PASS map_order")

	// ---------------------------------------------------------------
	// policies: SkipFieldsWithPattern and AllowUnexportedFields keep
	// their meaning; Funcs, fuzz.Interface, and the time.Time handler
	// are not invoked.
	// ---------------------------------------------------------------
	func() {
		type policyStruct struct {
			Keep   uint64
			SkipMe uint64
			hidden uint64
		}
		const sentinel = uint64(0x5e171e1)

		// Skipped and inaccessible fields are untouched and free.
		p := policyStruct{Keep: sentinel, SkipMe: sentinel, hidden: sentinel}
		cs := fxCounter(71)
		f := New().RandSource(cs).SkipFieldsWithPattern(regexp.MustCompile("^Skip"))
		if err := f.FuzzExisting(&p); err != nil {
			t.Fatalf("policies skip: %v", err)
		}
		if p.SkipMe != sentinel || p.hidden != sentinel {
			t.Fatal("policies: skipped or unexported field was modified")
		}
		only := struct{ Only uint64 }{}
		csOnly := fxCounter(71)
		if err := New().RandSource(csOnly).FuzzExisting(&only); err != nil {
			t.Fatalf("policies single: %v", err)
		}
		if cs.n != csOnly.n {
			t.Fatalf("policies: skipped fields consumed randomness (%d vs %d draws)", cs.n, csOnly.n)
		}

		// AllowUnexportedFields opts unexported fields back in.
		p2 := policyStruct{Keep: sentinel, SkipMe: sentinel, hidden: sentinel}
		cs2 := fxCounter(72)
		f2 := New().RandSource(cs2).AllowUnexportedFields(true).
			SkipFieldsWithPattern(regexp.MustCompile("^Skip"))
		if err := f2.FuzzExisting(&p2); err != nil {
			t.Fatalf("policies unexported: %v", err)
		}
		if p2.SkipMe != sentinel {
			t.Fatal("policies: skipped field modified with unexported allowed")
		}
		two := struct{ A, B uint64 }{}
		csTwo := fxCounter(72)
		if err := New().RandSource(csTwo).FuzzExisting(&two); err != nil {
			t.Fatalf("policies pair: %v", err)
		}
		if cs2.n != csTwo.n {
			t.Fatalf("policies: unexported field not filled as a normal leaf (%d vs %d draws)", cs2.n, csTwo.n)
		}

		// Custom Funcs are not invoked by FuzzExisting.
		const marker = uint64(0xdeadbeef)
		leaf := &fxLeaf{}
		fc := NewWithSeed(73).Funcs(func(l *fxLeaf, c Continue) { l.Value = marker })
		if err := fc.FuzzExisting(leaf); err != nil {
			t.Fatalf("policies funcs: %v", err)
		}
		if leaf.Value == marker {
			t.Fatal("policies: custom Funcs callback was invoked")
		}

		// fuzz.Interface is not invoked either.
		sf := &fxSelfFuzzer{}
		if err := NewWithSeed(74).FuzzExisting(sf); err != nil {
			t.Fatalf("policies interface: %v", err)
		}
		if sf.N == 424242 {
			t.Fatal("policies: fuzz.Interface hook was invoked")
		}

		// The default time.Time handler is not invoked; a time.Time is
		// just a struct with unexported fields here.
		tt := struct{ T time.Time }{}
		if err := NewWithSeed(75).FuzzExisting(&tt); err != nil {
			t.Fatalf("policies time: %v", err)
		}
		if !tt.T.IsZero() {
			t.Fatal("policies: default time.Time handler was invoked")
		}
	}()
	fmt.Println("PASS policies")

	// ---------------------------------------------------------------
	// errors: pre-flight checks fail with positioned errors and leave
	// the graph and the random source untouched.
	// ---------------------------------------------------------------
	func() {
		const sentinel = uint64(0x5e171e1)

		var nilNode *fxNode
		if err := NewWithSeed(81).FuzzExisting(nilNode); err == nil {
			t.Fatal("errors: nil pointer root accepted")
		}
		if err := NewWithSeed(81).FuzzExisting(fxNode{}); err == nil {
			t.Fatal("errors: non-pointer root accepted")
		}
		if err := NewWithSeed(81).FuzzExisting(nil); err == nil {
			t.Fatal("errors: nil root accepted")
		}

		// chan in the reachable region: error names the field, and
		// nothing is generated.
		withChan := struct {
			A  uint64
			Ch chan int
		}{A: sentinel}
		cs := fxCounter(82)
		err := New().RandSource(cs).FuzzExisting(&withChan)
		if err == nil || !strings.Contains(err.Error(), "Ch") || !strings.Contains(err.Error(), "chan") {
			t.Fatalf("errors: chan check missing or unpositioned: %v", err)
		}
		if withChan.A != sentinel || cs.n != 0 {
			t.Fatal("errors: failed check modified the graph or consumed randomness")
		}

		// func and unsafe.Pointer are rejected the same way.
		withFunc := struct{ Fn func() }{}
		if err := NewWithSeed(83).FuzzExisting(&withFunc); err == nil || !strings.Contains(err.Error(), "Fn") {
			t.Fatalf("errors: func check missing or unpositioned: %v", err)
		}
		withUnsafe := struct{ P unsafe.Pointer }{}
		if err := NewWithSeed(84).FuzzExisting(&withUnsafe); err == nil || !strings.Contains(err.Error(), "P") {
			t.Fatalf("errors: unsafe.Pointer check missing or unpositioned: %v", err)
		}

		// Unsupported map key type: error names the map's position.
		withBadMap := struct{ Scores map[float64]uint64 }{Scores: map[float64]uint64{1.5: 2}}
		csMap := fxCounter(85)
		err = New().RandSource(csMap).FuzzExisting(&withBadMap)
		if err == nil || !strings.Contains(err.Error(), "Scores") || !strings.Contains(err.Error(), "float64") {
			t.Fatalf("errors: map key check missing or unpositioned: %v", err)
		}
		if withBadMap.Scores[1.5] != 2 || csMap.n != 0 {
			t.Fatal("errors: failed map key check modified the graph or consumed randomness")
		}

		// Skipped fields are exempt from the checks.
		skipped := struct {
			A  uint64
			Ch chan int
		}{}
		fs := New().RandSource(fxCounter(86)).SkipFieldsWithPattern(regexp.MustCompile("^Ch$"))
		if err := fs.FuzzExisting(&skipped); err != nil {
			t.Fatalf("errors: skipped field was checked: %v", err)
		}

		// Inaccessible unexported fields are exempt too.
		unexported := struct{ ch chan int }{}
		if err := NewWithSeed(87).FuzzExisting(&unexported); err != nil {
			t.Fatalf("errors: unexported field was checked: %v", err)
		}
	}()
	fmt.Println("PASS errors")

	// ---------------------------------------------------------------
	// old_stream: leaves come from the Fuzzer's own random stream with
	// the same interpretation as Fuzz, and existing entries are
	// unaffected.
	// ---------------------------------------------------------------
	func() {
		type streamStruct struct {
			B bool
			I int64
			U uint32
			F float64
			C complex128
			S string
		}
		var a, b streamStruct
		if err := NewWithSeed(123).FuzzExisting(&a); err != nil {
			t.Fatalf("old_stream FuzzExisting: %v", err)
		}
		NewWithSeed(123).Fuzz(&b)
		if a != b {
			t.Fatal("old_stream: FuzzExisting does not match Fuzz on the same stream")
		}

		// FuzzExisting draws from the same stream: interleaving it with
		// Fuzz shifts subsequent Fuzz output, and the whole sequence
		// replays under the same seed.
		run := func(withMiddle bool) (uint64, uint64, fxLeaf) {
			f := NewWithSeed(7)
			var x, y uint64
			l := &fxLeaf{}
			f.Fuzz(&x)
			if withMiddle {
				if err := f.FuzzExisting(l); err != nil {
					t.Fatalf("old_stream interleave: %v", err)
				}
			}
			f.Fuzz(&y)
			return x, y, *l
		}
		x1, y1, l1 := run(true)
		x2, y2, l2 := run(true)
		if x1 != x2 || y1 != y2 || l1 != l2 {
			t.Fatal("old_stream: same seed and same graph did not replay")
		}
		x3, y3, _ := run(false)
		if x3 != x1 {
			t.Fatal("old_stream: stream diverged before FuzzExisting")
		}
		if y3 == y1 {
			t.Fatal("old_stream: FuzzExisting did not draw from the Fuzzer's own stream")
		}

		// NewFromGoFuzz keeps its constant byte translation.
		data := []byte("local-benign-input-012345678901234567890123456789")
		var p, q struct {
			A uint64
			S string
		}
		NewFromGoFuzz(data).Fuzz(&p)
		NewFromGoFuzz(data).Fuzz(&q)
		if p != q {
			t.Fatal("old_stream: NewFromGoFuzz replay broke")
		}
	}()
	fmt.Println("PASS old_stream")
}
