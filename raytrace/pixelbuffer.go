package raytrace

import (
	"image"
	"image/draw"
	"image/jpeg"
	"image/png"
	"log"
	"os"
	"path/filepath"
)

type PixelBuffer struct {
	width  int
	height int
	isYUp  bool
	img    *image.RGBA
}

func NewPixelBuffer(width int, height int) *PixelBuffer {
	img := image.NewRGBA(image.Rect(0, 0, width, height))
	return &PixelBuffer{width, height, true, img}
}

func LoadPixelBuffer(imagePath string) *PixelBuffer {

	f, _ := os.Open(imagePath)
	defer f.Close()

	var img image.Image
	var err error

	if filepath.Ext(imagePath) == ".jpg" {
		img, err = jpeg.Decode(f)
		if err != nil {
			log.Fatal(err)
		}
	} else if filepath.Ext(imagePath) == ".png" {
		img, err = jpeg.Decode(f)
		if err != nil {
			log.Fatal(err)
		}
	} else {
		log.Fatal("Unknown file extension")
	}

	rgba := image.NewRGBA(img.Bounds())
	draw.Draw(rgba, rgba.Bounds(), img, image.Point{0, 0}, draw.Src)

	return &PixelBuffer{img.Bounds().Max.X, img.Bounds().Max.Y, false, rgba}
}

func (buf *PixelBuffer) Width() int {
	return buf.width
}

func (buf *PixelBuffer) Height() int {
	return buf.height
}

func (buf *PixelBuffer) getProperY(y int) int {
	if buf.isYUp {
		y = buf.height - 1 - y
	}
	return y
}

func (buf *PixelBuffer) SetPixelColor(x int, y int, clr ColorVector) {
	// Our Y axis is UP (right handed coordinate system)
	// X is right, and positive Z is out of the screen towards
	// the viewer.  So our calculated Y pixels are
	// the opposite direction of the Y in the image buffer.
	// If IsYUp is true then we'll invert Y when setting it into
	// the image.
	buf.img.Set(x, buf.getProperY(y), clr)
}

func (buf *PixelBuffer) GetPixelColor(x int, y int) ColorVector {
	clr := buf.img.At(x, buf.getProperY(y))
	return NewColorVectorFromRGBA(clr.RGBA())
}

func (buf *PixelBuffer) GetImage() *image.RGBA {
	return buf.img
}

func (buf *PixelBuffer) SavePng(path string) {
	f, err := os.Create(path)
	if err != nil {
		log.Fatal(err)
	}

	if err := png.Encode(f, buf.img); err != nil {
		f.Close()
		log.Fatal(err)
	}

	if err := f.Close(); err != nil {
		log.Fatal(err)
	}
}
