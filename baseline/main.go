package main
import (
 "fmt"
 fuzz "github.com/google/gofuzz"
)
type leaf struct { Value uint64 }
type graph struct { Left, Right *leaf; Values []uint16 }
func main() {
 old := &leaf{Value: 99}
 g := graph{Left: old, Right: old, Values: []uint16{1,2,3}}
 fuzz.NewWithSeed(19).NilChance(0).NumElements(2,2).Fuzz(&g)
 if g.Left == g.Right || g.Left == old || g.Right == old || old.Value != 99 || len(g.Values)!=2 { panic("unexpected rebuilding behavior") }
 fmt.Println("GOFUZZ_REBUILD_BASELINE_OK")
 var a,b graph
 input:=[]byte("local-benign-input-012345678901234567890123456789")
 fuzz.NewFromGoFuzz(input).NilChance(0).NumElements(2,2).Fuzz(&a)
 fuzz.NewFromGoFuzz(input).NilChance(0).NumElements(2,2).Fuzz(&b)
 if a.Left.Value!=b.Left.Value || a.Right.Value!=b.Right.Value || fmt.Sprint(a.Values)!=fmt.Sprint(b.Values) {panic("determinism")}
 fmt.Println("GOFUZZ_BYTESOURCE_REPLAY_OK")
}
