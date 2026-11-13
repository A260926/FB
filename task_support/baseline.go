package main
import("context";"encoding/json";"fmt";"net/http";"net/http/httptest";"strings";"github.com/mmcdole/gofeed")
const doc=`<feed xmlns="http://www.w3.org/2005/Atom" xml:base="../shared/"><title>Feed</title><id>urn:f</id><updated>2026-01-01T00:00:00Z</updated><link href="home"/><entry xml:base="items/"><title>Item</title><id>urn:i</id><link href="one"/><content type="html">&lt;img src="pic.png"&gt;</content></entry></feed>`
func main(){
 srv:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){if r.URL.Path=="/entry"{http.Redirect(w,r,"/feeds/2026/current.xml",302);return};w.Header().Set("Content-Type","application/atom+xml");fmt.Fprint(w,doc)}));defer srv.Close()
 p:=gofeed.NewParser();p.KeepOriginalFeed=true
 f,e:=p.ParseURLWithContext(srv.URL+"/entry",context.Background());if e!=nil{panic(e)}
 if len(f.Items)!=1||f.Items[0].Title!="Item"{panic("bad real feed parse")}
 b,_:=json.Marshal(map[string]any{"feed_link":f.Link,"item_link":f.Items[0].Link,"content":f.Items[0].Content});fmt.Println("REDIRECT_FEED_PARSED "+string(b))
 f,e=p.Parse(strings.NewReader(strings.ReplaceAll(doc,`xml:base="../shared/"`,`xml:base="https://absolute.invalid/shared/"`)));if e!=nil{panic(e)}
 if f.Items[0].Link!="https://absolute.invalid/shared/items/one"{panic(f.Items[0].Link)}
 fmt.Println("EXPLICIT_XML_BASE_OK "+f.Items[0].Link)
}
