package main
import (
 "bytes"
 "fmt"
 "github.com/tetratelabs/wabin/binary"
 "github.com/tetratelabs/wabin/wasm"
)
func main() {
 for mode:=0;mode<2;mode++ {
  target:=wasm.Index(2)
  writer:=[]byte{wasm.OpcodeEnd}
  if mode==1 {writer=[]byte{wasm.OpcodeI32Const,0,wasm.OpcodeRefNull,wasm.RefTypeFuncref,wasm.OpcodeTableSet,0,wasm.OpcodeEnd}}
  root:=[]byte{wasm.OpcodeI32Const,0,wasm.OpcodeCallIndirect,0,0,wasm.OpcodeEnd}
  m:=&wasm.Module{TypeSection:[]*wasm.FunctionType{{}},ImportSection:[]*wasm.Import{{Type:wasm.ExternTypeFunc,Module:"local",Name:"benign",DescFunc:0}},FunctionSection:[]wasm.Index{0,0,0},TableSection:[]*wasm.Table{{Type:wasm.RefTypeFuncref,Min:1}},ElementSection:[]*wasm.ElementSegment{{Type:wasm.RefTypeFuncref,Mode:wasm.ElementModeActive,TableIndex:0,OffsetExpr:&wasm.ConstantExpression{Opcode:wasm.OpcodeI32Const,Data:[]byte{0}},Init:[]*wasm.Index{&target}}},CodeSection:[]*wasm.Code{{Body:root},{Body:writer},{Body:[]byte{wasm.OpcodeEnd}}}}
  wire:=binary.EncodeModule(m)
  got,err:=binary.DecodeModule(wire,wasm.CoreFeaturesV2);if err!=nil {panic(err)}
  if len(got.ImportSection)!=1 || *got.ElementSection[0].Init[0]!=2 || !bytes.Equal(got.CodeSection[1].Body,writer){panic("module mismatch")}
  if !bytes.Equal(wire,binary.EncodeModule(got)){panic("wire mismatch")}
  fmt.Printf("WABIN_TABLE_%d_ROUNDTRIP_PASS\n",mode)
 }
 fmt.Println("WABIN_BASELINE_OK")
}
