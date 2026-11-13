package main
import("fmt";"io";"net/http";"strings";"sync";g "github.com/PuerkitoBio/gocrawl")
type ext struct{g.DefaultExtender;mu sync.Mutex; paths []string; visits int}
func(e *ext)Fetch(c *g.URLContext, ua string,head bool)(*http.Response,error){p:=c.URL().Path;e.mu.Lock();e.paths=append(e.paths,p);e.mu.Unlock();body:="<html></html>";if p=="/robots.txt"{body="User-agent: *\nAllow: /\nSitemap: http://a.test/maps/index.xml\n"};if p=="/root"{body=`<a href="/ordinary">normal</a>`};r,_:=http.NewRequest("GET",c.URL().String(),nil);return &http.Response{StatusCode:200,Status:"200 OK",Header:make(http.Header),Body:io.NopCloser(strings.NewReader(body)),Request:r},nil}
func(e *ext)Visited(c *g.URLContext,h interface{}){e.mu.Lock();defer e.mu.Unlock();e.visits++}
func main(){e:=&ext{};o:=g.NewOptions(e);o.CrawlDelay=0;o.LogFlags=0;o.URLNormalizationFlags=0;c:=g.NewCrawlerWithOptions(o);if err:=c.Run("http://a.test/root");err!=nil{panic(err)};e.mu.Lock();defer e.mu.Unlock();if len(e.paths)!=3||e.visits!=2{panic(fmt.Sprint(e.paths,e.visits))};fmt.Printf("GOCRAWL_PATHS=%v\nORDINARY_PAGES=2\nSITEMAP_FETCHES=0\n",e.paths)}
