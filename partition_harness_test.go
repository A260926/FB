package cookiejar
import("testing";"sync")
type partitionCase struct{Name,Group string; Run func(*testing.T)}
var partitionCounts sync.Map
func partitionCheck(t *testing.T,ok bool){t.Helper();if !ok{t.Fatal("partition content assertion failed")};p,_:=partitionCounts.LoadOrStore(t,new(int));*p.(*int)++}
func TestPartitionContract(t *testing.T){
 cases:=partitionCases();if len(cases)<6{t.Fatal("need at least six cases")};names:=map[string]bool{};groups:=map[string]bool{}
 for _,c:=range cases{if c.Name==""||names[c.Name]||c.Run==nil{t.Fatal("invalid case")};names[c.Name]=true;groups[c.Group]=true
  t.Run(c.Group+"/"+c.Name,func(t *testing.T){partitionCounts.Store(t,new(int));t.Cleanup(func(){p,_:=partitionCounts.LoadAndDelete(t);if t.Skipped()||*p.(*int)==0{t.Error("skipped or empty case")}});c.Run(t)})
 }
 for _,g:=range []string{"identity","persistence","legacy"}{if !groups[g]{t.Fatal("missing group",g)}}
 t.Logf("PARTITION_COMPLETE cases=%d",len(cases))
}
