package main
import (
 "context"
 "encoding/json"
 "errors"
 "fmt"
 "github.com/go-rod/rod"
 "github.com/go-rod/rod/lib/cdp"
 "github.com/go-rod/rod/lib/devices"
 "github.com/go-rod/rod/lib/input"
 "github.com/go-rod/rod/lib/proto"
)
type client struct { fail bool; mods []int; events chan *cdp.Event }
func(c *client) Event() <-chan *cdp.Event { return c.events }
func(c *client) Call(ctx context.Context, _ string, method string, p interface{})([]byte,error){
 if err:=ctx.Err();err!=nil{return nil,err}
 if method=="Target.attachToTarget" { return []byte(`{"sessionId":"s"}`),nil }
 if method=="Input.dispatchKeyEvent" || method=="Input.dispatchMouseEvent" {
  b,_:=json.Marshal(p); var v struct {Modifiers int}; _=json.Unmarshal(b,&v); c.mods=append(c.mods,v.Modifiers)
  if c.fail {c.fail=false; return nil,errors.New("dispatch rejected")}
 }; return []byte(`{}`),nil
}
func main(){
 ctx,cancel:=context.WithCancel(context.Background()); defer cancel()
 c:=&client{events:make(chan *cdp.Event)}; defer close(c.events)
 b:=rod.New().Context(ctx).Client(c).DefaultDevice(devices.Clear); if err:=b.Connect();err!=nil {panic(err)}
 p,err:=b.PageFromTarget("t");if err!=nil {panic(err)}
 c.fail=true; if p.Keyboard.Press(input.ControlLeft)==nil {panic("missing failure")}
 if err=p.Mouse.MoveTo(proto.Point{X:1,Y:2});err!=nil {panic(err)}
 if c.mods[len(c.mods)-1]!=2 {panic("expected baseline phantom ctrl")}
 if err=p.Keyboard.Release(input.ControlLeft);err!=nil {panic(err)}
 if err=p.Keyboard.Press(input.ShiftLeft);err!=nil {panic(err)}
 c.fail=true; if p.Keyboard.Release(input.ShiftLeft)==nil {panic("missing release failure")}
 if err=p.Keyboard.Type('a');err!=nil {panic(err)}
 if c.mods[len(c.mods)-1]!=0 {panic("expected baseline lost shift")}
 dead,stop:=context.WithCancel(ctx);stop()
 if err=p.Context(dead).Keyboard.Type('b');err!=nil {panic("expected baseline ignored clone context")}
 fmt.Println("BASELINE rejected-control phantom-mouse-modifier=2 rejected-shift-release next-modifier=0 clone-cancel=ignored")
}
