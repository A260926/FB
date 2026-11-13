package main
import("encoding/json";"fmt";"io";"os")
func main(){f,e:=os.Open(".task-build/tests.json");if e!=nil{panic(e)};defer f.Close();d:=json.NewDecoder(f);want:=map[string]bool{"TestCBRClock":false,"TestCBRTables":false,"TestCBRMedia":false,"TestCBRCompatibility":false};for{var x struct{Action,Test string};e=d.Decode(&x);if e==io.EOF{break};if e!=nil{panic(e)};if x.Action=="skip"||x.Action=="fail"{panic("skip/fail")};if _,ok:=want[x.Test];ok&&x.Action=="pass"{want[x.Test]=true}};for n,v:=range want{if !v{panic(n+" did not pass")}};fmt.Println("four CBR groups passed")}
