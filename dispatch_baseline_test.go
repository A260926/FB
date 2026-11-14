package godet_test

import (
 "encoding/json"
 "fmt"
 "net/http"
 "net/http/httptest"
 "strings"
 "testing"
 "time"

 "github.com/gorilla/websocket"
 "github.com/raff/godet"
)

// Normal protocol input only. It contains no overflow or termination repair.
func TestDispatchBaseline(t *testing.T) {
 mux := http.NewServeMux()
 server := httptest.NewServer(mux)
 defer server.Close()
 commands := make(chan map[string]interface{}, 16)
 trigger := make(chan struct{})
 wsDone := make(chan struct{})
 mux.HandleFunc("/json/list", func(w http.ResponseWriter, r *http.Request) {
  json.NewEncoder(w).Encode([]map[string]interface{}{{"id":"one", "type":"page", "webSocketDebuggerUrl":"ws"+strings.TrimPrefix(server.URL,"http")+"/page/one"}})
 })
 mux.HandleFunc("/page/one", func(w http.ResponseWriter, r *http.Request) {
  ws, err := (&websocket.Upgrader{}).Upgrade(w,r,nil)
  if err != nil { return }
  defer ws.Close()
  defer close(wsDone)
  // Only this goroutine writes the server websocket.
  for {
   var q map[string]interface{}
   if ws.ReadJSON(&q)!=nil {return}
   commands <- q
   result := map[string]interface{}{}
   if q["method"]=="Runtime.evaluate" { result["result"]=map[string]interface{}{"type":"number","value":42} }
   if ws.WriteJSON(map[string]interface{}{"id":q["id"],"result":result})!=nil{return}
   if q["method"]=="Page.enable" {
    <-trigger
    if ws.WriteJSON(map[string]interface{}{"method":"Page.loadEventFired","params":map[string]interface{}{"stamp":1}})!=nil{return}
   }
  }
 })
 remote, err:=godet.Connect(strings.TrimPrefix(server.URL,"http://"), false)
 if err!=nil {t.Fatal(err)}
 defer remote.Close()
 callback := make(chan error,1)
 remote.CallbackEvent("Page.loadEventFired",func(p godet.Params){callback<-remote.MouseEvent(godet.MousePress,7,9,godet.LeftButton())})
 closed := make(chan struct{},1)
 remote.CallbackEvent(godet.EventClosed,func(godet.Params){closed<-struct{}{}})
 if err:=remote.PageEvents(true);err!=nil {t.Fatal(err)}
 close(trigger)
 select {case err:=<-callback: if err!=nil{t.Fatal(err)};case <-time.After(3*time.Second):t.Fatal("normal callback request stalled")}
 value,err:=remote.Evaluate("6*7")
 if err!=nil || value!=float64(42){t.Fatalf("evaluate: %v %v",value,err)}
 a,b,c:=<-commands,<-commands,<-commands
 if a["method"]!="Page.enable" || b["method"]!="Input.dispatchMouseEvent" || c["method"]!="Runtime.evaluate" {t.Fatal("unexpected command order",a,b,c)}
 p:=b["params"].(map[string]interface{})
 if p["x"]!=float64(7)||p["y"]!=float64(9)||p["button"]!="left"||p["type"]!="mousePressed"{t.Fatal("wrong mouse request",p)}
 if err:=remote.Close();err!=nil{t.Fatal(err)}
 select{case <-closed:case <-time.After(3*time.Second):t.Fatal("normal close notification stalled")}
 select{case <-wsDone:case <-time.After(3*time.Second):t.Fatal("server socket remains open")}
 fmt.Println("GODET_BASELINE_OK callback mouse evaluate close")
}
