package main
import("fmt";"io";"net/http";"strings";"github.com/headzoo/surf")
type transport struct{body string}
func(t *transport)RoundTrip(r *http.Request)(*http.Response,error){
 if r.Method=="POST" {b,e:=io.ReadAll(r.Body);if e!=nil{return nil,e};t.body=string(b)}
 h:=`<input form="f" name="outside" value="A"><form id="f" method="post"><input name="z" value="1"><fieldset disabled><input name="blocked" value="bad"></fieldset><textarea name="memo">hello</textarea><input name="a" value="2"><input name="z" value="3"></form>`
 return &http.Response{StatusCode:200,Status:"200 OK",Header:http.Header{"Content-Type":{"text/html"}},Body:io.NopCloser(strings.NewReader(h)),Request:r},nil
}
func main(){b:=surf.NewBrowser();tr:=&transport{};b.SetTransport(tr);if e:=b.Open("http://capture.invalid/form");e!=nil{panic(e)};f,e:=b.Form("#f");if e!=nil{panic(e)};if e=f.Submit();e!=nil{panic(e)};want:="a=2&blocked=bad&memo=&z=1&z=3";if tr.body!=want{panic(tr.body)};fmt.Println("SURF_BASELINE form-body="+tr.body)}
