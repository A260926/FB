package benign
import "testing"
func FuzzEcho(f *testing.F) {
 f.Add("hello")
 f.Fuzz(func(t *testing.T, s string) { b:=[]byte(s); if string(b)!=s { t.Fatal("roundtrip") } })
}
