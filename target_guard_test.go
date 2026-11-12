package rdf
import("os";"testing";"sync";"fmt")
var targetMu sync.Mutex
var targetCounts=map[string]int{}
func targetCheck(t *testing.T, ok bool){t.Helper();targetMu.Lock();targetCounts[t.Name()]++;targetMu.Unlock();if !ok {t.Fatal("targetCheck false")}}
func TestMain(m *testing.M){code:=m.Run();if os.Getenv("CHECK_TARGET")=="1" {for _,n:=range []string{"TestTargetQuotedTerms","TestTargetAnnotations","TestTargetFormats","TestTargetInvalidRoles"}{if targetCounts[n]==0 {fmt.Fprintln(os.Stderr,"MISSING_ASSERTIONS",n);code=1}}};os.Exit(code)}
