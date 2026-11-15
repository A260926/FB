#pragma once

#include <cstdint>
#include <string>
#include <vector>

// A plain RGBA pixel buffer with no windowing/SDL dependency. Both the
// interactive app (via DrawingWindow) and the headless renderer draw into a
// Canvas; only presentation to a real window needs SDL.
class Canvas {
public:
	size_t width;
	size_t height;
	std::vector<uint32_t> pixels;

	Canvas(size_t width, size_t height);
	void setPixelColour(size_t x, size_t y, uint32_t colour);
	uint32_t getPixelColour(size_t x, size_t y) const;
	void clearPixels();
	void savePPM(const std::string &filename) const;
	void savePNG(const std::string &filename) const; // web-ready output
};
