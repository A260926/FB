// Fixed consumer of the generated entry; only the zero-round normal lifecycle.
package main
import (
    "testing"
    "pgregory.net/rapid"
)
func TestGeneratedZero(t *testing.T) {
    rapid.Check(t, func(rt *rapid.T) {
        closed := 0
        factory := func() rapid.DiffBackend { return &backend{closes:&closed} }
        cfg := rapid.DiffConfig{Backends: []func() rapid.DiffBackend{factory, factory}, MaxPolls: 4}
        report := rt.Differential(cfg)
        if report.Status != "equal" || report.Rounds != 0 || closed != 2 || len(report.Plan) != 0 || len(report.Events) != 0 { rt.Fatal("generated zero-round lifecycle mismatch") }
    })
}
