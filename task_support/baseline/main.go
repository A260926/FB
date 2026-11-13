package main
import("fmt";"net";"os";"time";dns "github.com/miekg/dns")
func rr(s string)dns.RR{r,e:=dns.NewRR(s);if e!=nil{panic(e)};return r}
func run(wrap bool)int{
 a,b:=net.Pipe();done:=make(chan error,1);q:=new(dns.Msg);q.SetAxfr("zone.test.");old:=rr("zone.test. 60 IN SOA ns.zone.test. host.zone.test. 4294967294 1 2 3 4");cur:=rr("zone.test. 60 IN SOA ns.zone.test. host.zone.test. 1 1 2 3 4");data:=rr("x.zone.test. 60 IN A 192.0.2.1")
 if wrap{q.SetIxfr("zone.test.",4294967294,"ns.zone.test.","host.zone.test.")}
 go func(){defer b.Close();c:=&dns.Conn{Conn:b};req,e:=c.ReadMsg();if e!=nil{done<-e;return};reply:=new(dns.Msg);reply.SetReply(req);if wrap{reply.Answer=[]dns.RR{cur};e=c.WriteMsg(reply);if e==nil{reply.Answer=[]dns.RR{old,cur,data,cur};e=c.WriteMsg(reply)}}else{reply.Answer=[]dns.RR{cur,data,cur};e=c.WriteMsg(reply)};done<-e}()
 tr:=&dns.Transfer{Conn:&dns.Conn{Conn:a},ReadTimeout:time.Second};ch,e:=tr.In(q,"unused");if e!=nil{panic(e)};n:=0;for env:=range ch{if env.Error!=nil{panic(env.Error)};n+=len(env.RR)};e=<-done;if !wrap&&e!=nil{panic(e)};return n
}
func main(){if len(os.Args)>1&&os.Args[1]=="wrap"{n:=run(true);if n!=1{panic(fmt.Sprint(n))};fmt.Println("DNS_IXFR_BASELINE wrap_records=1")}else{n:=run(false);if n!=3{panic(fmt.Sprint(n))};fmt.Println("DNS_XFR_BASELINE axfr_records=3")}}
