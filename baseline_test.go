package pop3
import (
 "bufio"
 "fmt"
 "io"
 "net"
 "testing"
)
type testDialer struct{c net.Conn}
func(d testDialer) Dial(_, _ string)(net.Conn,error){return d.c,nil}
func TestMailCapture(t *testing.T){
 a,b:=net.Pipe();defer a.Close();defer b.Close()
 done:=make(chan error,1)
 go func(){
  r:=bufio.NewReader(b);io.WriteString(b,"+OK ready\r\n")
  for _,step:=range []struct{cmd,reply string}{
   {"UIDL\r\n","+OK ids\r\n7 uid-seven\r\n12 uid-twelve\r\n.\r\n"},
   {"RETR 7\r\n","+OK mail\r\nSubject: Captured\r\n\r\n..leading\r\n.\r\n"},
  }{
   got,e:=r.ReadString('\n');if e!=nil {done<-e;return};if got!=step.cmd {done<-fmt.Errorf("command %q",got);return}
   if _,e=io.WriteString(b,step.reply);e!=nil {done<-e;return}
  };done<-nil
 }()
 c,e:=New(Opt{Dialer:testDialer{a}}).NewConn();if e!=nil {t.Fatal(e)}
 ids,e:=c.Uidl(0);if e!=nil || len(ids)!=2 || ids[0].ID!=7 || ids[1].UID!="uid-twelve" {t.Fatalf("ids=%v err=%v",ids,e)}
 raw,e:=c.RetrRaw(7);if e!=nil {t.Fatal(e)}
 if raw.String()!="Subject: Captured\r\n\r\n.leading\r\n" {t.Fatalf("raw=%q",raw.String())}
 if e=<-done;e!=nil {t.Fatal(e)}
 fmt.Println("POP3_BASELINE uid=uid-seven raw-dot=preserved")
}
