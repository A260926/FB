package main
import (
    "context"
    "testing"
    "github.com/leanovate/gopter"
    "github.com/leanovate/gopter/commands"
)
func TestGeneratedProperty(t *testing.T){
    s,_:=newSuite()
    params:=gopter.DefaultTestParametersWithSeed(791114)
    params.MinSize=1;params.MaxSize=1;params.MinSuccessfulTests=2;params.Workers=1
    result:=commands.DifferentialProp(context.Background(),s).Check(params)
    if !result.Passed() || s.calls<4 || s.creates!=s.closes || s.creates<4 {t.Fatal("generated property did not execute and close both normal implementations")}
}
