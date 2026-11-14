package main
import (
 "bytes"
 "debug/elf"
 "debug/dwarf"
 "fmt"
 "os"
 "strings"
 "github.com/go-delve/delve/pkg/dwarf/op"
 "github.com/go-delve/delve/pkg/dwarf/godwarf"
 "github.com/go-delve/delve/task_support/fixture"
)
func main() {
 m:=fixture.Build(); raw:=m.ELF()
 if len(os.Args)==2 {if err:=os.WriteFile(os.Args[1],raw,0600);err!=nil {panic(err)}}
 f,err:=elf.NewFile(bytes.NewReader(raw));if err!=nil {panic(err)}
 d,err:=f.DWARF();if err!=nil {panic(err)}
 tree,err:=godwarf.LoadTree(11,d,0);if err!=nil {panic(err)}
 if len(tree.Children)!=2 {panic("CU tree")}
 r:=d.Reader();r.Seek(dwarf.Offset(m.RootA));entry,err:=r.Next();if err!=nil {panic(err)}
 expr:=entry.Val(dwarf.AttrLocation).([]byte)
 v,_,err:=op.ExecuteStackProgram(op.DwarfRegisters{},[]byte{0x11,28,0x11,28,0x22},8,nil)
 if err!=nil || v!=56 {panic("normal production expression failed")}
 fmt.Println("NORMAL_EXPR=56 CU_CHILDREN=2")
 _,_,err=op.ExecuteStackProgram(op.DwarfRegisters{},expr,8,nil)
 if err==nil || !strings.Contains(err.Error(),"invalid instruction") {panic(fmt.Sprintf("call current behavior changed: %v",err))}
 fmt.Printf("CURRENT_CALL_REJECTED=%v ROOT_A=%d PROC_A=%d ROOT_B=%d PROC_B=%d\n",err,m.RootA,m.ProcA,m.RootB,m.ProcB)
 fmt.Println("BASELINE_OK")
}
