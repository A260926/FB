package selenium_test

import (
 "fmt"
 "io"
 "net/http"
 "reflect"
 "strings"
 "testing"
 selenium "github.com/tebeka/selenium"
)
type baselineTransport struct{ seen []string }
func (b *baselineTransport) RoundTrip(r *http.Request)(*http.Response,error){
 b.seen=append(b.seen,r.Method+" "+r.URL.Path)
 body:=`{"value":null}`
 switch r.URL.Path {
 case "/session": body=`{"value":{"sessionId":"form","capabilities":{"browserName":"chrome","browserVersion":"120.0.0"}}}`
 case "/session/form/element": body=`{"value":{"element-6066-11e4-a52e-4f735466cecf":"host"}}`
 case "/session/form/element/host/element":body=`{"value":{"element-6066-11e4-a52e-4f735466cecf":"input"}}`
 case "/session/form/element/input/text":body=`{"value":"baseline field"}`
 }
 return &http.Response{StatusCode:200,Status:"200 OK",Header:http.Header{"Content-Type":[]string{"application/json"}},Body:io.NopCloser(strings.NewReader(body)),Request:r},nil
}
func TestBaselineOrdinaryForm(t *testing.T){
 old:=selenium.HTTPClient;tr:=&baselineTransport{};selenium.HTTPClient=&http.Client{Transport:tr};defer func(){selenium.HTTPClient=old}()
 wd,e:=selenium.NewRemote(selenium.Capabilities{"browserName":"chrome"},"http://webdriver.invalid");if e!=nil{t.Fatal(e)}
 host,e:=wd.FindElement(selenium.ByCSSSelector,"contact-card");if e!=nil{t.Fatal(e)}
 input,e:=host.FindElement(selenium.ByCSSSelector,"input");if e!=nil{t.Fatal(e)}
 text,e:=input.Text();if e!=nil||text!="baseline field"{t.Fatalf("text=%q err=%v",text,e)}
 if e=input.SendKeys("Ada");e!=nil{t.Fatal(e)}
 if len(tr.seen)!=5{t.Fatal(tr.seen)}
 _,root:=reflect.TypeOf(host).MethodByName("ShadowRoot")
 fmt.Printf("ordinary form text=%q requests=%d ShadowRoot_available=%v\n",text,len(tr.seen),root)
}
