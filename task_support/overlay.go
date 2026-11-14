// Generate a Go overlay for the exported package, including added production/tests.
package main
import("encoding/json";"fmt";"os";"path/filepath";"strings")
func main(){
 if len(os.Args)!=3 {panic("overlay SOURCE GOROOT_PACKAGE")}
 source,e:=filepath.Abs(os.Args[1]);if e!=nil{panic(e)}
 base,e:=filepath.Abs(os.Args[2]);if e!=nil{panic(e)}
 m:=map[string]string{}
 old,e:=os.ReadDir(base);if e!=nil{panic(e)}
 for _,f:=range old {if !f.IsDir()&&strings.HasSuffix(f.Name(),".go"){m[filepath.Join(base,f.Name())]=""}}
 files,e:=os.ReadDir(source);if e!=nil{panic(e)}
 for _,f:=range files {if !f.IsDir()&&strings.HasSuffix(f.Name(),".go"){m[filepath.Join(base,f.Name())]=filepath.Join(source,f.Name())}}
 b,e:=json.Marshal(struct{Replace map[string]string}{m});if e!=nil{panic(e)};fmt.Println(string(b))
}
