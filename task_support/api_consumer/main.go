package main
import("bytes";"fmt";"os";mp4 "github.com/Eyevinn/mp4ff/mp4";"github.com/Eyevinn/mp4ff/task_support/fixture")
func main(){if e:=run();e!=nil{fmt.Fprintln(os.Stderr,e);os.Exit(1)}}
func run()error{if len(os.Args)==2{b,e:=os.ReadFile(os.Args[1]);if e!=nil{return e};if e=fixture.VerifyNormal(b);e!=nil{return e};fmt.Println("FRESH_OUTPUT_NORMAL_OK");return nil};b,e:=fixture.Source(false);if e!=nil{return e};rs:=bytes.NewReader(b);f,e:=mp4.DecodeFile(rs,mp4.WithDecodeMode(mp4.DecModeLazyMdat));if e!=nil{return e};var out bytes.Buffer;if e=mp4.DefragmentPreserveGaps(f,rs,&out);e!=nil{return e};if e=fixture.VerifyNormal(out.Bytes());e!=nil{return e};fmt.Printf("API_NORMAL_OK bytes=%d\n",out.Len());return nil}
