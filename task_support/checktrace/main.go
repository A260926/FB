// Validates the required example output, not the scheduling implementation.
package main
import ("encoding/json";"fmt";"os")
func main(){
 if len(os.Args)!=2 {panic("trace path")}
 b,e:=os.ReadFile(os.Args[1]);if e!=nil {panic(e)}
 var v struct{Issued,Completed uint64; Owners,Nodes []json.RawMessage}
 if e=json.Unmarshal(b,&v);e!=nil {panic(e)}
 if v.Issued<2 || v.Completed<2 || v.Completed!=v.Issued || len(v.Owners)==0 || len(v.Nodes)==0 {panic("empty or incomplete trace")}
 fmt.Println("OWNERSHIP_EXAMPLE_OUTPUT_OK")
}
