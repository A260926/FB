package tail

import (
 "os"
 "path/filepath"
 "reflect"
 "testing"
 "time"
)
func TestCollectorBaselineFinite(t *testing.T) {
 for _,poll:=range []bool{false,true} {
  t.Run(map[bool]string{false:"notify",true:"poll"}[poll],func(t *testing.T){
   p:=filepath.Join(t.TempDir(),"events.log")
   if e:=os.WriteFile(p,[]byte("ab\n\n中\r\ntail"),0600); e!=nil {t.Fatal(e)}
   x,e:=TailFile(p,Config{MustExist:true,Poll:poll,CompleteLines:true,Logger:DiscardingLogger}); if e!=nil {t.Fatal(e)}
   defer x.Cleanup(); var got []string
   for line:=range x.Lines {if line.Err!=nil {t.Fatal(line.Err)};got=append(got,line.Text)}
   if e=x.Wait();e!=nil {t.Fatal(e)}
   if !reflect.DeepEqual(got,[]string{"ab","","中\r","tail"}) {t.Fatalf("%q",got)}
  })
 }
}
func TestCollectorBaselineSplitAndSeek(t *testing.T) {
 p:=filepath.Join(t.TempDir(),"events.log"); if e:=os.WriteFile(p,[]byte("old\nabcdef\n"),0600);e!=nil {t.Fatal(e)}
 x,e:=TailFile(p,Config{MustExist:true,CompleteLines:true,MaxLineSize:2,Location:&SeekInfo{Offset:4},Logger:DiscardingLogger}); if e!=nil {t.Fatal(e)}
 defer x.Cleanup(); var got []string
 for line:=range x.Lines {got=append(got,line.Text)}
 if e=x.Wait();e!=nil {t.Fatal(e)}
 if !reflect.DeepEqual(got,[]string{"ab","cd","ef"}) {t.Fatal(got)}
}
func TestCollectorBaselineFollowAppend(t *testing.T) {
 p:=filepath.Join(t.TempDir(),"events.log"); if e:=os.WriteFile(p,[]byte("first\n"),0600);e!=nil {t.Fatal(e)}
 x,e:=TailFile(p,Config{MustExist:true,Follow:true,ReOpen:true,Poll:true,CompleteLines:true,Logger:DiscardingLogger}); if e!=nil {t.Fatal(e)}
 defer x.Cleanup();defer x.Stop()
 receive:=func(want string){t.Helper();select{case line:=<-x.Lines:if line==nil||line.Text!=want {t.Fatalf("want %q got %+v",want,line)};case <-time.After(3*time.Second):t.Fatal("line deadline")}}
 receive("first")
 f,e:=os.OpenFile(p,os.O_APPEND|os.O_WRONLY,0600);if e!=nil {t.Fatal(e)}
 if _,e=f.WriteString("sec");e!=nil {t.Fatal(e)}
 select{case line:=<-x.Lines:t.Fatalf("premature record %+v",line);case <-time.After(30*time.Millisecond):}
 if _,e=f.WriteString("ond\n");e!=nil {t.Fatal(e)};f.Close();receive("second")
}
