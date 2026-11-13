package unitcheck

import "testing"

// A Counter belongs to one named subtest. Call assertions on its test goroutine.
type Counter struct {
	t *testing.T
	n int
}

func New(t *testing.T) *Counter {
	t.Helper()
	c := &Counter{t: t}
	t.Cleanup(func() {
		if c.n == 0 {
			t.Error("no assertions executed")
		}
		t.Logf("UNIT_ASSERTIONS %d", c.n)
	})
	return c
}
func (c *Counter) Check(ok bool, message string, args ...interface{}) {
	c.t.Helper()
	c.n++
	if !ok {
		c.t.Fatalf(message, args...)
	}
}
