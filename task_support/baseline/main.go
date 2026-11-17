package main
import (
 "encoding/binary"
 "fmt"
 "os"
 "unsafe"
 dwarf "github.com/blacktop/go-dwarf"
 "github.com/blacktop/go-dwarf/task_support/fixture"
)
func main(){
 a,i,s,off:=fixture.Normal(binary.LittleEndian)
 d,err:=dwarf.New(a,nil,nil,i,nil,nil,nil,s);if err!=nil{panic(err)}
 r:=d.Reader();cu,err:=r.Next();if err!=nil||cu==nil||cu.Tag!=dwarf.TagCompileUnit{panic("CU")}
 e,err:=r.Next();if err!=nil||e==nil||uint64(e.Offset)!=off{panic("DIE")}
 t,err:=d.Type(e.Offset);if err!=nil||t.String()!="int"||t.Size()!=4{fmt.Fprintln(os.Stderr,t,err);os.Exit(1)}
 fmt.Printf("BASELINE_OK type=%s size=%d offset_bits=%d\n",t.String(),t.Size(),unsafe.Sizeof(dwarf.Offset(0))*8)
}
