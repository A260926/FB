package aiff
import (
 "bytes"
 "encoding/binary"
 "reflect"
 "testing"
)
func TestPCMStart(t *testing.T){
 var comm,ssnd,form bytes.Buffer
 binary.Write(&comm,binary.BigEndian,uint16(2));binary.Write(&comm,binary.BigEndian,uint32(3));binary.Write(&comm,binary.BigEndian,uint16(16))
 comm.Write([]byte{0x40,0x0e,0xac,0x44,0,0,0,0,0,0})
 binary.Write(&ssnd,binary.BigEndian,uint32(0));binary.Write(&ssnd,binary.BigEndian,uint32(0))
 want:=[]int16{1234,-1234,2,-2,32767,-32768};binary.Write(&ssnd,binary.BigEndian,want)
 form.WriteString("AIFF");form.WriteString("COMM");binary.Write(&form,binary.BigEndian,uint32(comm.Len()));form.Write(comm.Bytes());form.WriteString("SSND");binary.Write(&form,binary.BigEndian,uint32(ssnd.Len()));form.Write(ssnd.Bytes())
 var file bytes.Buffer;file.WriteString("FORM");binary.Write(&file,binary.BigEndian,uint32(form.Len()));file.Write(form.Bytes())
 d:=NewDecoder(bytes.NewReader(file.Bytes()));pcm,err:=d.FullPCMBuffer();if err!=nil{t.Fatal(err)}
 if !reflect.DeepEqual(pcm.Data,[]int{1234,-1234,2,-2,32767,-32768}){t.Fatal(pcm.Data)}
 t.Log("AIFF_PCM_BASE_OK scalars=6 frames=3")
}
