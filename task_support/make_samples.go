// Deterministic, non-executable ELF objects containing synthetic DWARF sections.
package main
import("encoding/binary";"os";"path/filepath")
var le=binary.LittleEndian
func u32(v uint32)[]byte{b:=make([]byte,4);le.PutUint32(b,v);return b}
func unit(body []byte)[]byte{h:=[]byte{5,0,1,8,0,0,0,0};h=append(h,body...);return append(u32(uint32(len(h))),h...)}
func elf(sections [][]byte,names []string)[]byte{
 strings:=[]byte{0};offsets:=[]uint32{0}
 for _,n:=range names{offsets=append(offsets,uint32(len(strings)));strings=append(strings,[]byte(n)...);strings=append(strings,0)}
 sections=append([][]byte{strings},sections...)
 out:=make([]byte,64);copy(out,[]byte{0x7f,'E','L','F',2,1,1});le.PutUint16(out[16:],1);le.PutUint16(out[18:],62);le.PutUint32(out[20:],1);le.PutUint16(out[52:],64);le.PutUint16(out[58:],64);le.PutUint16(out[60:],uint16(len(sections)+1));le.PutUint16(out[62:],1)
 positions:=[]int{}
 for _,s:=range sections{positions=append(positions,len(out));out=append(out,s...)}
 for len(out)%8!=0{out=append(out,0)};le.PutUint64(out[40:],uint64(len(out)));out=append(out,make([]byte,64)...)
 for i,s:=range sections{h:=make([]byte,64);le.PutUint32(h,offsets[i+1]);kind:=uint32(1);if i==0{kind=3};le.PutUint32(h[4:],kind);le.PutUint64(h[24:],uint64(positions[i]));le.PutUint64(h[32:],uint64(len(s)));le.PutUint64(h[48:],1);out=append(out,h...)}
 return out
}
func main(){
 abbrev:=[]byte{1,0x11,1,3,8,0,0,2,0x24,0,3,8,0x3e,0x0b,0x0b,0x0b,0,0,3,0x16,0,3,0x1d,0x49,0x1c,0,0,4,0x13,1,3,0x0e,0x0b,0x0b,0,0,5,0x0d,0,3,8,0x49,0x13,0x38,0x0b,0,0,6,0x0f,0,0x49,0x13,0,0,0}
 mainInfo:=unit([]byte{1,'m',0,2,'l','o','c','a','l',0,5,4,3,0,0,0,0,15,0,0,0,0})
 supInfo:=unit([]byte{1,'s',0,4,0,0,0,0,8,5,'n','e','x','t',0,33,0,0,0,0,0,6,15,0,0,0,0})
 mainID:=append([]byte{5,0,0},[]byte("supplement.elf\x00")...);mainID=append(mainID,4,1,0x23,0x45,0x67)
 supID:=[]byte{5,0,1,0,4,1,0x23,0x45,0x67}
 names:=[]string{".shstrtab",".debug_abbrev",".debug_info",".debug_str",".debug_sup"}
 out:="task_support/samples";if e:=os.MkdirAll(out,0755);e!=nil{panic(e)}
 for n,sections:=range map[string][][]byte{"main.elf":{abbrev,mainInfo,[]byte("local\x00"),mainID},"supplement.elf":{abbrev,supInfo,[]byte("Node\x00"),supID}}{if e:=os.WriteFile(filepath.Join(out,n),elf(sections,names),0644);e!=nil{panic(e)}}
}
