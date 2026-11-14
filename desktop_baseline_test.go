package vnc

import (
 "bytes"
 "encoding/binary"
 "fmt"
 "io"
 "net"
 "testing"
 "time"
)

// This is a small legacy success path, not an Extended Clipboard test.
func TestDesktopBaseline(t *testing.T) {
 client, server := net.Pipe()
 defer client.Close()
 defer server.Close()
 _ = client.SetDeadline(time.Now().Add(8*time.Second))
 _ = server.SetDeadline(time.Now().Add(8*time.Second))
 done := make(chan error, 1)
 expect := func(want []byte) error {
  got := make([]byte, len(want))
  if _, err := io.ReadFull(server, got); err != nil { return err }
  if !bytes.Equal(got,want) { return fmt.Errorf("legacy wire got %x want %x",got,want) }
  return nil
 }
 go func() { done <- func() error {
  if _,err:=io.WriteString(server,"RFB 003.008\n");err!=nil{return err}
  if err:=expect([]byte("RFB 003.008\n"));err!=nil{return err}
  if _,err:=server.Write([]byte{1,1});err!=nil{return err}
  if err:=expect([]byte{1});err!=nil{return err}
  if _,err:=server.Write([]byte{0,0,0,0});err!=nil{return err}
  if err:=expect([]byte{1});err!=nil{return err}
  init:=[]byte{1,64,0,200,32,24,0,1,0,255,0,255,0,255,16,8,0,0,0,0,0,0,0,7}
  init=append(init,[]byte("desktop")...)
  if _,err:=server.Write(init);err!=nil{return err}
  frames:=[][]byte{
   {2,0,0,1,0,0,0,0},
   append([]byte{6,0,0,0,0,0,0,7},[]byte("account")...),
   {4,1,0,0,0,0,0,97}, {4,0,0,0,0,0,0,97},
   {5,1,0,10,0,20}, {3,0,0,0,0,0,0,2,0,1},
   {0,0,0,0,32,24,0,1,0,255,0,255,0,255,16,8,0,0,0,0},
  }
  for _,frame:=range frames {if err:=expect(frame);err!=nil{return err}}
  _,err:=server.Write([]byte{2}); return err
 }() }()
 messages:=make(chan ServerMessage,1)
 c,err:=Client(client,&ClientConfig{ServerMessageCh:messages})
 if err!=nil{t.Fatal(err)}
 defer c.Close()
 if c.DesktopName!="desktop" || c.FrameBufferWidth!=320 || c.FrameBufferHeight!=200 {t.Fatal("handshake state")}
 for _,err=range []error{
  c.SetEncodings([]Encoding{new(RawEncoding)}),c.CutText("account"),
  c.KeyEvent(97,true),c.KeyEvent(97,false),c.PointerEvent(ButtonLeft,10,20),
  c.FramebufferUpdateRequest(false,0,0,2,1),
  c.SetPixelFormat(&PixelFormat{BPP:32,Depth:24,TrueColor:true,RedMax:255,GreenMax:255,BlueMax:255,RedShift:16,GreenShift:8}),
 }{if err!=nil{t.Fatal(err)}}
 if err=<-done;err!=nil{t.Fatal(err)}
 select {
 case msg:=<-messages: if _,ok:=msg.(*BellMessage);!ok{t.Fatalf("unexpected message %T",msg)}
 case <-time.After(8*time.Second): t.Fatal("missing bell")
 }
 // Ensure the oracle's own integers are big endian, independently of production helpers.
 if binary.BigEndian.Uint16([]byte{1,64})!=320 {t.Fatal("fixture")}
 t.Log("VNC_DESKTOP_BASELINE_OK handshake encodings clipboard key pointer update pixel bell")
}
