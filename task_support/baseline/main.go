package main
import("fmt";"net";"time";coap "github.com/dustin/go-coap")
func main(){
 s,e:=net.ListenUDP("udp4",&net.UDPAddr{IP:net.IPv4(127,0,0,1)});if e!=nil{panic(e)};defer s.Close()
 done:=make(chan error,1);go func(){buf:=make([]byte,1500);s.SetReadDeadline(time.Now().Add(2*time.Second));n,a,e:=s.ReadFromUDP(buf);if e!=nil{done<-e;return};q,e:=coap.ParseMessage(buf[:n]);if e==nil{e=coap.Transmit(s,a,coap.Message{Type:coap.Acknowledgement,MessageID:q.MessageID})};if e==nil{e=coap.Transmit(s,a,coap.Message{Type:coap.NonConfirmable,Code:coap.Content,MessageID:91,Token:q.Token,Payload:[]byte("reading=42")})};done<-e}()
 c,e:=coap.Dial("udp4",s.LocalAddr().String());if e!=nil{panic(e)}
 r,e:=c.Send(coap.Message{Type:coap.Confirmable,Code:coap.GET,MessageID:7,Token:[]byte("t")});if e!=nil{panic(e)};if r.Code!=0||len(r.Payload)!=0{panic("expected transport ACK")}
 r,e=c.Receive();if e!=nil{panic(e)};if string(r.Payload)!="reading=42"{panic("wrong data")};if e=<-done;e!=nil{panic(e)};fmt.Println("COAP_BASELINE send=empty-ack receive=reading=42")
}
