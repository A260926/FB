// Verification scaffold; contains only existing production API calls.
package main
import (
 "os"
 "testing"
 . "github.com/dvyukov/go-fuzz/go-fuzz-defs"
)
func authorOwnershipHub() *Hub {
 c:=make([]byte,CoverSize);c[7]=1
 h:=&Hub{corpusSigs:make(map[Sig]struct{}),newInputC:make(chan Input,16),newCrasherC:make(chan NewCrasherArgs,16),syncC:make(chan Stats,16),triageC:make(chan CoordinatorInput,16)}
 h.ro.Store(&ROData{corpus:[]Input{{data:[]byte("A"),cover:c,coverSize:1,execTime:1,score:10,runningScoreSum:10}},corpusCover:makeCopy(c),badInputs:make(map[Sig]struct{}),suppressions:make(map[Sig]struct{})})
 h.maxCover.Store(makeCopy(c));h.corpusSigs[hash([]byte("A"))]=struct{}{}
 return h
}
func authorOwnershipWorker(t *testing.T,h *Hub) *Worker {
 t.Helper();p:=os.Getenv("GOFUZZ_BENIGN_BIN");if p=="" {t.Fatal("missing benign testee")}
 w:=&Worker{hub:h,mutator:newMutator()}
 w.coverBin=newTestBinary(p,func(){},&w.stats,0)
 t.Cleanup(func(){w.coverBin.close()})
 return w
}
func TestBenignBaseline(t *testing.T) {
 h:=authorOwnershipHub();h.updateScores()
 if len(h.ro.Load().(*ROData).corpus)!=1 {t.Fatal("corpus update")}
 w:=authorOwnershipWorker(t,h)
 w.testInput([]byte("ABC"),1,execFuzz)
 w.triageInput(CoordinatorInput{Data:[]byte("A"),Prio:1,Type:execCorpus,Minimized:true,Smashed:true})
 inp:=<-h.newInputC
 if string(inp.data)!="A" || inp.cover[7]!=1 || inp.cover[11]!=1 || inp.cover[13]!=0 || inp.res!=0 {t.Fatal("real triage result mismatch")}
 if w.execs[execTriageInput]!=3 || w.execs[execFuzz]!=1 || w.stats.execs!=4 {t.Fatal("production call counts")}
 t.Log("BENIGN_WORKER_TESTBINARY_TRIAGE_OK execs=4 triage=3 slots=7,11")
}
