package unitcheck
import "testing"
type C struct { count int }
func (c *C) Check(t *testing.T, ok bool, format string, args ...interface{}) {
 t.Helper(); c.count++; if !ok { t.Errorf(format,args...) }
}
func Run(t *testing.T, body func(*C)) {
 t.Helper(); c:=new(C); body(c)
 if c.count==0 { t.Fatal("no content assertions executed") }
 t.Logf("UNIT_DONE %s %d",t.Name(),c.count)
}
