// SPDX-License-Identifier: MIT
// Copyright (c) 2026 testassert contributors
package testassert

import "testing"

type recorder struct{ errors int }

func (*recorder) Helper()                 {}
func (r *recorder) Errorf(string, ...any) { r.errors++ }

func TestAssertionPredicates(t *testing.T) {
	var pointer *int
	cases := []struct {
		name    string
		yes, no func(Reporter) bool
	}{
		{"equal", func(r Reporter) bool { return Equal(r, []int{2, 3}, []int{2, 3}) }, func(r Reporter) bool { return Equal(r, []int{2, 3}, []int{3, 2}) }},
		{"type-sensitive", func(r Reporter) bool { return Equal(r, 2, 2) }, func(r Reporter) bool { return Equal(r, 2, int64(2)) }},
		{"nil", func(r Reporter) bool { return Nil(r, nil) }, func(r Reporter) bool { return Nil(r, pointer) }},
		{"not-nil", func(r Reporter) bool { return NotNil(r, pointer) }, func(r Reporter) bool { return NotNil(r, nil) }},
		{"true", func(r Reporter) bool { return True(r, true) }, func(r Reporter) bool { return True(r, false) }},
		{"false", func(r Reporter) bool { return False(r, false) }, func(r Reporter) bool { return False(r, true) }},
		{"greater-equal", func(r Reporter) bool { return Greater(r, 0, 1) }, func(r Reporter) bool { return Greater(r, 1, 1) }},
		{"greater-less", func(r Reporter) bool { return Greater(r, -2, -1) }, func(r Reporter) bool { return Greater(r, 1, 0) }},
		{"contains", func(r Reporter) bool { return Contains(r, "field=one", "field=one&other=two") }, func(r Reporter) bool { return Contains(r, "field=one&other=two", "field=one") }},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			r := new(recorder)
			if !c.yes(r) || r.errors != 0 {
				t.Fatal("valid assertion rejected", r.errors)
			}
			if c.no(r) || r.errors != 1 {
				t.Fatal("invalid assertion accepted", r.errors)
			}
			if !c.yes(r) || r.errors != 1 {
				t.Fatal("failed assertion prevented later assertion", r.errors)
			}
		})
	}
}
