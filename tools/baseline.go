package main
import (
 "fmt"
 "github.com/datarhei/gosrt/circular"
 "github.com/datarhei/gosrt/congestion/live"
 "github.com/datarhei/gosrt/packet"
)
func main() {
 delivered:=0
 s:=live.NewSender(live.SendConfig{InitialSequenceNumber:circular.New(2,packet.MAX_SEQUENCENUMBER),MaxBW:1000,DropThreshold:10000000,OnDeliver:func(p packet.Packet){delivered++}})
 for i:=0;i<3;i++ {p:=packet.NewPacket(nil);p.SetData(make([]byte,84));p.Header().PktTsbpdTime=0;s.Push(p)}
 s.Tick(1000)
 if delivered!=3 || s.Stats().PktUnique!=3 {panic("unexpected current burst")}
 fmt.Println("BASELINE_SRT_THREE_PACKETS_ONE_TICK")
 if s.Stats().UsPktSndPeriod>=1000 {panic("unexpected current period")}
 fmt.Printf("BASELINE_SRT_MAXBW_IGNORED period_us=%.6f\n",s.Stats().UsPktSndPeriod)
 s.ACK(circular.New(5,packet.MAX_SEQUENCENUMBER))
 if s.Stats().PktBuf!=0 {panic("ACK did not release")}
 fmt.Println("BASELINE_SRT_ACK_RELEASE_OK")
}
