package cookiejar
import("testing";"net/http";"net/url";"encoding/json";"time")
func TestCaptureBaseline(t *testing.T){
 u,_:=url.Parse("https://api.example.com/forms/page")
 j,e:=New(&Options{NoPersist:true});if e!=nil{t.Fatal(e)}
 j.SetCookies(u,[]*http.Cookie{{Name:"sid",Value:"one",Path:"/",Secure:true,Expires:time.Now().Add(time.Hour)}})
 if c:=j.Cookies(u);len(c)!=1||c[0].Value!="one"{t.Fatal(c)}
 raw,e:=json.Marshal(j);if e!=nil||len(raw)==0{t.Fatal(e)}
 c,e:=http.ParseSetCookie("part=value; Path=/; Secure; Partitioned");if e!=nil||!c.Partitioned{t.Fatal(c,e)}
 j.SetCookies(u,[]*http.Cookie{c});t.Logf("CURRENT_LEGACY_PARTITIONED_COUNT=%d",len(j.Cookies(u)))
 t.Log("BASELINE_COOKIE_STORAGE_OK")
}
