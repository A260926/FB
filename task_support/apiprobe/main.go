// Fixed production consumer: one legal scalar command, no normalization implementation.
package main
import (
    "context"
    "fmt"
    "github.com/leanovate/gopter"
    "github.com/leanovate/gopter/commands"
    "github.com/leanovate/gopter/gen"
)
type probeCommand struct { *commands.ProtoCommand; calls *int }
func (p probeCommand) RunDifferential(_ commands.SystemUnderTest, bindings *commands.HandleBindings) (commands.DiffOutput, error) {
    *p.calls++
    if _,ok:=bindings.Resolve("unknown"); ok { panic("empty binding unexpectedly resolves") }
    return commands.DiffOutput{Value:commands.DiffValue{Kind:commands.DiffScalar,Text:"safe"}},nil
}
type suite struct { *commands.ProtoCommands; creates, closes, calls int }
func (s *suite) ImplementationNames() []string { return []string{"first","second"} }
func (s *suite) CloneState(state commands.State) commands.State { return state.(int) }
func (s *suite) NewImplementation(_ string, state commands.State) (commands.SystemUnderTest,error) {
    if state.(int)!=0 {panic("wrong initial state")};s.creates++;return new(int),nil
}
func (s *suite) DestroyImplementation(_ string, _ commands.SystemUnderTest){s.closes++}
func newSuite() (*suite, commands.Command) {
    s:=&suite{}
    cmd:=probeCommand{&commands.ProtoCommand{Name:"read",PreConditionFunc:func(st commands.State)bool{return st.(int)==0}},&s.calls}
    s.ProtoCommands=&commands.ProtoCommands{
        InitialStateGen:gen.Const(0),
        GenCommandFunc:func(commands.State)gopter.Gen{return gen.Const(cmd)},
    }
    return s,cmd
}
func main(){
    s,cmd:=newSuite()
    r:=commands.ReplayDifferential(context.Background(),s,0,cmd)
    if r==nil || r.Status!=gopter.PropTrue || s.creates!=2 || s.closes!=2 || s.calls!=2 {panic("normal scalar differential replay failed")}
    fmt.Println("API_PROBE_OK scalar=equal calls=2 close=2")
}
