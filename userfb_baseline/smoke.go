package main
import (
 "bytes"
 "fmt"
 fuzz "github.com/AdaLogics/go-fuzz-headers"
)
type packet struct { Kind byte; Payload byte }
func main() {
 var plain packet
 if err := fuzz.NewConsumer([]byte{7,9}).GenerateStruct(&plain); err != nil || plain.Kind != 7 || plain.Payload != 9 { panic("plain generation") }
 f := fuzz.NewConsumer(bytes.Repeat([]byte{0x11}, 32))
 called := 0
 f.AddFuncs([]interface{}{func(v *packet, c fuzz.Continue) error { called++; v.Kind=99; v.Payload=88; return nil }})
 var custom packet
 if err := f.GenerateWithCustom(&custom); err == nil || called != 1 || custom.Kind != 99 || custom.Payload != 88 { panic(fmt.Sprintf("unexpected custom behavior: %v %+v calls=%d",err,custom,called)) }
 fmt.Println("BASELINE_DEFAULT bytes=7,9")
 fmt.Println("BASELINE_CUSTOM callback=1 value=99,88 error=missing-child-handler")
}
