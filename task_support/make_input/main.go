package main
import("os";"github.com/Eyevinn/mp4ff/task_support/fixture")
func main(){b,e:=fixture.Source(false);if e!=nil{panic(e)};if e=os.WriteFile(os.Args[1],b,0600);e!=nil{panic(e)}}
