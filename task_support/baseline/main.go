package main

import (
 "fmt"
 "net/http"
 "net/http/httptest"
 "net/url"
 "github.com/headzoo/surf"
)

func must(err error) { if err != nil { panic(err) } }
func main() {
 observed := make(chan url.Values, 4)
 ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
  if r.Method == "POST" { must(r.ParseForm()); observed <- r.PostForm; fmt.Fprint(w,"<title>receipt</title>"); return }
  token, name, action := "token-a", "Alice", "/old-submit"
  if r.URL.Path == "/fresh" { token, name, action = "token-b", "Server", "/fresh-submit" }
  fmt.Fprintf(w,`<title>entry</title><form id="apply" method="post" action="%s"><input name="token" type="hidden" value="%s"><input name="person" value="%s"><input type="submit" name="save" value="yes"></form>`,action,token,name)
 }))
 defer ts.Close()
 bow := surf.NewBrowser(); must(bow.Open(ts.URL+"/first"))
 old, err := bow.Form("#apply"); must(err); must(old.Input("person","Ada"))
 must(bow.Open(ts.URL+"/fresh")); fresh, err := bow.Form("#apply"); must(err)
  must(old.Submit()); a:= <-observed
  must(fresh.Submit()); b:= <-observed
  if a.Get("token")!="token-a" || a.Get("person")!="Ada" || b.Get("token")!="token-b" || b.Get("person")!="Server" { panic("unexpected upstream workflow") }
  fmt.Printf("SURF_WORKFLOW old=%s/%s fresh=%s/%s\n",a.Get("token"),a.Get("person"),b.Get("token"),b.Get("person"))
}
