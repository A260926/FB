package main
import("bytes";"encoding/binary";"fmt";"os"; e "github.com/yalue/elf_reader")
func main(){
 raw:=make([]byte,512);h:=e.ELF64Header{Signature:0x464c457f,Class:2,Endianness:1,Version:1,Version2:1,Type:3,Machine:62,HeaderSize:64,ProgramHeaderOffset:64,ProgramHeaderEntrySize:56,ProgramHeaderEntries:2}
 put:=func(off int,v interface{}){var b bytes.Buffer;if err:=binary.Write(&b,binary.LittleEndian,v);err!=nil{panic(err)};copy(raw[off:],b.Bytes())}
 put(0,h);put(64,e.ELF64ProgramHeader{Type:1,Flags:4,FileOffset:0,VirtualAddress:0x400000,FileSize:512,MemorySize:512,Align:4096});put(120,e.ELF64ProgramHeader{Type:2,Flags:4,FileOffset:256,VirtualAddress:0x400100,FileSize:48,MemorySize:48,Align:8})
 put(256,[]e.ELF64DynamicEntry{{Tag:5,Value:0x400180},{Tag:10,Value:4},{Tag:0}});copy(raw[384:],[]byte{0,'o','k',0})
 f,err:=e.ParseELF64File(raw);if err!=nil{panic(err)};b,err:=f.GetSegmentContent(1);if err!=nil||len(b)!=48{panic("segment parse")};_,err=f.GetDynamicTable(0);if err==nil{panic("unexpected section")}
 fmt.Printf("SECTIONLESS_ELF_OK segments=%d dynamic_bytes=%d sections=%d\n",len(f.Segments),len(b),len(f.Sections));fmt.Println("SECTION_API_UNAVAILABLE:",err)
 if len(os.Args)>1&&os.Args[1]=="write"{if err:=os.WriteFile("tools/baseline/sectionless.elf",raw,0644);err!=nil{panic(err)}}
}
