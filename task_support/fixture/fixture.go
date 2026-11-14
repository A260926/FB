// Package fixture constructs ordinary synthetic modules; it performs no linking.
package fixture

import (
 "github.com/tetratelabs/wabin/wasm"
 "github.com/tetratelabs/wabin/leb128"
)
func Const(x int32) *wasm.ConstantExpression { return &wasm.ConstantExpression{Opcode:wasm.OpcodeI32Const,Data:leb128.EncodeInt32(x)} }
func Provider() *wasm.Module {
 max:=uint32(8)
 return &wasm.Module{
  TypeSection:[]*wasm.FunctionType{{}},
  FunctionSection:[]wasm.Index{0}, CodeSection:[]*wasm.Code{{Body:[]byte{wasm.OpcodeEnd}}},
  MemorySection:&wasm.Memory{Min:1,Max:2,IsMaxEncoded:true},
  TableSection:[]*wasm.Table{{Min:4,Max:&max,Type:wasm.RefTypeFuncref}},
  GlobalSection:[]*wasm.Global{{Type:&wasm.GlobalType{ValType:wasm.ValueTypeI32},Init:Const(16)}},
  ExportSection:[]*wasm.Export{{Name:"mem",Type:wasm.ExternTypeMemory},{Name:"tab",Type:wasm.ExternTypeTable},{Name:"base",Type:wasm.ExternTypeGlobal},{Name:"fn",Type:wasm.ExternTypeFunc}},
 }
}
func Consumer() *wasm.Module {
 index:=wasm.Index(0);max:=uint32(8)
 return &wasm.Module{
  TypeSection:[]*wasm.FunctionType{{}},
  ImportSection:[]*wasm.Import{
   {Type:wasm.ExternTypeMemory,Module:"p",Name:"mem",DescMem:&wasm.Memory{Min:1,Max:2,IsMaxEncoded:true}},
   {Type:wasm.ExternTypeFunc,Module:"p",Name:"fn",DescFunc:0},
   {Type:wasm.ExternTypeGlobal,Module:"p",Name:"base",DescGlobal:&wasm.GlobalType{ValType:wasm.ValueTypeI32}},
   {Type:wasm.ExternTypeTable,Module:"p",Name:"tab",DescTable:&wasm.Table{Min:4,Max:&max,Type:wasm.RefTypeFuncref}},
  },
  ExportSection:[]*wasm.Export{{Name:"shared",Type:wasm.ExternTypeMemory}},
  DataSection:[]*wasm.DataSegment{{OffsetExpression:&wasm.ConstantExpression{Opcode:wasm.OpcodeGlobalGet,Data:[]byte{0}},Init:[]byte("abc")}},
  ElementSection:[]*wasm.ElementSegment{{Mode:wasm.ElementModeActive,Type:wasm.RefTypeFuncref,TableIndex:0,OffsetExpr:Const(1),Init:[]*wasm.Index{&index}}},
 }
}
