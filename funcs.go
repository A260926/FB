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
	"reflect"
)

// ErrCustomFunction is the sentinel error wrapped when a registered
// custom generator returns an error. The handler's own error is
// wrapped alongside it, so callers can recognize either with
// errors.Is.
var ErrCustomFunction = errors.New("custom function returned an error")

type Continue struct {
	F *ConsumeFuzzer
}

// errorInterface is the reflect.Type of the error interface, used to
// validate a custom generator's return signature at registration time.
var errorInterface = reflect.TypeOf((*error)(nil)).Elem()

// continueType is the exact type accepted as the second argument of a
// custom generator.
var continueType = reflect.TypeOf(Continue{})

func (f *ConsumeFuzzer) AddFuncs(fuzzFuncs []interface{}) {
	for i := range fuzzFuncs {
		v := reflect.ValueOf(fuzzFuncs[i])
		if v.Kind() != reflect.Func {
			panic("Need only funcs!")
		}
		// A typed nil function would panic in reflect.Call (or
		// reflect.IsNil) later, so reject it while registering.
		if v.IsNil() {
			panic("fuzzFunc must not be nil")
		}
		t := v.Type()
		if t.NumIn() != 2 || t.NumOut() != 1 {
			panic("Need 2 in and 1 out params. In must be the type. Out must be an error")
		}
		argT := t.In(0)
		switch argT.Kind() {
		case reflect.Ptr, reflect.Map:
		default:
			panic("fuzzFunc must take pointer or map type")
		}
		if t.In(1) != continueType {
			panic("fuzzFunc's second parameter must be type Continue")
		}
		if !t.Out(0).Implements(errorInterface) {
			panic("fuzzFunc's return value must implement the error interface")
		}
		f.Funcs[argT] = v
	}
}

func (f *ConsumeFuzzer) GenerateWithCustom(targetStruct interface{}) error {
	e := reflect.ValueOf(targetStruct).Elem()
	return f.fuzzStruct(e, true)
}

// GenerateStruct runs the default generation over the whole passed
// subtree, ignoring any registered custom generators.
func (c Continue) GenerateStruct(targetStruct interface{}) error {
	return c.F.GenerateStruct(targetStruct)
}

// GenerateStructWithCustom keeps using the registered generators for
// the passed subtree. It reuses the parent's consumer, so sibling
// fields after a nested call continue reading at the next byte.
func (c Continue) GenerateStructWithCustom(targetStruct interface{}) error {
	return c.F.GenerateWithCustom(targetStruct)
}
