package aiff
import("bytes";"encoding/binary";"fmt";"reflect";"testing")
func authorAIFF(bits uint16, frames uint32, offset uint32, pcm []byte) []byte {
 chunk:=func(id string,b []byte)[]byte{var out bytes.Buffer;out.WriteString(id);binary.Write(&out,binary.BigEndian,uint32(len(b)));out.Write(b);if len(b)%2!=0 {out.WriteByte(0)};return out.Bytes()}
 var comm,ssnd,body,out bytes.Buffer
 binary.Write(&comm,binary.BigEndian,uint16(1));binary.Write(&comm,binary.BigEndian,frames);binary.Write(&comm,binary.BigEndian,bits)
 comm.Write([]byte{0x40,0x0e,0xac,0x44,0,0,0,0,0,0})
 binary.Write(&ssnd,binary.BigEndian,offset);binary.Write(&ssnd,binary.BigEndian,uint32(0));ssnd.Write(make([]byte,offset));ssnd.Write(pcm)
 body.WriteString("AIFF");body.Write(chunk("COMM",comm.Bytes()));body.Write(chunk("SSND",ssnd.Bytes()))
 out.WriteString("FORM");binary.Write(&out,binary.BigEndian,uint32(body.Len()));out.Write(body.Bytes());return out.Bytes()
}
func TestAuthorBaseline(t *testing.T){
 d:=NewDecoder(bytes.NewReader(authorAIFF(16,2,0,[]byte{0x04,0xd2,0xfb,0x2e})))
 b,err:=d.FullPCMBuffer();if err!=nil||!reflect.DeepEqual(b.Data,[]int{1234,-1234}) {t.Fatalf("normal: %v %v",b,err)}
 if d.PCMLen()!=0 {t.Fatalf("current blockSize interpretation changed: %d",d.PCMLen())}
 odd:=NewDecoder(bytes.NewReader(authorAIFF(8,1,2,[]byte{17})))
 ob,err:=odd.FullPCMBuffer();if err!=nil||!reflect.DeepEqual(ob.Data,[]int{17,0}) {t.Fatalf("odd current: %v %v",ob,err)}
 if odd.PCMLen()!=4294967294 {t.Fatalf("current offset underflow: %d",odd.PCMLen())}
 fmt.Println("BASELINE normal_pcm=1234,-1234 odd_pcm=17,0 pcm_len=4294967294")
}
