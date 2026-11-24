package main
import (
 "context"
 "fmt"
 "path/filepath"
 r "github.com/mvrahden/go-test/internal/gotestrunner"
)
func main() {
 dir,err:=filepath.Abs("task_support/benign");if err!=nil {panic(err)}
 session:=r.StartFuzzSession(context.Background(),r.FuzzRunConfig{Jobs:1,BuildFlags:[]string{"-fuzztime=20x","-parallel=1"}},nil)
 ticket,err:=session.Submit("normal",r.FuzzTarget{Package:".",Dir:dir,Func:"FuzzEcho",Suite:"Echo"});if err!=nil {panic(err)}
 if ticket.Key!="normal" || ticket.Generation!=1 {panic("ticket identity")}
 result:=session.Wait()
 if len(result.Attempts)!=1 {panic("outcome count")}
 a:=result.Attempts[0]
 if a.Attempt!=ticket || a.Superseded || a.Outcome.Skipped || a.Outcome.EffectiveExitCode()!=0 || a.Outcome.Execs<20 {panic("normal fuzz result")}
 if _,err:=session.Submit("late",r.FuzzTarget{});err==nil {panic("sealed admission")}
 if session.Cancel(ticket) {panic("completed cancellation")}
 fmt.Println("API_CONSUMER generation=1 normal=pass")
}
