package main

import (
	"fmt"
	"image/png"
	"os"
)

func main() {
	if len(os.Args) != 2 {
		panic("PNG path required")
	}
	f, e := os.Open(os.Args[1])
	if e != nil {
		panic(e)
	}
	defer f.Close()
	im, e := png.Decode(f)
	if e != nil {
		panic(e)
	}
	b := im.Bounds()
	if b.Dx() != 96 || b.Dy() != 48 {
		panic("expected 96x48 image")
	}
	different := 0
	for y := 0; y < 48; y++ {
		for x := 0; x < 48; x++ {
			r, g, bb, a := im.At(x, y).RGBA()
			r1, g1, b1, a1 := im.At(x+48, y).RGBA()
			if r != r1 || g != g1 || bb != b1 || a != a1 {
				different++
			}
		}
	}
	if different == 0 {
		panic("comparison panels identical")
	}
	fmt.Println("ANALYTIC_IMAGE_OK", different)
}
