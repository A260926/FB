// Fixed normal API consumer; provides no epoch implementation.
package main
import("fmt";"os";h "github.com/bluenviron/gohlslib/v2";f "github.com/bluenviron/gohlslib/v2/task_support/offlinefixture")
func main(){r,e:=f.Run([]uint64{240000},false,func(c *h.Client){c.EnableFMP4Discontinuities=true});if e!=nil||len(r.PTS)!=1||r.PTS[0]!=0||len(r.Payload)!=1||r.Payload[0]!=0x40||r.Times[0].IsZero(){fmt.Fprintln(os.Stderr,"epoch normal",r,e);os.Exit(41)};fmt.Println("EPOCH_API_NORMAL_OK")}
