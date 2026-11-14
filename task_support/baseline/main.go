package main
import("debug/dwarf";"debug/elf";"fmt";"os")
func main(){
 probe:=len(os.Args)>1&&os.Args[1]=="--probe"
 f,e:=elf.Open("task_support/samples/main.elf");if e!=nil{panic(e)};defer f.Close()
 d,e:=f.DWARF();if e!=nil{panic(e)};r:=d.Reader();cu,e:=r.Next();if e!=nil{panic(e)}
 b,e:=r.Next();if e!=nil{panic(e)};t,e:=d.Type(b.Offset);if e!=nil{panic(e)}
 if cu.Tag!=dwarf.TagCompileUnit||t.String()!="local"||t.Size()!=4{panic("normal decode mismatch")}
 fmt.Printf("NORMAL cu=%s type=%s size=%d offset=%d\n",cu.Val(dwarf.AttrName),t.String(),t.Size(),b.Offset)
 if probe{n,e:=r.Next();if e!=nil{panic(e)};fmt.Printf("CURRENT supplementary name=%T:%v type=%T:%v\n",n.Val(dwarf.AttrName),n.Val(dwarf.AttrName),n.Val(dwarf.AttrType),n.Val(dwarf.AttrType))}
 s,e:=elf.Open("task_support/samples/supplement.elf");if e!=nil{panic(e)};defer s.Close();sd,e:=s.DWARF();if e!=nil{panic(e)}
 st,e:=sd.Type(15);if e!=nil{panic(e)};node,ok:=st.(*dwarf.StructType);if !ok||len(node.Field)!=1{panic("supplement struct missing")}
 ptr,ok:=node.Field[0].Type.(*dwarf.PtrType);if !ok||ptr.Type!=node{panic("supplement recursion missing")};fmt.Println("SUPPLEMENT standalone=Node recursive=true offset=15")
}
