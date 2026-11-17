// Package fixture supplies ordinary synthetic DWARF and a sparse byte source.
package fixture

import (
 "encoding/binary"
 "io"
)

func Normal(order binary.ByteOrder) (abbrev, info, str []byte, typeOffset uint64) {
 // CU(name:string), base_type(name:string, byte_size:data1, encoding:data1).
 abbrev = []byte{1,0x11,1,3,8,0,0,2,0x24,0,3,8,0x0b,0x0b,0x3e,0x0b,0,0,0}
 body := []byte{4,0,0,0,0,0,8,1,'u','n','i','t',0,2,'i','n','t',0,4,5,0}
 order.PutUint16(body[:2],4)
 info = make([]byte,4+len(body));order.PutUint32(info[:4],uint32(len(body)));copy(info[4:],body)
 return abbrev,info,[]byte("example\x00"),17
}

type Span struct { Offset int64; Data []byte }
// Sparse is an immutable logical section: gaps contain zero bytes.
// Calls and Bytes let tests observe how much the consumer actually requests.
type Sparse struct { Size int64; Spans []Span; Calls, Bytes int64 }
func (s *Sparse) ReadAt(p []byte, off int64) (int,error) {
 s.Calls++;s.Bytes+=int64(len(p))
 if off<0 { return 0,io.EOF };if off>=s.Size { if len(p)==0{return 0,nil};return 0,io.EOF }
 n:=len(p);if int64(n)>s.Size-off {n=int(s.Size-off)}
 for i:=0;i<n;i++{p[i]=0}
 for _,span:=range s.Spans {
  lo,hi:=off,off+int64(n);if lo<span.Offset{lo=span.Offset};if end:=span.Offset+int64(len(span.Data));hi>end{hi=end}
  if lo<hi{copy(p[lo-off:hi-off],span.Data[lo-span.Offset:hi-span.Offset])}
 }
 if n<len(p){return n,io.EOF};return n,nil
}
