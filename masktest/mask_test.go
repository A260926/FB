package masktest_test
import("image";"image/color";"testing"; . "github.com/fogleman/gg")
func testMask(shift bool) *image.Alpha {if !shift {m:=image.NewAlpha(image.Rect(0,0,2,2));for i:=range m.Pix{m.Pix[i]=255};return m};p:=image.NewAlpha(image.Rect(10,20,16,24));for i:=range p.Pix{p.Pix[i]=77};m:=p.SubImage(image.Rect(12,21,14,23)).(*image.Alpha);for y:=21;y<23;y++{for x:=12;x<14;x++{m.SetAlpha(x,y,color.Alpha{255})}};return m}
func testFill(m *image.Alpha)*Context{d:=NewContext(2,2);if err:=d.SetMask(m);err!=nil{panic(err)};d.SetRGBA(1,0,0,1);d.DrawRectangle(0,0,2,2);d.Fill();return d}
func TestBaseline(t *testing.T){d:=testFill(testMask(false));if got:=d.Image().(*image.RGBA).RGBAAt(0,0).A;got!=255{t.Fatal(got)};t.Log("PASS zero-origin mask alpha=255")}
func TestMaskOrigin(t *testing.T){d:=testFill(testMask(true));if got:=d.Image().(*image.RGBA).RGBAAt(0,0).A;got!=255{t.Fatalf("shifted subimage alpha=%d want=255",got)}}
func TestMaskPadding(t *testing.T){m:=testMask(true);before:=append([]byte(nil),m.Pix...);d:=NewContext(2,2);if err:=d.SetMask(m);err!=nil{t.Fatal(err)};d.InvertMask();for i,v:=range m.Pix{active:=i<2||i>=m.Stride&&i<m.Stride+2;if !active&&v!=before[i]{t.Fatalf("outside active mask byte %d changed %d->%d",i,before[i],v)}}}
