// Fixed API consumer: one normal replayed command, not a scheduler implementation.
package main
import (
    "fmt"
    "pgregory.net/rapid"
)
type operation struct { polls *int; value []byte }
func (o *operation) Poll() (bool, []byte) { *o.polls++; return true, o.value }
func (*operation) Cancel() { panic("completed normal probe must not cancel") }
type backend struct { closes *int; starts *int; polls *int }
func (*backend) Ready(name string) bool { return name == "echo" }
func (b *backend) Start(name string, input []byte) rapid.DiffOperation {
    if name != "echo" || string(input) != "safe" { panic("unexpected legal command") }
    *b.starts++
    return &operation{b.polls, append([]byte(nil), input...)}
}
func (b *backend) Close() { *b.closes++ }
func main() {
    closed, started, polled := 0, 0, 0
    factory := func() rapid.DiffBackend { return &backend{&closed, &started, &polled} }
    cfg := rapid.DiffConfig{
        Backends: []func() rapid.DiffBackend{factory, factory}, MaxPolls: 4, Rounds: 1,
        Commands: []rapid.DiffCommand{{Name:"echo", Input:func(*rapid.T) []byte {panic("Replay must not generate parameters")}}},
    }
    report := rapid.ReplayDifferential(cfg, []rapid.DiffRound{{Command:0, Input:[]byte("safe"), Polls:[]int{0,1}}})
    if report.Status != "equal" || report.Rounds != 1 || closed != 2 || started != 2 || polled != 2 { panic("one-round replay lifecycle mismatch") }
    fmt.Println("API_PROBE_OK rounds=1 starts=2 polls=2 close=2")
}
