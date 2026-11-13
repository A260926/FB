package expect
import("bufio";"io";"log";"strings";"testing";"time";"bytes")
func baselineConsole(input io.Reader)(*Console,error){
 pp,e:=NewPassthroughPipe(input);if e!=nil{return nil,e}
 d:=time.Second
 return &Console{opts:ConsoleOpts{Logger:log.New(io.Discard,"",0),ReadTimeout:&d},passthroughPipe:pp,runeReader:bufio.NewReaderSize(pp,4),closers:[]io.Closer{pp}},nil
}
func TestUnitBaseline(t *testing.T){
 c,e:=baselineConsole(strings.NewReader("ready> done"));if e!=nil{t.Fatal(e)};defer c.Close()
 calls:=0; var first string
 a,e:=c.Expect(All(String("ready"),String(">")).Then(func(b *bytes.Buffer)error{calls++;first=b.String();return nil}))
 if e!=nil||a!="ready>"||first!=a||calls!=1{t.Fatalf("first=%q callback=%q calls=%d err=%v",a,first,calls,e)}
 b,e:=c.Expect(String("done"));if e!=nil||b!=" done"{t.Fatalf("remaining=%q err=%v",b,e)}
 t.Log("EXPECT_FLOW first=ready> callback=1 remaining=done")
}
