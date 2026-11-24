// Existing legal counter commands, executed through production Prop and Replay.
package commands_test
import (
    "testing"
    "github.com/leanovate/gopter"
    "github.com/leanovate/gopter/commands"
)
func TestProductionBaseline(t *testing.T){
    params:=gopter.DefaultTestParametersWithSeed(791114)
    params.MinSuccessfulTests=12;params.MinSize=3;params.MaxSize=8;params.Workers=1
    result:=commands.Prop(&counterCommands{}).Check(params)
    if !result.Passed(){t.Fatalf("normal property failed: %v",result)}
    c:=&counter{}
    replay:=commands.Replay(c,0,IncCommand,IncCommand,GetCommand,DecCommand,GetCommand)
    if !replay.Success() || c.value!=1 {t.Fatal("normal replay failed")}
    t.Logf("BASELINE_GOPTER succeeded=%d replay_value=%d",result.Succeeded,c.value)
}
