package support_test

import (
 "encoding/json"
 "fmt"
 "io"
 "net/http"
 "strings"
 "testing"

 "github.com/sclevine/agouti"
)

type transport func(*http.Request) (*http.Response,error)
func (f transport) RoundTrip(r *http.Request)(*http.Response,error){return f(r)}

func TestOriginalSelection(t *testing.T) {
 var requests []string
 client:= &http.Client{Transport:transport(func(r *http.Request)(*http.Response,error){
  var body map[string]interface{}
  if r.Body!=nil {if err:=json.NewDecoder(r.Body).Decode(&body);err!=nil && err!=io.EOF {t.Fatal(err)}}
  key:=r.URL.Path+"|"+fmt.Sprint(body["value"])
  requests=append(requests,key)
  var value interface{}
  switch key {
  case "/session/s/elements|section": value=[]map[string]string{{"ELEMENT":"p1"},{"ELEMENT":"p2"}}
  case "/session/s/element/p1/elements|.//tr": value=[]map[string]string{{"ELEMENT":"a0"},{"ELEMENT":"a1"}}
  case "/session/s/element/p2/elements|.//tr": value=[]map[string]string{{"ELEMENT":"b0"},{"ELEMENT":"b1"},{"ELEMENT":"b2"}}
  case "/session/s/elements|section .row": value=[]map[string]string{{"ELEMENT":"a0"},{"ELEMENT":"b0"},{"ELEMENT":"b1"}}
  case "/session/s/elements|input": value=[]map[string]string{{"ELEMENT":"a1"},{"ELEMENT":"b1"}}
  default:
   if !strings.HasSuffix(r.URL.Path,"/clear") && !strings.HasSuffix(r.URL.Path,"/value") {t.Fatalf("unexpected command %s",key)}
  }
  data,err:=json.Marshal(map[string]interface{}{"value":value});if err!=nil {t.Fatal(err)}
  return &http.Response{StatusCode:200,Header:http.Header{"Content-Type":[]string{"application/json"}},Body:io.NopCloser(strings.NewReader(string(data)))},nil
 })}
 page:=agouti.JoinPage("http://fixture/session/s",agouti.HTTPClient(client))
 elems,err:=page.All("section").AllByXPath(".//tr").At(1).Elements()
 if err!=nil {t.Fatal(err)}
 if len(elems)!=2 || elems[0].ID!="a1" || elems[1].ID!="b1" {t.Fatalf("wrong per-parent result %v",elems)}
 fmt.Println("PER_PARENT_AT=a1,b1")
 requests=nil
 count,err:=page.All("section").All(".row").Count()
 if err!=nil || count!=3 || len(requests)!=1 || requests[0]!="/session/s/elements|section .row" {t.Fatalf("merge: %d %v %v",count,err,requests)}
 fmt.Println("CSS_MERGE=one_request,count3")
 requests=nil
 if err:=page.All("input").Fill("ok");err!=nil {t.Fatal(err)}
 if len(requests)!=5 || !strings.HasPrefix(requests[1],"/session/s/element/a1/clear|") || !strings.HasPrefix(requests[4],"/session/s/element/b1/value|") {t.Fatalf("fill order %v",requests)}
 fmt.Println("FILL=resolve_once,clear_value_each")
}
