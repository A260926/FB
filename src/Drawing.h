#pragma once

#include <CanvasPoint.h>
#include <CanvasTriangle.h>
#include <Colour.h>
#include <Canvas.h>
#include <TextureMap.h>
#include <cstdint>

// Pack 8-bit RGB into the canvas's 0xAARRGGBB pixel format (alpha forced opaque).
uint32_t packColour(int red, int green, int blue);

// Full-screen demos.
void redNoise(Canvas &canvas);
void singleDimensionGreyscaleInterpolation(Canvas &canvas);
void twoDimensionalColourInterpolation(Canvas &canvas);

// Lines and triangles.
void drawLine(const CanvasPoint &from, const CanvasPoint &to, const Colour &colour, Canvas &canvas);
void drawStraightLines(Canvas &canvas);
void drawCanvasStrokedTriangle(CanvasTriangle triangle, const Colour &colour, Canvas &canvas);
void drawRandomTriangle(Canvas &canvas);
void drawFilledTriangle(CanvasTriangle triangle, const Colour &colour, Canvas &canvas);
void drawRandomFilledTriangle(Canvas &canvas);

// Texture mapping.
void drawTexturedLine(Canvas &canvas, CanvasPoint from, CanvasPoint to, TextureMap texture);
void drawTexturedTriangle(Canvas &canvas, CanvasTriangle triangle, TextureMap &map);
void drawTexturedTriangleExample(Canvas &canvas);

// Perspective-correct attribute interpolation (weight each vertex value by 1/z).
float perspectiveInterp(float w0, float w1, float w2, float iz0, float iz1, float iz2, float a0, float a1, float a2);
// Barycentric textured-triangle fill with perspective-correct UVs (uses depth).
void drawTexturedTrianglePerspective(Canvas &canvas, CanvasTriangle triangle, const TextureMap &map);
