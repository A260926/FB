package astisub_test

import (
 "bytes"
 "fmt"
 "strings"
 "testing"
 "time"
 astisub "github.com/asticode/go-astisub"
)

func TestSeedEditBaseline(t *testing.T) {
 s,err:=astisub.ReadFromWebVTT(strings.NewReader("WEBVTT\n\n1\n00:00:01.000 --> 00:00:05.000\n<v Alice><b>one <00:00:03.000>two</b>\n"))
 if err!=nil || len(s.Items)!=1 {t.Fatalf("read: %v",err)}
 l:=s.Items[0].Lines[0]
 if l.VoiceName!="Alice" || len(l.Items)!=2 || l.Items[1].StartAt!=3*time.Second {t.Fatalf("words: %#v",l)}
 var out bytes.Buffer
 if err=s.WriteToWebVTT(&out);err!=nil {t.Fatal(err)}
 r,err:=astisub.ReadFromWebVTT(&out)
 if err!=nil || r.Items[0].Lines[0].Items[1].StartAt!=3*time.Second {t.Fatalf("roundtrip: %v",err)}
 s.Fragment(2*time.Second)
 if len(s.Items)!=3 || s.Items[0].StartAt!=time.Second || s.Items[2].EndAt!=5*time.Second {t.Fatal("fragment intervals")}
 fmt.Println("SEED_EDIT_OK words=2 voice=Alice roundtrip=ok fragments=3")
}
