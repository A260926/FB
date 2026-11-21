package main
import("fmt";"image/png";"os";"path/filepath")
func main(){
 if len(os.Args)!=2{panic("output dir required")}
 var previous []uint32
 for i,name:=range []string{"before.png","after.png"}{
  f,e:=os.Open(filepath.Join(os.Args[1],name));if e!=nil{panic(e)}
  im,e:=png.Decode(f);f.Close();if e!=nil{panic(e)}
  b:=im.Bounds();if b.Dx()<2||b.Dy()<2{panic("too small")}
  var pixels []uint32
  varied:=false;var first [4]uint32
  for y:=b.Min.Y;y<b.Max.Y;y++{for x:=b.Min.X;x<b.Max.X;x++{
   r,g,bb,a:=im.At(x,y).RGBA();v:=[4]uint32{r,g,bb,a};if len(pixels)==0{first=v}else if v!=first{varied=true};pixels=append(pixels,r,g,bb,a)
  }}
  if !varied{panic("flat example")}
  if i==1{same:=len(previous)==len(pixels);if same{for j,p:=range pixels{if p!=previous[j]{same=false;break}}};if same{panic("identical example")}}
  previous=pixels
 }
 fmt.Println("EXAMPLE_IMAGES decoded varied different")
}
