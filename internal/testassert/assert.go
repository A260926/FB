// SPDX-License-Identifier: MIT
// Copyright (c) 2026 testassert contributors

// Package testassert supplies the assertions used by Surf's historical tests.
// It is local test infrastructure, with no dependency on an external test library.
package testassert

import (
	"reflect"
	"strings"
)

// Reporter is satisfied by testing.T. Errorf records a failure and continues.
type Reporter interface {
	Helper()
	Errorf(string, ...any)
}

func result(t Reporter, ok bool, operation string, operands ...any) bool {
	t.Helper()
	if !ok {
		t.Errorf("%s assertion failed; operands: %#v", operation, operands)
	}
	return ok
}

func Equal(t Reporter, expected, actual any) bool {
	t.Helper()
	return result(t, reflect.DeepEqual(expected, actual), "equal", expected, actual)
}

// Nil deliberately tests the interface itself, matching the existing call sites.
// A non-nil interface containing a nil pointer does not satisfy this assertion.
func Nil(t Reporter, value any) bool {
	t.Helper()
	return result(t, value == nil, "nil", value)
}

func NotNil(t Reporter, value any) bool {
	t.Helper()
	return result(t, value != nil, "not nil", value)
}

func True(t Reporter, value bool) bool {
	t.Helper()
	return result(t, value, "true", value)
}

func False(t Reporter, value bool) bool {
	t.Helper()
	return result(t, !value, "false", value)
}

func Greater(t Reporter, lower, actual int) bool {
	t.Helper()
	return result(t, actual > lower, "greater", lower, actual)
}

// Contains takes the expected substring before the actual string.
func Contains(t Reporter, substring, actual string) bool {
	t.Helper()
	return result(t, strings.Contains(actual, substring), "contains", substring, actual)
}
