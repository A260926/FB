package main
import (
 "bytes"
 "fmt"
 "os"
 "path/filepath"
 "strings"
 "github.com/loopholelabs/wasm-toolkit/pkg/wasm/expression"
 "github.com/loopholelabs/wasm-toolkit/pkg/wasm/wasmfile"
 "github.com/loopholelabs/wasm-toolkit/task_support/samples"
)
func must(ok bool,message string){if !ok {panic(message)}}
func main(){
 if len(os.Args)==3 && os.Args[1]=="generate"{
  must(os.MkdirAll(os.Args[2],0755)==nil,"mkdir")
  must(os.WriteFile(filepath.Join(os.Args[2],"ordinary.wasm"),samples.Image(false),0644)==nil,"write ordinary")
  must(os.WriteFile(filepath.Join(os.Args[2],"exception.wasm"),samples.Image(true),0644)==nil,"write EH")
  fmt.Println("FIXTURES_WRITTEN");return
 }
 for _,name:=range []string{"ordinary.wasm","exception.wasm"}{
  d,err:=os.ReadFile(filepath.Join("task_support/fixtures",name));must(err==nil,"read fixture")
  must(bytes.Equal(d,samples.Image(name=="exception.wasm")),"fixture drift "+name)
 }
 wf:=wasmfile.NewEmpty();err:=wf.DecodeBinary(samples.Image(false));must(err==nil,fmt.Sprint(err))
 must(len(wf.Code)==1 && len(wf.Code[0].Expression)==1,"normal expression count")
 e:=wf.Code[0].Expression[0];must(e.Opcode==1 && e.PCNext==e.PC+1,"normal PC span")
 must(wf.FindFunction(e.PC)==0,"normal function mapping")
 fmt.Printf("ORDINARY_WASM_OK functions=%d opcode=%d pc=%d next=%d\n",len(wf.Code),e.Opcode,e.PC,e.PCNext)
 if len(os.Args)>1 && os.Args[1]=="ordinary" {return}
 eh:=wasmfile.NewEmpty();err=eh.DecodeBinary(samples.Image(true))
 must(err!=nil && strings.Contains(err.Error(),"Unknown section 13"),fmt.Sprint("unexpected EH status: ",err))
 fmt.Println("EH_SECTION_UNSUPPORTED:",err)
 // A syntactically complete standalone try_table expression, empty handlers.
 _,_,err=expression.NewExpression([]byte{0x1f,0x40,0,0x0b,0x0b},100)
 must(err!=nil && strings.Contains(err.Error(),"Unsupported opcode 31"),fmt.Sprint("unexpected instruction status: ",err))
 fmt.Println("TRY_TABLE_UNSUPPORTED:",err)
}
