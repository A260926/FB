package main
import("encoding/json";"os/exec";"os";"fmt";"bytes")
func main(){
 cmd:=exec.Command("go","test","-json","-count=1","-run","^TestLockOrderContract$","./go/analysis/passes/lockorder")
 b,e:=cmd.CombinedOutput();os.Stdout.Write(b);if e!=nil{os.Exit(1)}
 required:=map[string]bool{"TestLockOrderContract":false,"TestLockOrderContract/aliases":false,"TestLockOrderContract/summaries":false,"TestLockOrderContract/deferred":false,"TestLockOrderContract/cycles":false}
 dec:=json.NewDecoder(bytes.NewReader(b));for dec.More(){var v struct{Action,Test string};if e:=dec.Decode(&v);e!=nil{panic(e)};if v.Action=="fail"||v.Action=="skip"{os.Exit(2)};if v.Action=="pass"{if _,ok:=required[v.Test];ok{required[v.Test]=true}}}
 for k,v:=range required{if !v{fmt.Println("missing executed test",k);os.Exit(3)}}
 fmt.Println("LOCKORDER_TARGET_TESTS_EXECUTED groups=4")
}
