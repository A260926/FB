package fuzz
import("testing";"bytes")
func TestTaskAPIConsumer(t *testing.T){
 p:=ReplayPlan{BuildID:"normal-build",CoverageSize:2,Stages:[][]CorpusEntry{{{Values:[]any{[]byte("normal")}}}}}
 r,e:=NewReplaySession(p);if e!=nil{t.Fatal(e)}
 l,ok,e:=r.Next();if e!=nil||!ok||l.Sequence!=1||l.Stage!=0{t.Fatal("dispatch",l,ok,e)}
 accepted,e:=r.Accept(l,[]byte{1,0});if e!=nil||!accepted{t.Fatal("accept",e)}
 st:=r.State();if st.Committed!=1||st.Pending!=0||st.CorpusEntries!=1||!bytes.Equal(st.Coverage,[]byte{1,0}){t.Fatal("state",st)}
 b,e:=r.Snapshot();if e!=nil||len(b)==0{t.Fatal("snapshot",e)}
 restored,e:=RestoreReplaySession(p,b);if e!=nil{t.Fatal(e)}
 if restored.State().Committed!=1{t.Fatal("restore count")}
 _,ok,e=restored.Next();if e!=nil||ok{t.Fatal("completed restore",ok,e)}
}
