package main
import (
 "fmt"
 "github.com/kolkov/racedetector/internal/race/detector"
 "github.com/kolkov/racedetector/internal/race/goroutine"
)
func main(){
 d:=detector.NewDetector()
 a,b,c:=goroutine.Alloc(1),goroutine.Alloc(2),goroutine.Alloc(3)
 d.OnRead(0x1000,a);d.OnRead(0x1000,b)
 d.OnRelease(0x2000,a);d.OnRelease(0x3000,b)
 d.OnAcquire(0x2000,c);d.OnAcquire(0x3000,c);d.OnWrite(0x1000,c)
 if d.RacesDetected()!=0 || d.GetPromotionStats().Promotions<1 {panic("normal promoted frontier not preserved")}
 fmt.Println("FASTTRACK_BASELINE promoted=1 synchronized_write=1 races=0")
}
