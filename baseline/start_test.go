package baseline
import (
 "fmt"
 "net/url"
 "strings"
 "testing"
 "unicode/utf8"
 readability "github.com/go-shiori/go-readability"
 "golang.org/x/net/html"
)
func TestReadabilityBaseline(t *testing.T) {
 htmlText:="<html><title>离线研究记录</title><body><article><h1>离线研究记录</h1><p>"+strings.Repeat("这是一段独立的研究说明，包含背景、方法和结论。 ",40)+"<a href='#note'>说明</a></p></article></body></html>"
 u,_:=url.Parse("https://capture.test/report");p:=readability.NewParser()
 a,e:=p.Parse(strings.NewReader(htmlText),u);if e!=nil || !strings.Contains(a.TextContent,"研究说明") {t.Fatal(a,e)}
 if a.Length!=utf8.RuneCountInString(a.TextContent){t.Fatal("rune length")}
 doc,_:=html.Parse(strings.NewReader(htmlText));b,e:=p.ParseDocument(doc,u)
 if e!=nil || a.Content!=b.Content || a.TextContent!=b.TextContent {t.Fatal("entry mismatch",e)}
 fmt.Println("READABILITY_PARSE_OK unicode=1 entries=2 reused_parser=1")
}
