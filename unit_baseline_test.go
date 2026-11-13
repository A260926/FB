package webdav
import("context";"io";"net/http";"strings";"testing")
type baselineHTTP struct{ calls int }
func(b *baselineHTTP)Do(r *http.Request)(*http.Response,error){
 b.calls++; if r.Method!="PROPFIND"||r.Header.Get("Depth")!="1"{panic("unexpected request")}
 xml:=`<multistatus xmlns="DAV:"><response><href>/box/item</href><propstat><prop><resourcetype/><getcontentlength>3</getcontentlength><getetag>"one"</getetag></prop><status>HTTP/1.1 200 OK</status></propstat></response></multistatus>`
 return &http.Response{StatusCode:207,Header:make(http.Header),Body:io.NopCloser(strings.NewReader(xml)),Request:r},nil
}
func TestUnitBaseline(t *testing.T){
 h:=new(baselineHTTP);c,e:=NewClient(h,"https://collector.invalid/box/");if e!=nil{t.Fatal(e)}
 rows,e:=c.ReadDir(context.Background(),"",false)
 if e!=nil||len(rows)!=1||rows[0].Path!="/box/item"||rows[0].Size!=3||rows[0].ETag!="one"||h.calls!=1{t.Fatalf("rows=%+v err=%v calls=%d",rows,e,h.calls)}
 t.Log("WEBDAV_LIST rows=1 path=/box/item size=3")
}
