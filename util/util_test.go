package util

import (
	"github.com/headzoo/surf/internal/testassert"
	"testing"
)

func TestFileExists(t *testing.T) {

	ex := FileExists("./util_test.go")
	testassert.True(t, ex)

	ex = FileExists("./util.txt")
	testassert.False(t, ex)
}
