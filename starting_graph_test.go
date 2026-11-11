package trafilatura
import (
 "fmt"
 "net/url"
 "strings"
 "testing"
)
func TestStartingGraph(t *testing.T) {
 source:=`<html><head><script type="application/ld+json">{"@context":"https://schema.org","@graph":[{"@type":"WebPage","@id":"https://example.invalid/story","mainEntity":{"@id":"#main"}},{"@type":"NewsArticle","@id":"#recommend","headline":"Recommended article","author":{"@type":"Person","name":"Other Writer"}},{"@type":"NewsArticle","@id":"#main","headline":"Main article","author":{"@id":"#writer"}},{"@type":"Person","@id":"#writer","name":"Main Writer"}]}</script></head><body><article><p>`+strings.Repeat("Collected source content has enough sentences and useful details for the reader. ",20)+`</p></article></body></html>`
 u,_:=url.Parse("https://example.invalid/story")
 r,err:=Extract(strings.NewReader(source),Options{OriginalURL:u})
 if err!=nil {t.Fatal(err)}
 if r.Metadata.Title!="Recommended article" {t.Fatalf("title=%q",r.Metadata.Title)}
 if r.ContentNode==nil {t.Fatal("no article content")}
 fmt.Println("TRAFILATURA_BASELINE title=Recommended article")
}
