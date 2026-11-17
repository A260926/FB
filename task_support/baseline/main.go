package main
import (
 "bytes"
 "fmt"
 "os"
 "reflect"
 macho "github.com/blacktop/go-macho"
 "github.com/blacktop/go-macho/task_support/fixture"
 "github.com/blacktop/go-macho/types"
)
func run() error {
 image:=fixture.Build(true,false,false)
 f,err:=macho.NewFile(bytes.NewReader(image.Bytes));if err!=nil{return err};defer f.Close()
 if f.Symtab==nil||len(f.Symtab.Syms)!=6||f.Symtab.Syms[2].Name!="_same"||f.Symtab.Syms[3].Value!=0x200{return fmt.Errorf("ordinary symbols mismatch")}
 if f.Dysymtab==nil||len(f.Dysymtab.IndirectSyms)!=3||f.Dysymtab.IndirectSyms[1]!=types.INDIRECT_SYMBOL_LOCAL{return fmt.Errorf("ordinary indirect symbols mismatch")}
 fmt.Printf("SYMBOLS count=%d duplicate=%s/%s values=%#x/%#x\n",len(f.Symtab.Syms),f.Symtab.Syms[2].Name,f.Symtab.Syms[3].Name,f.Symtab.Syms[2].Value,f.Symtab.Syms[3].Value)
 fmt.Printf("DYSYMTAB modules=%d toc=%d refs=%d indirect=%v\n",f.Dysymtab.Nmodtab,f.Dysymtab.Ntoc,f.Dysymtab.Nextrefsyms,f.Dysymtab.IndirectSyms)
 fmt.Printf("MODULE_MODEL present=%v\n",reflect.ValueOf(f.Dysymtab).Elem().FieldByName("ModuleTable").IsValid())
 reversed:=fixture.Build(true,false,true);g,revErr:=macho.NewFile(bytes.NewReader(reversed.Bytes));if g!=nil {g.Close()};fmt.Printf("DYSYMTAB_FIRST error=%v\n",revErr!=nil)
 fmt.Println("BASELINE_OK");return nil
}
func main(){if err:=run();err!=nil{fmt.Fprintln(os.Stderr,err);os.Exit(1)}}
