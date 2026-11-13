package gos7
import("encoding/binary";"io";"net";"testing";"time")
func baselineRead(fragment bool)([]byte,error){
 a,b:=net.Pipe();defer a.Close();defer b.Close()
 go func(){
  h:=make([]byte,4);if _,e:=io.ReadFull(b,h);e!=nil{return};req:=make([]byte,int(binary.BigEndian.Uint16(h[2:])));copy(req,h);if _,e:=io.ReadFull(b,req[4:]);e!=nil{return}
  reply:=[]byte{3,0,0,28,2,0xf0,0x80,0x32,3,0,0,0,0,0,2,0,7,0,0,4,1,0xff,4,0,24,0x11,0x22,0x33}
  copy(reply[11:13],req[11:13])
  if !fragment{b.Write(reply);return}
  p:=append([]byte{3,0,0,15,2,0xf0,0},reply[7:15]...);b.Write(p)
  p=append([]byte{3,0,0,20,2,0xf0,0x80},reply[15:]...);b.Write(p)
 }()
 h:=&TCPClientHandler{};h.conn=a;h.Timeout=time.Second;h.IdleTimeout=0;h.PDULength=240
 out:=make([]byte,3);e:=NewClient(h).AGReadDB(1,0,3,out);return out,e
}
func TestUnitBaseline(t *testing.T){
 out,e:=baselineRead(false);if e!=nil||string(out)!=string([]byte{0x11,0x22,0x33}){t.Fatalf("bytes=%x err=%v",out,e)}
 t.Log("S7_SINGLE bytes=112233")
}
func TestObservedFragmentGap(t *testing.T){
 out,e:=baselineRead(true);if e==nil{t.Fatalf("starting version unexpectedly handles fragment: %x",out)}
 t.Logf("S7_FRAGMENT_GAP err=%v bytes=%x",e,out)
}
