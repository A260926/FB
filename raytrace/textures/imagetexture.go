package textures

import (
	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
)

type ImageTexture struct {
	pixelBuffer *raytrace.PixelBuffer
	floatWidth  float64
	floatHeight float64
}

func NewImageTexture(pixelBuffer *raytrace.PixelBuffer) raytrace.Texture {
	floatWidth := float64(pixelBuffer.Width())
	floatHeight := float64(pixelBuffer.Height())
	t := &ImageTexture{pixelBuffer, floatWidth, floatHeight}
	return t
}

func (t *ImageTexture) GetValue(uvCoords mgl64.Vec2, p mgl64.Vec3) raytrace.ColorVector {
	i := int(uvCoords.X() * t.floatWidth)
	j := int(((1.0 - uvCoords.Y()) * t.floatHeight) - 0.001)
	if i < 0 {
		i = 0
	}
	if j < 0 {
		j = 0
	}

	if i > t.pixelBuffer.Width()-1 {
		i = t.pixelBuffer.Width() - 1
	}
	if j > t.pixelBuffer.Height()-1 {
		j = t.pixelBuffer.Height() - 1
	}

	return t.pixelBuffer.GetPixelColor(i, j)
}
