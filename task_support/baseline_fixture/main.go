package main
import("os";"github.com/go-delve/delve/task_support/fixture")
func main(){if len(os.Args)!=2 {panic("path")};if err:=os.WriteFile(os.Args[1],fixture.Build().ELF(),0600);err!=nil{panic(err)}}
