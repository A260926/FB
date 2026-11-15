package raytrace_test

import (
 "fmt"
 "math"
 "testing"
 "github.com/go-gl/mathgl/mgl64"
 rt "github.com/markzuber/zgotrace/raytrace"
 "github.com/markzuber/zgotrace/raytrace/cameras"
 "github.com/markzuber/zgotrace/raytrace/hitables"
 "github.com/markzuber/zgotrace/raytrace/materials"
 "github.com/markzuber/zgotrace/raytrace/textures"
)

func TestBaseline(t *testing.T) {
 light:=materials.NewDiffuseLight(textures.NewColorTexture(rt.NewColorVector(.16,.36,.64)))
 sphere:=hitables.NewSphere(mgl64.Vec3{0,0,-3},1,light)
 world:=hitables.NewBvhNode([]rt.Hitable{sphere},0,1)
 camera:=cameras.NewBasicCamera(mgl64.Vec3{},mgl64.Vec3{-1,-1,-1},mgl64.Vec3{2,0,0},mgl64.Vec3{0,2,0})
 background:=func(*rt.Ray)rt.ColorVector{return rt.NewColorVector(.01,.04,.09)}
 cfg:=rt.NewRenderConfig(0,1,false)
 tr:=rt.NewMonteCarloTracer()
 scene:=rt.NewScene(camera,world,sphere,background)
 buf:=rt.NewPixelBuffer(4,4)
 rt.NewRenderer(cfg).Render(tr,buf,scene,cfg)
 hits:=0
 for y:=0;y<4;y++ {for x:=0;x<4;x++ {
  r:=camera.GetRay(float64(x)/4,float64(y)/4)
  hb:=world.Hit(r,.001,math.MaxFloat64);hs:=sphere.Hit(r,.001,math.MaxFloat64)
  if (hb==nil)!=(hs==nil) || (hb!=nil && math.Abs(hb.T()-hs.T())>1e-12) {t.Fatal("BVH/sphere mismatch")}
  expected:=rt.NewColorVector(.1,.2,.3)
  if hb!=nil {hits++;expected=rt.NewColorVector(.4,.6,.8)}
  c:=buf.GetPixelColor(x,y)
  if math.Abs(c.R()-expected.R())>1.0/255 || math.Abs(c.G()-expected.G())>1.0/255 || math.Abs(c.B()-expected.B())>1.0/255 {t.Fatalf("pixel %d,%d RGB mismatch",x,y)}
 }}
 if hits!=1 {t.Fatalf("unexpected geometry hit count %d",hits)}
 fmt.Printf("BASELINE Renderer pixels=16 emissive_hits=%d background=15 BVH=match\n",hits)
}
