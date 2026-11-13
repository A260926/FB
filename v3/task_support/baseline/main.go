package main
import("errors";"fmt";"io";"net/http";"net/http/httptest";"os";"path/filepath";"sync/atomic";grab "github.com/cavaliergopher/grab/v3")
func main(){
 var hits,before,after atomic.Int32
 s:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){hits.Add(1);conn,buf,err:=w.(http.Hijacker).Hijack();if err!=nil{panic(err)};defer conn.Close();fmt.Fprint(buf,"HTTP/1.1 200 OK\r\nContent-Length: 6\r\nETag: \"v1\"\r\nAccept-Ranges: bytes\r\nConnection: close\r\n\r\nabc");if err:=buf.Flush();err!=nil{panic(err)}}));defer s.Close()
 dir,err:=os.MkdirTemp("","grab-baseline-");if err!=nil{panic(err)};defer os.RemoveAll(dir)
 req,err:=grab.NewRequest(filepath.Join(dir,"capture"),s.URL);if err!=nil{panic(err)}
 req.BeforeCopy=func(*grab.Response)error{before.Add(1);return nil};req.AfterCopy=func(*grab.Response)error{after.Add(1);return nil}
 resp:=grab.NewClient().Do(req);err=resp.Err();data,e:=os.ReadFile(resp.Filename);if e!=nil{panic(e)}
 if !errors.Is(err,io.ErrUnexpectedEOF)||string(data)!="abc"||resp.BytesComplete()!=3||resp.Size()!=6||hits.Load()!=1||before.Load()!=1||after.Load()!=0{panic(fmt.Sprintf("unexpected %v %q %d/%d hits=%d hooks=%d/%d",err,data,resp.BytesComplete(),resp.Size(),hits.Load(),before.Load(),after.Load()))}
 fmt.Printf("current: interrupted=%t bytes=%d/%d file=%q GETs=%d BeforeCopy=%d AfterCopy=%d\n",errors.Is(err,io.ErrUnexpectedEOF),resp.BytesComplete(),resp.Size(),data,hits.Load(),before.Load(),after.Load())
}
