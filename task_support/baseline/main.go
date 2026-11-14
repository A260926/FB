package main
import (
 "bytes"
 "fmt"
 "os"
 pe "github.com/saferwall/pe"
 "github.com/saferwall/pe/task_support/fixture"
)
func main(){
 b:=fixture.Image();before:=append([]byte(nil),b...)
 f,err:=pe.NewBytes(b,nil);if err==nil{err=f.Parse()};if err!=nil{fmt.Fprintln(os.Stderr,err);os.Exit(1)}
 if f.NtHeader.FileHeader.Machine!=0xaa64||len(f.Export.Functions)!=1||f.Export.Functions[0].Name!="NativeSymbol"||!bytes.Equal(b,before){panic("native production parsing changed")}
 fmt.Printf("native machine=%x export=%s rva=%x sections=%d loadconfig=%t dvrt=%t input_unchanged=true\n",uint16(f.NtHeader.FileHeader.Machine),f.Export.Functions[0].Name,f.Export.Functions[0].FunctionRVA,len(f.Sections),f.HasLoadCFG,f.LoadConfig.DVRT!=nil)
}
