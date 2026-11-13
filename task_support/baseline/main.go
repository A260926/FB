package main
import("errors";"fmt";"net/http";"net/http/httptest";"sync/atomic";colly "github.com/gocolly/colly/v2")
func main(){
 var hits atomic.Int32
 s:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){hits.Add(1);if r.URL.Path=="/entry"{http.Redirect(w,r,"/leaf",302);return};w.WriteHeader(503);fmt.Fprint(w,"retry later")}));defer s.Close()
 c:=colly.NewCollector(colly.IgnoreRobotsTxt());e1:=c.Visit(s.URL+"/entry");if e1==nil{panic("503 unexpectedly accepted")}
 a,e:=c.HasVisited(s.URL+"/entry");if e!=nil{panic(e)};b,e:=c.HasVisited(s.URL+"/leaf");if e!=nil{panic(e)}
 e2:=c.Visit(s.URL+"/entry");var av *colly.AlreadyVisitedError
 if !a||!b||!errors.As(e2,&av)||hits.Load()!=2{panic(fmt.Sprintf("unexpected existing behavior %v %v %v hits=%d",a,b,e2,hits.Load()))}
 fmt.Printf("current: failed_redirect entry_visited=%t leaf_visited=%t requests=%d ordinary_retry_blocked=%t\n",a,b,hits.Load(),errors.As(e2,&av))
}
