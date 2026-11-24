package main
import (
 "fmt"
 "github.com/kolkov/racedetector/race"
)
func main(){
 a:=race.TaskRef{ID:1,Generation:1}
 es:=[]race.TaskEvent{
 {Op:"create",Task:a},
 {Op:"switch",Task:a,Worker:1},
 {Op:"write",Task:a,Worker:1,Object:7},
 {Op:"suspend",Task:a,Worker:1},
 {Op:"switch",Task:a,Worker:2},
 {Op:"read",Task:a,Worker:2,Object:7},
 }
 r,err:=race.ReplayTaskEvents(es,8)
 if err!=nil || r.Accepted!=len(es) || len(r.Reports)!=0 || len(r.States)!=1 {panic("real task replay API must execute the migration")}
 st:=r.States[0]
 if st.Task!=a || !st.Active || st.Worker!=2 || st.State!="running" {panic("task state mismatch")}
 ordered,err:=r.Ordered(2,5);if err!=nil || !ordered {panic("migration lost task order")}
 fmt.Println("TASK_REPLAY_API_OK")
}
