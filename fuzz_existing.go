/*
Copyright 2014 Google Inc. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

package fuzz

import (
	"fmt"
	"reflect"
	"sort"
	"unsafe"
)

// FuzzExisting fills the leaf values of the object graph reachable from obj
// in place, without rebuilding the graph. It is the explicit entry point for
// callers that have already assembled cycles, shared nodes, or overlapping
// slices and only want the values inside regenerated.
//
// obj must be a non-nil pointer. Everything reachable from it is walked, but
// only bool, integer, float, complex, and string leaves receive random
// content. The existing structure is preserved exactly:
//
//   - pointers are never re-pointed; nil pointers stay nil;
//   - slices keep their length, capacity, and backing store;
//   - maps keep their key set and their identity (values are written back
//     through the existing keys);
//   - arrays and structs are processed in their existing field order;
//   - a non-nil interface keeps its dynamic type: scalars and value structs
//     inside are filled and written back, pointers/maps/slices inside are
//     followed into the original object, and a typed nil pointer inside an
//     interface is left as-is rather than replaced with a new object.
//
// Each writable storage unit is generated at most once per call, so shared
// nodes, map self-references, pointers into slice elements, and partially
// overlapping slices all terminate and observe mutually consistent values.
// The walk is cycle-safe by object identity, not by depth truncation, and it
// never copies the graph.
//
// Determinism: for a given seed and a given logical graph the result is
// reproducible and does not depend on Go's randomized map iteration order.
// Map keys are visited sorted by value. Only maps whose key kind is bool,
// one of the integer kinds, or string (including named types with those
// underlying kinds) are supported; any other reachable map key type is an
// error. Key values themselves are used for ordering and identity, never
// their printed text. A map shared by several fields is traversed once.
//
// SkipFieldsWithPattern and AllowUnexportedFields keep their usual meaning:
// skipped or inaccessible fields are left untouched, consume no randomness,
// and are exempt from the pre-flight checks described below.
//
// Unlike Fuzz, this entry point does NOT invoke any custom Funcs, the
// fuzz.Interface self-fuzzing hook, or the default time.Time handler,
// because such arbitrary callbacks could rebuild the very graph this mode
// promises to preserve. A time.Time reached here is treated as a plain
// struct (its unexported fields are skipped unless AllowUnexportedFields is
// set). NilChance, NumElements, and MaxDepth only serve Fuzz/FuzzNoCustom
// and have no effect on this identity-based mode. Random leaf values are
// drawn from this Fuzzer's own random source (see RandSource), so output
// remains reproducible for a given seed.
//
// Before anything is generated, the reachable region is checked: obj must
// be a non-nil pointer, every reachable map key type must be supported, and
// no reachable chan, func, or unsafe.Pointer may exist. If a check fails,
// an error naming the field or key position is returned and neither the
// graph nor the random source is touched. On success the call runs under
// the same lock as Fuzz. All traversal state is local to a single call;
// objects seen by a previous call are generated again by the next one.
func (f *Fuzzer) FuzzExisting(obj interface{}) error {
	f.fuzzLock.Lock()
	defer f.fuzzLock.Unlock()

	v := reflect.ValueOf(obj)
	if !v.IsValid() || v.Kind() != reflect.Ptr || v.IsNil() {
		return fmt.Errorf("gofuzz: FuzzExisting: need a non-nil pointer, got %T", obj)
	}

	w := &existingWalker{fuzzer: f, visited: map[existingVisit]struct{}{}}
	// Pre-flight check: no randomness is consumed and the graph is not
	// modified, so a failure here leaves both untouched.
	if err := w.walk(v.Elem(), "root", false); err != nil {
		return err
	}
	// Generation pass with a fresh visited set: traversal state never
	// leaks between calls.
	w.visited = map[existingVisit]struct{}{}
	return w.walk(v.Elem(), "root", true)
}

// existingVisit identifies one writable storage unit by type and address
// (for maps, by type and map identity) within a single FuzzExisting call.
type existingVisit struct {
	typ reflect.Type
	ptr uintptr
}

// existingWalker carries the per-call traversal state of FuzzExisting.
type existingWalker struct {
	fuzzer  *Fuzzer
	visited map[existingVisit]struct{}
}

// mark reports whether the storage unit identified by (t, ptr) is seen for
// the first time during this call.
func (w *existingWalker) mark(t reflect.Type, ptr uintptr) bool {
	k := existingVisit{typ: t, ptr: ptr}
	if _, ok := w.visited[k]; ok {
		return false
	}
	w.visited[k] = struct{}{}
	return true
}

// markValue marks an addressable value's own storage. Non-addressable
// values (which cannot form cycles by themselves) are always let through.
func (w *existingWalker) markValue(v reflect.Value) bool {
	if !v.CanAddr() {
		return true
	}
	return w.mark(v.Type(), v.UnsafeAddr())
}

// walk traverses v. When gen is false it only validates the reachable
// region; when gen is true it fills leaf values in place.
func (w *existingWalker) walk(v reflect.Value, path string, gen bool) error {
	switch v.Kind() {
	case reflect.Ptr:
		if v.IsNil() {
			// Nil pointers stay nil; they are never replaced.
			return nil
		}
		// The pointer slot itself is never written; the pointee is the
		// storage unit and is marked when it is visited.
		return w.walk(v.Elem(), path, gen)

	case reflect.Bool,
		reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64,
		reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr,
		reflect.Float32, reflect.Float64,
		reflect.Complex64, reflect.Complex128,
		reflect.String:
		if !w.markValue(v) {
			return nil
		}
		if gen {
			fillFuncMap[v.Kind()](v, w.fuzzer.r)
		}
		return nil

	case reflect.Struct:
		if !w.markValue(v) {
			return nil
		}
		t := v.Type()
		for i := 0; i < v.NumField(); i++ {
			sf := t.Field(i)
			skip := false
			for _, pattern := range w.fuzzer.skipFieldPatterns {
				if pattern.MatchString(sf.Name) {
					skip = true
					break
				}
			}
			if skip {
				continue
			}
			fv := v.Field(i)
			if !fv.CanSet() {
				if !w.fuzzer.allowUnexportedFields || !fv.CanAddr() {
					continue
				}
				fv = reflect.NewAt(fv.Type(), unsafe.Pointer(fv.UnsafeAddr())).Elem()
			}
			if err := w.walk(fv, path+"."+sf.Name, gen); err != nil {
				return err
			}
		}
		return nil

	case reflect.Array:
		if !w.markValue(v) {
			return nil
		}
		for i := 0; i < v.Len(); i++ {
			if err := w.walk(v.Index(i), fmt.Sprintf("%s[%d]", path, i), gen); err != nil {
				return err
			}
		}
		return nil

	case reflect.Slice:
		if v.IsNil() {
			return nil
		}
		// The slice header is never written; length, capacity, and backing
		// store are preserved. Elements are marked individually, so
		// overlapping slices share their common elements exactly once.
		for i := 0; i < v.Len(); i++ {
			if err := w.walk(v.Index(i), fmt.Sprintf("%s[%d]", path, i), gen); err != nil {
				return err
			}
		}
		return nil

	case reflect.Map:
		if v.IsNil() {
			return nil
		}
		if !w.mark(v.Type(), v.Pointer()) {
			return nil
		}
		keyType := v.Type().Key()
		if !existingSupportedMapKey(keyType.Kind()) {
			return fmt.Errorf("gofuzz: FuzzExisting: unsupported map key type %s at %s", keyType, path)
		}
		keys := v.MapKeys()
		sortExistingKeys(keys)
		for _, k := range keys {
			keyPath := fmt.Sprintf("%s[%v]", path, k)
			// Map values are not addressable; fill an addressable copy
			// and write it back through the existing key.
			val := reflect.New(v.Type().Elem()).Elem()
			val.Set(v.MapIndex(k))
			if err := w.walk(val, keyPath, gen); err != nil {
				return err
			}
			if gen {
				v.SetMapIndex(k, val)
			}
		}
		return nil

	case reflect.Interface:
		if v.IsNil() {
			return nil
		}
		dv := v.Elem()
		switch dv.Kind() {
		case reflect.Ptr, reflect.Map, reflect.Slice:
			// Keep walking the original object; a typed nil pointer is
			// left as-is by the Ptr case above.
			return w.walk(dv, path, gen)
		default:
			// Scalars, value structs, and arrays stored in an interface
			// are copied out, filled, and written back into the same
			// interface slot, preserving the dynamic type.
			cp := reflect.New(dv.Type()).Elem()
			cp.Set(dv)
			if err := w.walk(cp, path, gen); err != nil {
				return err
			}
			if gen {
				v.Set(cp)
			}
			return nil
		}

	default:
		return fmt.Errorf("gofuzz: FuzzExisting: unsupported kind %s at %s", v.Kind(), path)
	}
}

// existingSupportedMapKey reports whether map keys of this kind can be
// sorted deterministically by value.
func existingSupportedMapKey(k reflect.Kind) bool {
	switch k {
	case reflect.Bool,
		reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64,
		reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr,
		reflect.String:
		return true
	}
	return false
}

// sortExistingKeys orders map keys by their values so that traversal does
// not depend on Go's randomized map iteration order. Keys within one map
// share a single kind, so a single comparison per kind is enough.
func sortExistingKeys(keys []reflect.Value) {
	sort.Slice(keys, func(i, j int) bool {
		a, b := keys[i], keys[j]
		switch a.Kind() {
		case reflect.Bool:
			return !a.Bool() && b.Bool()
		case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
			return a.Int() < b.Int()
		case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr:
			return a.Uint() < b.Uint()
		default:
			return a.String() < b.String()
		}
	})
}
