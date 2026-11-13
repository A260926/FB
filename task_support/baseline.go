package main
import("fmt";"strings";"time";"github.com/andybalholm/cascadia";"golang.org/x/net/html")
func main(){
 for _,n:=range []int{800,1600,3200}{
 var b strings.Builder;b.WriteString("<ul>");for i:=0;i<n;i++{b.WriteString(`<li class="visible">x</li>`)};b.WriteString("</ul>")
 doc,e:=html.Parse(strings.NewReader(b.String()));if e!=nil{panic(e)}
 s,e:=cascadia.Parse(`li:nth-child(2n of .visible):not(:nth-last-child(-n+2 of .visible))`);if e!=nil{panic(e)}
 start:=time.Now();v:=cascadia.QueryAll(doc,s);elapsed:=time.Since(start)
 if len(v)!=n/2-1{panic(fmt.Sprintf("wrong matches %d",len(v)))}
 fmt.Printf("POSITION_QUERY_OK n=%d matches=%d elapsed=%s\n",n,len(v),elapsed)
 }
 fmt.Println("NTH_FILTER_SUPPORTED; SOURCE_PER_CANDIDATE_SIBLING_SCAN")
}
