package main
import (
 "bytes"
 "encoding/binary"
 "fmt"
 "os"
 macho "github.com/blacktop/go-macho"
 "github.com/blacktop/go-macho/task_support/fixture"
)
func run() error {
 raw,err:=fixture.Normal();if err!=nil{return err}
 f,err:=macho.NewFile(bytes.NewReader(raw));if err!=nil{return err};defer f.Close()
 s:=f.Section("__TEXT","__unwind_info");if s==nil{return fmt.Errorf("section missing")}
 data,err:=s.Data();if err!=nil{return err}
 off,err:=f.GetOffset(fixture.Base+0x400);if err!=nil{return err}
 version:=binary.LittleEndian.Uint32(data);encoding:=binary.LittleEndian.Uint32(data[0x100c:])
 fmt.Printf("MACHO base=%#x unwind_bytes=%d version=%d raw_encoding=%#x text_file=%#x\n",f.GetBaseAddress(),len(data),version,encoding,off)
 if version!=1||encoding!=0x02040000||off!=0x400{return fmt.Errorf("normal fixture mismatch")};return nil
}
func main(){if err:=run();err!=nil{fmt.Fprintln(os.Stderr,err);os.Exit(1)}}
