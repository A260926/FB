// Benign in-process coverage callback: every byte string returns normally.
package main
import dep "github.com/dvyukov/go-fuzz/go-fuzz-dep"
func main() {
 dep.Main([]func([]byte) int{func(data []byte) int {
  dep.CoverTab[7] = 1
  if len(data)>0 && data[0]=='A' {dep.CoverTab[11]=1}
  if len(data)==0 {dep.CoverTab[13]=1}
  return 0
 }})
}
