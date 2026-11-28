package main
import("os";"image/png")
func main(){for _,p:=range os.Args[1:]{f,e:=os.Open(p);if e!=nil{panic(e)};im,e:=png.Decode(f);f.Close();if e!=nil{panic(e)};if im.Bounds().Dx()<16||im.Bounds().Dy()<16{panic("small example")}}}
