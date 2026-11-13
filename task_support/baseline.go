package main
import("fmt";"strings";"net/url";"github.com/go-shiori/dom";readability "github.com/go-shiori/go-readability")
func main(){
 para:=func(id,marker string)string{return `<p id="`+id+`">`+marker+` `+strings.Repeat("This article explains field measurements, collection constraints, and independent observations in ordinary prose. ",5)+`</p>`}
 source:=`<html><head><title>Field report</title></head><body><main><article><div>`+para("a1","ALPHA_MARK")+para("b1","BETA_MARK")+para("c1","GAMMA_MARK")+`</div><section>`+para("a2","ALPHA_MARK")+para("b2","BETA_MARK")+para("c2","GAMMA_MARK")+`</section><p>`+strings.Repeat("A unique closing observation remains in the article, with useful measurements. ",5)+`<a href="#b2">refer to measurement</a></p></article></main></body></html>`
 u,_:=url.Parse("https://capture.invalid/reports/current");p:=readability.NewParser()
 a,e:=p.Parse(strings.NewReader(source),u);if e!=nil{panic(e)}
 doc,e:=dom.Parse(strings.NewReader(source));if e!=nil{panic(e)};before:=dom.OuterHTML(doc)
 b,e:=p.ParseDocument(doc,u);if e!=nil{panic(e)}
 if a.TextContent!=b.TextContent||before!=dom.OuterHTML(doc){panic("entry compatibility")}
 n:=strings.Count(a.TextContent,"ALPHA_MARK");if n!=2{panic(fmt.Sprintf("expected duplicate text, got %d",n))}
 fmt.Printf("ARTICLE_DUPLICATE_RUN alpha=%d beta=%d gamma=%d\n",n,strings.Count(a.TextContent,"BETA_MARK"),strings.Count(a.TextContent,"GAMMA_MARK"))
 fmt.Printf("ANCHOR_TO_SECOND_COPY %v\n",strings.Contains(a.Content,`href="#b2"`))
 fmt.Printf("READER_DOCUMENT_AGREE length=%d original_unchanged=true\n",a.Length)
}
