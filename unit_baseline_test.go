package retryablehttp
import("io";"net/http";"strings";"testing";"time")
type baselineTransport struct{ calls int; bodies []string }
func(b *baselineTransport)RoundTrip(r *http.Request)(*http.Response,error){
 b.calls++;v,e:=io.ReadAll(r.Body);if e!=nil{return nil,e};r.Body.Close();b.bodies=append(b.bodies,string(v))
 code:=200;if b.calls==1{code=503}
 return &http.Response{StatusCode:code,Header:make(http.Header),Body:io.NopCloser(strings.NewReader("ok")),Request:r},nil
}
func TestUnitBaseline(t *testing.T){
 tr:=new(baselineTransport);c:=NewClient();c.Logger=nil;c.HTTPClient=&http.Client{Transport:tr};c.RetryMax=1;c.Backoff=func(time.Duration,time.Duration,int,*http.Response)time.Duration{return 0}
 req,e:=NewRequest("POST","https://collector.invalid/query",[]byte("filter=active"));if e!=nil{t.Fatal(e)}
 resp,e:=c.Do(req);if e!=nil{t.Fatal(e)};defer resp.Body.Close()
 if tr.calls!=2||len(tr.bodies)!=2||tr.bodies[0]!="filter=active"||tr.bodies[1]!=tr.bodies[0]||resp.StatusCode!=200{t.Fatalf("calls=%d bodies=%q response=%v",tr.calls,tr.bodies,resp)}
 t.Log("RETRY_REPLAY attempts=2 bodies=identical status=200")
}
