package main
import (
 "fmt"
 "math"
 "github.com/markphelps/go-trace/primitive"
 "github.com/markphelps/go-trace/render"
)
func main(){
 sphere:=primitive.NewSphere(0,0,-1,0.5,primitive.Lambertian{Attenuation:primitive.Color{0.5,0.4,0.3}})
 world:=&primitive.World{};world.Add(sphere)
 ok,h:=world.Hit(primitive.Ray{Origin:primitive.Vector{},Direction:primitive.Vector{Z:-2}},0.001,100)
 if !ok||math.Abs(h.T-0.25)>1e-12||math.Abs(h.Normal.Z-1)>1e-12{panic("real sphere hit")}
 // An empty World deterministically exercises the actual recursive renderer's sky branch.
 empty:=&primitive.World{}
 camera:=primitive.NewCamera(primitive.Vector{Z:1},primitive.Vector{},50,2,0)
 progress:=make(chan int,4)
 img:=render.Do(empty,camera,1,1,8,4,progress)
 if img.Bounds().Dx()!=8||img.Bounds().Dy()!=4||len(progress)!=4{panic("real renderer")}
 nonzero:=0
 for y:=0;y<4;y++{for x:=0;x<8;x++{r,g,b,a:=img.At(x,y).RGBA();if r+g+b>0&&a>0{nonzero++}}}
 if nonzero!=32{panic("sky pixels")}
 blackWorld:=&primitive.World{}
 blackWorld.Add(primitive.NewSphere(0,0,-1,0.5,primitive.Lambertian{Attenuation:primitive.Black}))
 narrow:=primitive.NewCamera(primitive.Vector{Z:1},primitive.Vector{Z:-1},5,2,0)
 shaded:=render.Do(blackWorld,narrow,1,1,4,2,make(chan int,2))
 for y:=0;y<2;y++{for x:=0;x<4;x++{r,g,b,_:=shaded.At(x,y).RGBA();if r+g+b!=0{panic("real diffuse shading")}}}
 fmt.Printf("BASELINE sphere_t=%.6f normal_z=%.6f rendered=8x4 sky_pixels=%d diffuse_pixels=8\n",h.T,h.Normal.Z,nonzero)
}
