package main
import("fmt";"os";"github.com/abema/go-mp4/task_support/fixture")
func main(){_,t,e:=fixture.Open();if e!=nil{fmt.Fprintln(os.Stderr,e);os.Exit(1)};fmt.Println(t.TrackID)}
