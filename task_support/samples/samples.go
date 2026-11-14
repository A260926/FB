// Package samples builds inert WebAssembly format examples using only Go's standard library.
package samples
import "encoding/binary"
func U(x uint64) []byte { b:=make([]byte,10);n:=binary.PutUvarint(b,x);return b[:n] }
func Section(id byte,payload []byte) []byte { b:=[]byte{id};b=append(b,U(uint64(len(payload)))...);return append(b,payload...) }
// Image describes a single () -> () function. EH adds a declared () -> () tag
// and a try_table that catches a throw by branching out of its enclosing block.
func Image(eh bool) []byte {
 b:=[]byte{0,97,115,109,1,0,0,0}
 b=append(b,Section(1,[]byte{1,0x60,0,0})...)
 b=append(b,Section(3,[]byte{1,0})...)
 if eh { b=append(b,Section(13,[]byte{1,0,0})...) }
 body:=[]byte{0,1,0x0b} // locals, nop, function end
 if eh {body=[]byte{0,2,0x40,0x1f,0x40,1,0,0,0,8,0,0x0b,0x0b,1,0x0b}}
 code:=append([]byte{1},U(uint64(len(body)))...);code=append(code,body...)
 b=append(b,Section(10,code)...)
 return b
}
