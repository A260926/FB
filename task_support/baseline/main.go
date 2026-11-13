package main
import("bytes";"fmt";"os";"github.com/tetratelabs/wabin/binary";"github.com/tetratelabs/wabin/wasm")
func main(){
 body:=[]byte{wasm.OpcodeBlock,wasm.ValueTypeI32,wasm.OpcodeI32Const,7,wasm.OpcodeBr,0,wasm.OpcodeEnd,wasm.OpcodeEnd}
 m:=&wasm.Module{TypeSection:[]*wasm.FunctionType{{Results:[]wasm.ValueType{wasm.ValueTypeI32}}},FunctionSection:[]wasm.Index{0},CodeSection:[]*wasm.Code{{Body:body}}}
 wire:=binary.EncodeModule(m);got,e:=binary.DecodeModule(wire,wasm.CoreFeaturesV2)
 if e!=nil||len(got.CodeSection)!=1||!bytes.Equal(got.CodeSection[0].Body,body){fmt.Println(e);os.Exit(1)}
 fmt.Printf("WABIN_BODY bytes=%x locals=%d functions=%d\n",got.CodeSection[0].Body,len(got.CodeSection[0].LocalTypes),len(got.FunctionSection))
 if !bytes.Equal(binary.EncodeModule(got),wire){os.Exit(2)}
 fmt.Println("BASELINE_OK real_binary_encode_decode")
}
