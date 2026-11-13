package support_test

import (
 "bytes"
 "encoding/base64"
 "encoding/json"
 "fmt"
 "image"
 "image/color"
 "image/png"
 "io"
 "net/http"
 "strings"
 "testing"

 "github.com/tebeka/selenium"
)

type transport func(*http.Request)(*http.Response,error)
func (f transport) RoundTrip(r *http.Request)(*http.Response,error){return f(r)}

func TestOriginalScreenshot(t *testing.T){
 for _,w3c:=range []bool{false,true}{t.Run(fmt.Sprintf("w3c_%v",w3c),func(t *testing.T){
  var calls []string
  tile:=image.NewRGBA(image.Rect(0,0,5,3));tile.Set(4,2,color.RGBA{R:71,G:91,B:31,A:255})
  var pngbuf bytes.Buffer;if err:=png.Encode(&pngbuf,tile);err!=nil {t.Fatal(err)}
  old:=selenium.HTTPClient;defer func(){selenium.HTTPClient=old}()
  selenium.HTTPClient=&http.Client{Transport:transport(func(r *http.Request)(*http.Response,error){
   calls=append(calls,r.Method+" "+r.URL.Path)
   var body map[string]interface{};if r.Body!=nil {if err:=json.NewDecoder(r.Body).Decode(&body);err!=nil && err!=io.EOF {t.Fatal(err)}}
   var reply interface{}
   switch r.URL.Path {
   case "/session":
    if w3c {reply=map[string]interface{}{"value":map[string]interface{}{"sessionId":"s","capabilities":map[string]interface{}{"browserName":"chrome","browserVersion":"120.0.0"}}}} else {reply=map[string]interface{}{"status":0,"sessionId":"s","value":map[string]interface{}{"browserName":"chrome","version":"120.0.0"}}}
   case "/session/s/execute", "/session/s/execute/sync":
    want:="/session/s/execute";if w3c {want+="/sync"};if r.URL.Path!=want {t.Fatalf("dialect path %s",r.URL.Path)}
    if !strings.Contains(fmt.Sprint(body["script"]),"scrollWidth") {t.Fatal("metrics script missing")}
    reply=map[string]interface{}{"value":map[string]interface{}{"width":7,"height":5,"viewportWidth":3,"viewportHeight":2,"x":0,"y":0,"dpr":1.5}}
   case "/session/s/screenshot": reply=map[string]interface{}{"value":base64.StdEncoding.EncodeToString(pngbuf.Bytes())}
   default:t.Fatalf("unexpected %s",r.URL.Path)
   }
   data,err:=json.Marshal(reply);if err!=nil {t.Fatal(err)}
   return &http.Response{StatusCode:200,Header:http.Header{"Content-Type":[]string{"application/json"}},Body:io.NopCloser(bytes.NewReader(data))},nil
  })}
  wd,err:=selenium.NewRemote(selenium.Capabilities{"browserName":"chrome"},"http://fixture");if err!=nil {t.Fatal(err)}
  result,err:=wd.ExecuteScript("return {width:document.documentElement.scrollWidth,height:document.documentElement.scrollHeight,viewportWidth:innerWidth,viewportHeight:innerHeight,x:scrollX,y:scrollY,dpr:devicePixelRatio}",nil);if err!=nil {t.Fatal(err)}
  metrics,ok:=result.(map[string]interface{});if !ok || metrics["width"]!=float64(7) || metrics["height"]!=float64(5) {t.Fatalf("metrics %v",result)}
  data,err:=wd.Screenshot();if err!=nil {t.Fatal(err)}
  im,err:=png.Decode(bytes.NewReader(data));if err!=nil {t.Fatal(err)}
  if im.Bounds().Dx()!=5 || im.Bounds().Dy()!=3 || len(calls)!=3 {t.Fatalf("viewport %v calls %v",im.Bounds(),calls)}
  r,g,b,_:=im.At(4,2).RGBA();if r!=71*257 || g!=91*257 || b!=31*257 {t.Fatal("screenshot bytes changed")}
  fmt.Printf("DIALECT=%v METRICS=7x5 VIEWPORT_PNG=5x3 CALLS=3\n",w3c)
 })}
}
