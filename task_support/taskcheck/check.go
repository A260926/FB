// Package taskcheck records executed assertions and normal test completion.
package taskcheck

import (
	"sync/atomic"
	"testing"
)

type C struct {
	t *testing.T
	checks uint64
}

func (c *C) Check(ok bool, message string, args ...interface{}) {
	c.t.Helper()
	atomic.AddUint64(&c.checks, 1)
	if !ok { c.t.Errorf(message, args...) }
}

func Run(t *testing.T, body func(*C)) {
	t.Helper()
	c := &C{t: t}
	returned := false
	t.Cleanup(func() {
		if !returned { t.Error("task body did not return normally") }
		n := atomic.LoadUint64(&c.checks)
		if n == 0 { t.Error("task group executed no content checks") }
		if returned && n > 0 && !t.Failed() && !t.Skipped() {
			t.Logf("TASK_GROUP_DONE %s checks=%d", t.Name(), n)
		}
	})
	body(c)
	returned = true
}
