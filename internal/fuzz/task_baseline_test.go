package fuzz
import("testing";"reflect";"io";"fmt";"bytes")
func TestProductionBaseline(t *testing.T){
 e:=CorpusEntry{Values:[]any{[]byte("normal")},IsSeed:true}
 c,err:=newCoordinator(CoordinateFuzzingOpts{Log:io.Discard,Parallel:1,Limit:4,Seed:[]CorpusEntry{e},Types:[]reflect.Type{reflect.TypeOf([]byte{})}})
 if err!=nil {t.Fatal(err)}
 in,ok:=c.peekInput();if !ok||!in.warmup {t.Fatal("no seed warmup")};c.sentInput(in)
 if c.countWaiting!=1||c.inputQueue.len!=0 {t.Fatal("dispatch accounting")}
 vals,err:=unmarshalCorpusFile(in.entry.Data);if err!=nil||!bytes.Equal(vals[0].([]byte),[]byte("normal")){t.Fatal("corpus decoding",err)}
 c.coverageMask=[]byte{0,0};if c.updateCoverage([]byte{1,4})!=2||c.updateCoverage([]byte{1,4})!=0{t.Fatal("bucket feedback")}
 c.updateStats(fuzzResult{count:1,limit:1});if c.count!=1||c.countWaiting!=0{t.Fatal("result accounting")}
 fmt.Printf("BASELINE_CORPUS_DISPATCH_FEEDBACK count=%d pending=%d coverage=%v\n",c.count,c.countWaiting,c.coverageMask)
}
