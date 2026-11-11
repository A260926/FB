package h264
import("fmt";"strings";"testing")
func TestSeedSPSObservation(t *testing.T){
 first:=casesDTSExtractor[0].sequence[0]
 b:=EmulationPreventionRemove(first.au[0][1:]); var s strings.Builder
 for _,v:=range b{s.WriteString(fmt.Sprintf("%08b",v))}
 bits:=s.String(); bits=bits[:24]+"010"+bits[25:]
 for len(bits)%8!=0{bits+="0"}
 out:=make([]byte,len(bits)/8)
 for i:=range out{for j:=0;j<8;j++{out[i]=out[i]*2+bits[i*8+j]-'0'}}
 other:=[]byte{0x67}; zeros:=0
 for _,v:=range out{if zeros==2 && v<=3{other=append(other,3);zeros=0};other=append(other,v);if v==0{zeros++}else{zeros=0}}
 var check SPS;if err:=check.Unmarshal(other);err!=nil || check.ID!=1{t.Fatalf("bad alternate SPS: %v id %d",err,check.ID)}
 d:=DTSExtractor{}; d.Initialize()
 pps:=[]byte{0x68,0xee,0x3c,0x80}
 if _,err:=d.Extract([][]byte{first.au[0],pps,first.au[1]},first.pts);err!=nil{t.Fatal(err)}
 next:=casesDTSExtractor[0].sequence[1]
 got,err:=d.Extract([][]byte{other,next.au[0]},next.pts)
 t.Logf("unused SPS1 observation: DTS=%d err=%v want=%d",got,err,next.dts)
}
