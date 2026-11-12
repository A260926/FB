package grab
import (
 "fmt"
 "io"
 "net/http"
 "strings"
 "testing"
)
type testHTTPClient struct{calls int}
func(c *testHTTPClient)Do(r *http.Request)(*http.Response,error){
 c.calls++
 raw:="--part\r\nContent-Range: bytes 8-9/10\r\n\r\nIJ\r\n--part\r\nContent-Range: bytes 0-1/10\r\n\r\nAB\r\n--part--\r\n"
 return &http.Response{StatusCode:206,Header:http.Header{"Content-Type":{"multipart/byteranges; boundary=part"}},Body:io.NopCloser(strings.NewReader(raw)),ContentLength:int64(len(raw)),Request:r},nil
}
func TestBaseline(t *testing.T){
 mock:=&testHTTPClient{};c:=NewClient();c.HTTPClient=mock
 req,e:=NewRequest("", "https://archive.example/data.bin");if e!=nil{t.Fatal(e)};req.NoStore=true
 b,e:=c.Do(req).Bytes();if e!=nil{t.Fatal(e)}
 if mock.calls!=1 || !strings.Contains(string(b),"Content-Range: bytes 8-9/10") || !strings.HasSuffix(string(b),"--part--\r\n"){t.Fatalf("not raw multipart: %q",b)}
 fmt.Printf("GRAB_MULTIRANGE_BASELINE_OK raw_mime_bytes=%d requests=%d\n",len(b),mock.calls)
}
