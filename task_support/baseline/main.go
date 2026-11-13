package main
import("fmt";"net/http";"net/http/httptest";"sync";"time";"github.com/PuerkitoBio/gocrawl")
type ext struct{gocrawl.DefaultExtender}
func(*ext) RequestRobots(*gocrawl.URLContext,string)([]byte,bool){return []byte("User-agent: *\nAllow: /\n"),false}
func main(){var mu sync.Mutex;n:=map[string]int{};s:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){mu.Lock();n[r.URL.Path]++;mu.Unlock();w.Header().Set("Content-Type","text/html");if r.URL.Path!="/shared"{fmt.Fprint(w,`<a href="/shared">shared</a>`)}else{fmt.Fprint(w,"shared")}}));defer s.Close();o:=gocrawl.NewOptions(&ext{});o.CrawlDelay=0;o.WorkerIdleTTL=time.Second;o.LogFlags=0;c:=gocrawl.NewCrawlerWithOptions(o);if e:=c.Run([]string{s.URL+"/a",s.URL+"/b"});e!=nil{panic(e)};mu.Lock();defer mu.Unlock();if n["/a"]!=1||n["/b"]!=1||n["/shared"]!=1{panic(fmt.Sprint(n))};fmt.Println("existing crawl: a=1 b=1 shared=1; physical URL dedupe works")}
