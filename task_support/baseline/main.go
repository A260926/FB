package main
import (
 "fmt"
 "bytes"
 "github.com/tetratelabs/wabin/binary"
 "github.com/tetratelabs/wabin/wasm"
 "github.com/tetratelabs/wabin/task_support/fixture"
)
func main(){
 modules:=[]*wasm.Module{fixture.Provider(),fixture.Consumer()}
 for i,m:=range modules{
  raw:=binary.EncodeModule(m);got,err:=binary.DecodeModule(raw,wasm.CoreFeaturesV2);if err!=nil{panic(err)}
  if !bytes.Equal(raw,binary.EncodeModule(got)){panic("roundtrip differs")}
  fmt.Printf("MODULE %d exports=%d imports=%d data=%d elements=%d\n",i,len(got.ExportSection),len(got.ImportSection),len(got.DataSection),len(got.ElementSection))
  if i==1{
   if got.ImportMemoryCount()!=1||got.ImportFuncCount()!=1||got.ImportGlobalCount()!=1||got.ImportTableCount()!=1{panic("kind count")}
   if got.DataSection[0].OffsetExpression.Opcode!=wasm.OpcodeGlobalGet||string(got.DataSection[0].Init)!="abc"||*got.ElementSection[0].Init[0]!=0{panic("decoded segments")}
   fmt.Println("MIXED_KIND_COUNTS=1,1,1,1 DATA_OFFSET=global.get:0 ELEMENT_FUNCTION=0")
  }
 }
 fmt.Println("DECODE_ROUNDTRIP_OK modules=2")
}
