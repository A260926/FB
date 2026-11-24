package main
import (
 "context"
 "fmt"
 "os"
 "path/filepath"
 r "github.com/mvrahden/go-test/internal/gotestrunner"
)
func main() {
 dir,err:=filepath.Abs("task_support/benign");if err!=nil {panic(err)}
 result:=r.RunFuzzTargets(context.Background(),[]r.FuzzTarget{{Package:".",Dir:dir,Func:"FuzzEcho",Suite:"Echo"}},r.FuzzRunConfig{Jobs:1,BuildFlags:[]string{"-fuzztime=20x","-parallel=1"}})
 if len(result.Outcomes)!=1 || result.ExitCode()!=0 || result.Outcomes[0].Execs<20 { fmt.Fprintln(os.Stderr,"baseline outcome",result);os.Exit(1) }
 fmt.Printf("BASELINE fuzz targets=1 exit=0 execs=%d\n",result.Outcomes[0].Execs)
}
