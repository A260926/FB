package mpeg2
import "testing"
type cbrChecks struct { t *testing.T; n int }
func(c *cbrChecks) Check(ok bool, message string) { c.n++; if !ok { c.t.Fatal(message) } }
func cbrGroup(t *testing.T, f func(*cbrChecks)) { c:=&cbrChecks{t:t}; f(c); if c.n==0 {t.Fatal("zero checks")}; t.Logf("checks=%d",c.n) }
func TestCBRClock(t *testing.T){cbrGroup(t,caseCBRClock)}
func TestCBRTables(t *testing.T){cbrGroup(t,caseCBRTables)}
func TestCBRMedia(t *testing.T){cbrGroup(t,caseCBRMedia)}
func TestCBRCompatibility(t *testing.T){cbrGroup(t,caseCBRCompatibility)}
