package baseline
import (
 "bytes"
 "fmt"
 "net/url"
 "strings"
 "testing"
 mf "willnorris.com/go/microformats"
 "golang.org/x/net/html"
)
func TestIncludeBaseline(t *testing.T) {
 s:=`<div class="vcard" itemref="shared"><span class="fn">Ann</span></div><div id="shared"><span class="nickname">A</span></div>`
 doc,_:=html.Parse(strings.NewReader(s));u,_:=url.Parse("https://capture.test/")
 render:=func()string{var b bytes.Buffer;html.Render(&b,doc);return b.String()}
 before:=render();one:=mf.ParseNode(doc,u);after:=render();two:=mf.ParseNode(doc,u)
 fmt.Printf("ONE=%#v TWO=%#v MUTATED=%t\n",one.Items[0].Properties,two.Items[0].Properties,before!=after)
 if len(one.Items)!=1 || one.Items[0].Properties["name"][0]!="Ann" {t.Fatal(one)}
 fmt.Println("MF_INCLUDE_BASELINE_OK")
}
