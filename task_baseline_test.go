// Baseline: exercises existing production Check/Repeat with legal actions.
package rapid
import "testing"
func TestProductionBaseline(t *testing.T) {
    steps := 0
    Check(t, func(rt *T) {
        value, model := 0, 0
        rt.Repeat(map[string]func(*T){
            "put": func(rt *T) { n := IntRange(1, 3).Draw(rt, "increment"); value += n; model += n; steps++ },
            "read": func(rt *T) { if value != model { rt.Fatalf("normal counter differs") }; steps++ },
            "": func(rt *T) { if value != model { rt.Fatalf("invariant differs") } },
        })
    })
    if steps == 0 { t.Fatal("no production actions executed") }
    t.Logf("BASELINE_RAPID actions=%d normal=true", steps)
}
