package main

import (
 "bufio"
 "fmt"
 "os"
 "os/exec"
 "strings"
 "sync/atomic"
 "time"
 expect "github.com/google/goexpect"
)
func must(err error) { if err!=nil { panic(err) } }
func main() {
 if len(os.Args)>1 && os.Args[1]=="child" {
  s:=bufio.NewScanner(os.Stdin)
  fmt.Print("NAME>"); if !s.Scan(){os.Exit(2)}; name:=s.Text()
  fmt.Print("ROLE>"); if !s.Scan(){os.Exit(2)}; role:=s.Text()
  fmt.Printf("RECEIPT %s/%s",name,role); if !s.Scan() || s.Text()!="quit" { os.Exit(2) }; return
 }
 cmd:=exec.Command(os.Args[0],"child"); in,err:=cmd.StdinPipe();must(err);out,err:=cmd.StdoutPipe();must(err)
 cmd.Stderr=os.Stderr;must(cmd.Start());var alive int32=1
 e,done,err:=expect.SpawnGeneric(&expect.GenOptions{In:in,Out:out,Wait:func()error {err:=cmd.Wait();atomic.StoreInt32(&alive,0);return err},Close:func()error{return in.Close()},Check:func()bool{return atomic.LoadInt32(&alive)!=0}},time.Second,expect.CheckDuration(time.Millisecond),expect.PartialMatch(true));must(err)
 res,err:=e.ExpectBatch([]expect.Batcher{&expect.BExp{R:"NAME>"},&expect.BSnd{S:"Ada\n"},&expect.BExp{R:"ROLE>"},&expect.BSnd{S:"ops\n"},&expect.BExp{R:"RECEIPT (Ada)/(ops)"}},time.Second);must(err)
 if len(res)!=3 || res[0].Idx!=0 || res[1].Idx!=2 || res[2].Idx!=4 || len(res[2].Match)!=3 || strings.Join(res[2].Match[1:],"/")!="Ada/ops" { panic(fmt.Sprint(res)) }
 fmt.Println("EXPECT_WORKFLOW completed=3 indices=0,2,4 receipt=Ada/ops")
 must(e.Send("quit\n")); select {case err:= <-done:must(err);case <-time.After(3*time.Second):panic("child did not exit")} // cmd.Wait has closed its owned process pipes.
}
