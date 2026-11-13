package ftp
import("io";"net";"net/textproto";"testing";"time")
func TestUnitBaseline(t *testing.T){
 control,peer:=net.Pipe();data,dp:=net.Pipe();defer control.Close();defer peer.Close();defer data.Close();defer dp.Close()
 limit:=time.Now().Add(2*time.Second);control.SetDeadline(limit);peer.SetDeadline(limit);data.SetDeadline(limit);dp.SetDeadline(limit)
 done:=make(chan error,1)
 go func(){
  p:=textproto.NewConn(peer)
  for i,want:=range []string{"EPSV","RETR sample","NOOP"}{
   got,e:=p.ReadLine();if e!=nil{done<-e;return};if got!=want{done<-ErrInvalidCommand;return}
   switch i{case 0:e=p.PrintfLine("229 Entering Extended Passive Mode (|||2121|)")
   case 1:e=p.PrintfLine("150 Data ready");go func(){dp.Write([]byte("sample"));dp.Close()}();if e==nil{e=p.PrintfLine("226 Transfer complete")}
   case 2:e=p.PrintfLine("200 OK")};if e!=nil{done<-e;return}
  };done<-nil
 }()
 c:=&ServerConn{options:&dialOptions{dialFunc:func(string,string)(net.Conn,error){return data,nil}},conn:textproto.NewConn(control),netConn:control,host:"127.0.0.1"}
 resp,e:=c.Retr("sample");if e!=nil{t.Fatal(e)};out,e:=io.ReadAll(resp);if e!=nil{t.Fatal(e)}
 if e=resp.Close();e!=nil{t.Fatal(e)};if e=c.NoOp();e!=nil{t.Fatal(e)}
 if e=<-done;e!=nil||string(out)!="sample"{t.Fatalf("output=%q peer=%v",out,e)}
 t.Log("FTP_RETR bytes=sample close=226 noop=200")
}
