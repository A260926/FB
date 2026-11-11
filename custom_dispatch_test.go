// Copyright 2023 The go-fuzz-headers Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package gofuzzheaders

import (
	"errors"
	"testing"
)

// The toy structures below exist only to exercise custom dispatch;
// they do not model any real protocol or file format.

// toyHeader is the "Header" part: a handler consumes bytes from the
// shared consumer and fills the two related fields together.
type toyHeader struct {
	A byte
	B byte
}

// toyPacket pairs a Header with a trailing plain byte field. The
// trailing field must read exactly the next byte after whatever the
// header handler consumed.
type toyPacket struct {
	Hdr  toyHeader
	Tail byte
}

type toyLeaf struct {
	V byte
}

// toyWithLeaf has only a leaf-level handler registered, while the
// rest of the structure keeps using default generation.
type toyWithLeaf struct {
	Leaf toyLeaf
	Tail byte
}

type toyNested struct {
	Inner toyHeader
	Tail  byte
}

type toyOuter struct {
	Sub  toyNested
	Tail byte
}

// toyByte is a newly defined named type. It shares byte's underlying
// type but must not be confused with it when looking up handlers.
type toyByte byte

// toyAlias is a genuine type alias, so it is exactly the same type as
// byte and must match a byte handler.
type toyAlias = byte

type toyNamedPacket struct {
	X toyByte
	Y byte
}

type toyAliasHolder struct {
	Z toyAlias
	W byte
}

type toyMapPacket struct {
	M    map[string]byte
	Tail byte
}

type toyPtrPacket struct {
	P    *toyLeaf
	Tail byte
}

type toyUnexported struct {
	First  byte
	hidden byte
	Last   byte
}

type toyContPacket struct {
	Hdr  toyHeader
	Mid  toyLeaf
	Tail byte
}

// errToyBoom is a handler-local sentinel used to test error wrapping.
var errToyBoom = errors.New("toy boom")

func remainingBytes(f *ConsumeFuzzer) int {
	return int(f.dataTotal - f.position)
}

func makeHeaderHandler(calls *int) func(*toyHeader, Continue) error {
	return func(h *toyHeader, c Continue) error {
		*calls++
		a, err := c.F.GetByte()
		if err != nil {
			return err
		}
		if _, err := c.F.GetByte(); err != nil {
			return err
		}
		// Constants, not derived from the second byte: default logic
		// must not overwrite them afterwards.
		h.A = a
		h.B = 88
		return nil
	}
}

func makeLeafHandler(calls *int) func(*toyLeaf, Continue) error {
	return func(v *toyLeaf, c Continue) error {
		*calls++
		b, err := c.F.GetByte()
		if err != nil {
			return err
		}
		v.V = b
		return nil
	}
}

func mustPanic(t *testing.T, name string, fn func()) {
	t.Helper()
	defer func() {
		if recover() == nil {
			t.Fatalf("%s: expected AddFuncs to panic", name)
		}
	}()
	fn()
}

func TestCustomDispatch(t *testing.T) {
	t.Run("NoCustomHandler", func(t *testing.T) {
		data := []byte{1, 2, 3, 4}
		f := NewConsumer(data)
		calls := 0
		// A handler is registered for an unrelated type, so toyPacket
		// must be generated exactly like GenerateStruct would.
		f.AddFuncs([]interface{}{makeLeafHandler(&calls)})

		var pkt toyPacket
		if err := f.GenerateWithCustom(&pkt); err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if pkt.Hdr.A != 1 || pkt.Hdr.B != 2 || pkt.Tail != 3 {
			t.Fatalf("default generation changed: got %+v", pkt)
		}
		if calls != 0 {
			t.Fatalf("unrelated handler ran %d times", calls)
		}
		if remainingBytes(f) != 1 {
			t.Fatalf("expected 1 byte left, got %d", remainingBytes(f))
		}
		b, err := f.GetByte()
		if err != nil || b != 4 {
			t.Fatalf("remaining byte not readable normally: %v %d", err, b)
		}
	})

	t.Run("LeafOnly", func(t *testing.T) {
		data := []byte{7, 8, 9}
		f := NewConsumer(data)
		calls := 0
		f.AddFuncs([]interface{}{func(v *toyLeaf, c Continue) error {
			calls++
			b, err := c.F.GetByte()
			if err != nil {
				return err
			}
			// Fixed value, while still consuming one byte: proves the
			// handler owns both the assignment and the byte cursor.
			v.V = b + 100
			return nil
		}})

		var pkt toyWithLeaf
		if err := f.GenerateWithCustom(&pkt); err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if calls != 1 {
			t.Fatalf("leaf handler called %d times, want 1", calls)
		}
		if pkt.Leaf.V != 107 {
			t.Fatalf("leaf value = %d, want 107", pkt.Leaf.V)
		}
		if pkt.Tail != 8 {
			t.Fatalf("tail must read the next byte after the handler, got %d", pkt.Tail)
		}
		if remainingBytes(f) != 1 {
			t.Fatalf("expected 1 byte left, got %d", remainingBytes(f))
		}
	})

	t.Run("HeaderFullTakeover", func(t *testing.T) {
		data := []byte{10, 20, 30, 40}

		f := NewConsumer(data)
		calls := 0
		f.AddFuncs([]interface{}{makeHeaderHandler(&calls)})

		var pkt toyPacket
		if err := f.GenerateWithCustom(&pkt); err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if calls != 1 {
			t.Fatalf("header handler called %d times, want 1", calls)
		}
		if pkt.Hdr.A != 10 || pkt.Hdr.B != 88 {
			t.Fatalf("handler values overwritten: got %+v", pkt.Hdr)
		}
		if pkt.Tail != 30 {
			t.Fatalf("tail = %d, want 30 (next byte after 2 handler bytes)", pkt.Tail)
		}
		if remainingBytes(f) != 1 {
			t.Fatalf("expected 1 byte left, got %d", remainingBytes(f))
		}

		// The same handler must fire through nested struct fields.
		f2 := NewConsumer(data)
		nestedCalls := 0
		f2.AddFuncs([]interface{}{makeHeaderHandler(&nestedCalls)})
		var outer toyOuter
		if err := f2.GenerateWithCustom(&outer); err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if nestedCalls != 1 {
			t.Fatalf("nested header handler called %d times, want 1", nestedCalls)
		}
		if outer.Sub.Inner.A != 10 || outer.Sub.Inner.B != 88 || outer.Sub.Tail != 30 || outer.Tail != 40 {
			t.Fatalf("nested generation wrong: %+v", outer)
		}
	})

	t.Run("RootFullTakeover", func(t *testing.T) {
		data := []byte{1, 2, 3}
		f := NewConsumer(data)
		calls := 0
		f.AddFuncs([]interface{}{func(p *toyPacket, c Continue) error {
			calls++
			// Complete takeover: assign everything, consume nothing.
			p.Hdr.A = 99
			p.Hdr.B = 88
			p.Tail = 77
			return nil
		}})

		var pkt toyPacket
		if err := f.GenerateWithCustom(&pkt); err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if calls != 1 {
			t.Fatalf("root handler called %d times, want 1", calls)
		}
		if pkt.Hdr.A != 99 || pkt.Hdr.B != 88 || pkt.Tail != 77 {
			t.Fatalf("root takeover did not own the value: %+v", pkt)
		}
		if f.position != 0 || remainingBytes(f) != 3 {
			t.Fatalf("root handler consumed bytes: position=%d", f.position)
		}
		b, err := f.GetByte()
		if err != nil || b != 1 {
			t.Fatalf("input cursor moved: %v %d", err, b)
		}
	})

	t.Run("MapTakeover", func(t *testing.T) {
		mapHandler := func(calls *int) func(map[string]byte, Continue) error {
			return func(m map[string]byte, c Continue) error {
				*calls++
				if m == nil {
					return errors.New("handler got a nil map")
				}
				b, err := c.F.GetByte()
				if err != nil {
					return err
				}
				m["k"] = b
				return nil
			}
		}

		f := NewConsumer([]byte{5, 9})
		calls := 0
		f.AddFuncs([]interface{}{mapHandler(&calls)})

		// A nil map field must be handed over as a writable non-nil map.
		var pkt toyMapPacket
		if err := f.GenerateWithCustom(&pkt); err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if calls != 1 {
			t.Fatalf("map handler called %d times, want 1", calls)
		}
		if pkt.M == nil || pkt.M["k"] != 5 {
			t.Fatalf("map not filled by handler: %v", pkt.M)
		}
		if pkt.Tail != 9 {
			t.Fatalf("tail = %d, want 9", pkt.Tail)
		}

		// A pre-initialized map is passed straight through.
		f2 := NewConsumer([]byte{6, 4})
		f2.AddFuncs([]interface{}{mapHandler(new(int))})
		pkt2 := toyMapPacket{M: map[string]byte{"pre": 7}}
		if err := f2.GenerateWithCustom(&pkt2); err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if pkt2.M["pre"] != 7 || pkt2.M["k"] != 6 || pkt2.Tail != 4 {
			t.Fatalf("pre-existing map not preserved: %v tail=%d", pkt2.M, pkt2.Tail)
		}
	})

	t.Run("NamedAndAliasTypes", func(t *testing.T) {
		// byte and uint8 (and the toyAlias alias) are one single type;
		// the separately defined toyByte is not.
		f := NewConsumer([]byte{10, 20})
		byteCalls := 0
		f.AddFuncs([]interface{}{func(v *byte, c Continue) error {
			byteCalls++
			b, err := c.F.GetByte()
			if err != nil {
				return err
			}
			*v = b + 100
			return nil
		}})

		var np toyNamedPacket
		if err := f.GenerateWithCustom(&np); err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if np.X != 10 {
			t.Fatalf("named type toyByte must fall through to default, got %d", np.X)
		}
		if np.Y != 120 || byteCalls != 1 {
			t.Fatalf("byte field should be handled, got Y=%d calls=%d", np.Y, byteCalls)
		}

		// A genuine alias is the same type as byte, so the handler matches.
		f2 := NewConsumer([]byte{30, 40})
		aliasCalls := 0
		f2.AddFuncs([]interface{}{func(v *byte, c Continue) error {
			aliasCalls++
			b, err := c.F.GetByte()
			if err != nil {
				return err
			}
			*v = b + 100
			return nil
		}})
		var ah toyAliasHolder
		if err := f2.GenerateWithCustom(&ah); err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if ah.Z != 130 || ah.W != 140 || aliasCalls != 2 {
			t.Fatalf("alias fields should match the byte handler: %+v calls=%d", ah, aliasCalls)
		}

		// An exact *toyByte registration does match, once registered.
		f3 := NewConsumer([]byte{3})
		namedCalls := 0
		f3.AddFuncs([]interface{}{func(v *toyByte, c Continue) error {
			namedCalls++
			b, err := c.F.GetByte()
			if err != nil {
				return err
			}
			*v = toyByte(b + 50)
			return nil
		}})
		var nb toyByte
		if err := f3.GenerateWithCustom(&nb); err != nil || nb != 53 || namedCalls != 1 {
			t.Fatalf("exact named handler failed: err=%v value=%d calls=%d", err, nb, namedCalls)
		}
	})

	t.Run("PointerField", func(t *testing.T) {
		// Without a handler, pointer fields keep the old allocate-and-
		// recurse behavior.
		f := NewConsumer([]byte{2, 3})
		var plain toyPtrPacket
		if err := f.GenerateWithCustom(&plain); err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if plain.P == nil || plain.P.V != 2 || plain.Tail != 3 {
			t.Fatalf("default pointer handling changed: %+v", plain)
		}

		// With a handler, the allocated element is handed over once.
		f2 := NewConsumer([]byte{2, 3})
		calls := 0
		f2.AddFuncs([]interface{}{func(v *toyLeaf, c Continue) error {
			calls++
			b, err := c.F.GetByte()
			if err != nil {
				return err
			}
			v.V = b + 200
			return nil
		}})
		var handled toyPtrPacket
		if err := f2.GenerateWithCustom(&handled); err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if calls != 1 || handled.P == nil || handled.P.V != 202 || handled.Tail != 3 {
			t.Fatalf("pointer element not handed to handler: %+v calls=%d", handled, calls)
		}
	})

	t.Run("UnexportedFields", func(t *testing.T) {
		// Default: the unexported field gets neither a handler nor a
		// byte, so First and Last consume the first two bytes.
		f := NewConsumer([]byte{1, 2, 3})
		var skip toyUnexported
		if err := f.GenerateWithCustom(&skip); err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if skip.First != 1 || skip.hidden != 0 || skip.Last != 2 {
			t.Fatalf("unexported field was touched: %+v", skip)
		}
		if remainingBytes(f) != 1 {
			t.Fatalf("unexported field consumed bytes, %d remain", remainingBytes(f))
		}

		// Explicit opt-in restores the writable reflection path.
		f2 := NewConsumer([]byte{1, 2, 3})
		f2.AllowUnexportedFields()
		var filled toyUnexported
		if err := f2.GenerateWithCustom(&filled); err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if filled.First != 1 || filled.hidden != 2 || filled.Last != 3 {
			t.Fatalf("unexported field not filled after opt-in: %+v", filled)
		}
		if remainingBytes(f2) != 0 {
			t.Fatalf("expected all bytes consumed, %d remain", remainingBytes(f2))
		}
	})

	t.Run("HandlerError", func(t *testing.T) {
		f := NewConsumer([]byte{4, 5, 6})
		calls := 0
		f.AddFuncs([]interface{}{func(h *toyHeader, c Continue) error {
			calls++
			b, err := c.F.GetByte()
			if err != nil {
				return err
			}
			h.A = b
			h.B = 88
			return errToyBoom
		}})

		var pkt toyPacket
		err := f.GenerateWithCustom(&pkt)
		if err == nil {
			t.Fatal("expected an error, got nil")
		}
		if !errors.Is(err, ErrCustomFunction) {
			t.Fatalf("error does not wrap ErrCustomFunction: %v", err)
		}
		if !errors.Is(err, errToyBoom) {
			t.Fatalf("error does not wrap the handler error: %v", err)
		}
		if calls != 1 {
			t.Fatalf("handler called %d times, want 1", calls)
		}
		// Assignments and consumed bytes are not rolled back, but the
		// trailing sibling field is never generated.
		if pkt.Hdr.A != 4 || pkt.Hdr.B != 88 || pkt.Tail != 0 {
			t.Fatalf("partial state unexpected: %+v", pkt)
		}
		if f.position != 1 {
			t.Fatalf("cursor = %d, want 1", f.position)
		}
		// Depth bookkeeping is restored: the consumer keeps reading.
		b, err := f.GetByte()
		if err != nil || b != 5 {
			t.Fatalf("consumer unusable after handler error: %v %d", err, b)
		}
		if remainingBytes(f) != 1 {
			t.Fatalf("expected 1 byte left, got %d", remainingBytes(f))
		}
	})

	t.Run("InputExhausted", func(t *testing.T) {
		// The handler needs two bytes but only one is available.
		f := NewConsumer([]byte{4})
		f.AddFuncs([]interface{}{func(h *toyHeader, c Continue) error {
			a, err := c.F.GetByte()
			if err != nil {
				return err
			}
			// Assignment happens before the failing read: it must
			// survive the error instead of being rolled back.
			h.A = a
			b, err := c.F.GetByte()
			if err != nil {
				return err
			}
			h.B = b
			return nil
		}})
		var pkt toyPacket
		err := f.GenerateWithCustom(&pkt)
		if err == nil {
			t.Fatal("expected exhaustion error, got nil")
		}
		if !errors.Is(err, ErrCustomFunction) {
			t.Fatalf("exhaustion error not wrapped in sentinel: %v", err)
		}
		if pkt.Hdr.A != 4 || pkt.Hdr.B != 0 || pkt.Tail != 0 {
			t.Fatalf("partial state unexpected: %+v", pkt)
		}
		if _, err := f.GetByte(); err == nil {
			t.Fatal("expected further reads to keep failing")
		}

		// Default generation also stops at exhaustion.
		f2 := NewConsumer([]byte{1, 2})
		var plain toyPacket
		if err := f2.GenerateStruct(&plain); err == nil {
			t.Fatal("expected default generation to report exhaustion")
		}
	})

	t.Run("InvalidRegistrations", func(t *testing.T) {
		var nilHandler func(*toyHeader, Continue) error
		mustPanic(t, "typed nil", func() {
			NewConsumer(nil).AddFuncs([]interface{}{nilHandler})
		})
		mustPanic(t, "not a func", func() {
			NewConsumer(nil).AddFuncs([]interface{}{42})
		})
		mustPanic(t, "wrong param count", func() {
			NewConsumer(nil).AddFuncs([]interface{}{func(h *toyHeader) error { return nil }})
		})
		mustPanic(t, "wrong Continue type", func() {
			NewConsumer(nil).AddFuncs([]interface{}{func(h *toyHeader, c int) error { return nil }})
		})
		mustPanic(t, "non-error return", func() {
			NewConsumer(nil).AddFuncs([]interface{}{func(h *toyHeader, c Continue) int { return 0 }})
		})
		mustPanic(t, "non-pointer non-map first arg", func() {
			NewConsumer(nil).AddFuncs([]interface{}{func(h toyHeader, c Continue) error { return nil }})
		})

		// A valid registration must still be accepted.
		NewConsumer(nil).AddFuncs([]interface{}{
			func(h *toyHeader, c Continue) error { return nil },
			func(m map[string]byte, c Continue) error { return nil },
		})
	})
}

func TestCustomContinuation(t *testing.T) {
	t.Run("ContinueGenerateStructUsesDefault", func(t *testing.T) {
		data := []byte{10, 20, 30, 40, 50}
		f := NewConsumer(data)
		headerCalls, leafCalls := 0, 0
		var seenConsumer *ConsumeFuzzer
		f.AddFuncs([]interface{}{
			func(h *toyHeader, c Continue) error {
				headerCalls++
				seenConsumer = c.F
				a, err := c.F.GetByte()
				if err != nil {
					return err
				}
				h.A = a
				// Default generation of the subtree: the registered
				// leaf handler must not run inside this call.
				var local toyLeaf
				if err := c.GenerateStruct(&local); err != nil {
					return err
				}
				h.B = local.V
				return nil
			},
			makeLeafHandler(&leafCalls),
		})

		var pkt toyContPacket
		if err := f.GenerateWithCustom(&pkt); err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if seenConsumer != f {
			t.Fatal("Continue created a new consumer instead of sharing it")
		}
		if headerCalls != 1 || leafCalls != 1 {
			t.Fatalf("calls wrong: header=%d leaf=%d (leaf must only fire for Mid)", headerCalls, leafCalls)
		}
		// Header: byte 10 -> A; default leaf inside consumes byte 20.
		// Mid handler then consumes byte 30; Tail takes byte 40.
		if pkt.Hdr.A != 10 || pkt.Hdr.B != 20 || pkt.Mid.V != 30 || pkt.Tail != 40 {
			t.Fatalf("continuation ordering wrong: %+v", pkt)
		}
		if remainingBytes(f) != 1 {
			t.Fatalf("expected 1 byte left, got %d", remainingBytes(f))
		}
	})

	t.Run("ContinueGenerateStructWithCustom", func(t *testing.T) {
		data := []byte{10, 20, 30, 40, 50}
		f := NewConsumer(data)
		headerCalls, leafCalls := 0, 0
		f.AddFuncs([]interface{}{
			func(h *toyHeader, c Continue) error {
				headerCalls++
				var local toyLeaf
				// Custom generation of the subtree keeps handlers on.
				if err := c.GenerateStructWithCustom(&local); err != nil {
					return err
				}
				h.A = local.V
				b, err := c.F.GetByte()
				if err != nil {
					return err
				}
				h.B = b
				return nil
			},
			makeLeafHandler(&leafCalls),
		})

		var pkt toyContPacket
		if err := f.GenerateWithCustom(&pkt); err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		// Once for the leaf nested inside the header handler, once for Mid.
		if headerCalls != 1 || leafCalls != 2 {
			t.Fatalf("calls wrong: header=%d leaf=%d", headerCalls, leafCalls)
		}
		// Nested leaf consumes byte 10; header then consumes byte 20;
		// Mid consumes byte 30; Tail takes byte 40.
		if pkt.Hdr.A != 10 || pkt.Hdr.B != 20 || pkt.Mid.V != 30 || pkt.Tail != 40 {
			t.Fatalf("custom continuation ordering wrong: %+v", pkt)
		}
		if remainingBytes(f) != 1 {
			t.Fatalf("expected 1 byte left, got %d", remainingBytes(f))
		}
	})

	t.Run("RecursionDepthStillLimited", func(t *testing.T) {
		f := NewConsumer(nil)
		calls := 0
		f.AddFuncs([]interface{}{func(h *toyHeader, c Continue) error {
			calls++
			var child toyHeader
			// Deliberately re-enters custom generation for the same
			// type; the existing depth limit is the only guard needed.
			return c.GenerateStructWithCustom(&child)
		}})

		var root toyHeader
		if err := f.GenerateWithCustom(&root); err != nil {
			t.Fatalf("depth limit should stop recursion without error: %v", err)
		}
		if calls == 0 || calls > maxDepth {
			t.Fatalf("recursion not bounded by maxDepth=%d, calls=%d", maxDepth, calls)
		}
	})
}
