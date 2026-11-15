#include "Lines.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

// True when (x, y) lies inside the canvas bounds.
bool inBounds(const Canvas &c, int x, int y) {
	return x >= 0 && y >= 0 && static_cast<size_t>(x) < c.width && static_cast<size_t>(y) < c.height;
}

// Blend `col` over the existing canvas pixel at (x, y) using `coverage` in
// [0, 1] as the alpha of `col`, writing the result back. coverage == 1 fully
// replaces the pixel; coverage == 0 leaves it untouched.
void blendPixel(Canvas &c, int x, int y, Colour col, float coverage) {
	if (!inBounds(c, x, y)) {
		return;
	}
	coverage = std::clamp(coverage, 0.0f, 1.0f);

	uint32_t existing = c.getPixelColour(static_cast<size_t>(x), static_cast<size_t>(y));
	auto er = static_cast<float>((existing >> 16u) & 0xFFu);
	auto eg = static_cast<float>((existing >> 8u) & 0xFFu);
	auto eb = static_cast<float>(existing & 0xFFu);

	float sr = static_cast<float>(col.red);
	float sg = static_cast<float>(col.green);
	float sb = static_cast<float>(col.blue);

	auto mix = [coverage](float dst, float src) { return dst + (src - dst) * coverage; };
	auto r = static_cast<uint8_t>(std::lround(mix(er, sr)));
	auto g = static_cast<uint8_t>(std::lround(mix(eg, sg)));
	auto b = static_cast<uint8_t>(std::lround(mix(eb, sb)));

	uint32_t packed =
	    (255u << 24u) | (static_cast<uint32_t>(r) << 16u) | (static_cast<uint32_t>(g) << 8u) | static_cast<uint32_t>(b);
	c.setPixelColour(static_cast<size_t>(x), static_cast<size_t>(y), packed);
}

// Fractional part helpers used by Wu's algorithm.
float fpart(float x) {
	return x - std::floor(x);
}
float rfpart(float x) {
	return 1.0f - fpart(x);
}

} // namespace

void bresenhamLine(Canvas &c, int x0, int y0, int x1, int y1, Colour col) {
	uint32_t packed = col.toUint32();

	int dx = std::abs(x1 - x0);
	int dy = -std::abs(y1 - y0);
	int sx = x0 < x1 ? 1 : -1;
	int sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;

	while (true) {
		if (inBounds(c, x0, y0)) {
			c.setPixelColour(static_cast<size_t>(x0), static_cast<size_t>(y0), packed);
		}
		if (x0 == x1 && y0 == y1) {
			break;
		}
		int e2 = 2 * err;
		if (e2 >= dy) {
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx) {
			err += dx;
			y0 += sy;
		}
	}
}

void wuLine(Canvas &c, float x0, float y0, float x1, float y1, Colour col) {
	bool steep = std::abs(y1 - y0) > std::abs(x1 - x0);
	if (steep) {
		std::swap(x0, y0);
		std::swap(x1, y1);
	}
	if (x0 > x1) {
		std::swap(x0, x1);
		std::swap(y0, y1);
	}

	float dx = x1 - x0;
	float dy = y1 - y0;
	float gradient = (dx == 0.0f) ? 1.0f : dy / dx;

	auto plot = [&](int x, int y, float coverage) {
		if (steep) {
			blendPixel(c, y, x, col, coverage);
		} else {
			blendPixel(c, x, y, col, coverage);
		}
	};

	// First endpoint.
	float xend = std::round(x0);
	float yend = y0 + gradient * (xend - x0);
	float xgap = rfpart(x0 + 0.5f);
	int xpxl1 = static_cast<int>(xend);
	int ypxl1 = static_cast<int>(std::floor(yend));
	plot(xpxl1, ypxl1, rfpart(yend) * xgap);
	plot(xpxl1, ypxl1 + 1, fpart(yend) * xgap);
	float intery = yend + gradient;

	// Second endpoint.
	xend = std::round(x1);
	yend = y1 + gradient * (xend - x1);
	xgap = fpart(x1 + 0.5f);
	int xpxl2 = static_cast<int>(xend);
	int ypxl2 = static_cast<int>(std::floor(yend));
	plot(xpxl2, ypxl2, rfpart(yend) * xgap);
	plot(xpxl2, ypxl2 + 1, fpart(yend) * xgap);

	// Main span.
	for (int x = xpxl1 + 1; x < xpxl2; ++x) {
		int y = static_cast<int>(std::floor(intery));
		plot(x, y, rfpart(intery));
		plot(x, y + 1, fpart(intery));
		intery += gradient;
	}
}

namespace {

constexpr int kInside = 0;
constexpr int kLeft = 1;
constexpr int kRight = 2;
constexpr int kBottom = 4;
constexpr int kTop = 8;

int computeOutcode(float x, float y, float xmin, float ymin, float xmax, float ymax) {
	int code = kInside;
	if (x < xmin) {
		code |= kLeft;
	} else if (x > xmax) {
		code |= kRight;
	}
	if (y < ymin) {
		code |= kBottom;
	} else if (y > ymax) {
		code |= kTop;
	}
	return code;
}

} // namespace

bool cohenSutherlandClip(float &x0, float &y0, float &x1, float &y1, float xmin, float ymin, float xmax, float ymax) {
	int code0 = computeOutcode(x0, y0, xmin, ymin, xmax, ymax);
	int code1 = computeOutcode(x1, y1, xmin, ymin, xmax, ymax);

	while (true) {
		if ((code0 | code1) == 0) {
			// Both endpoints inside the rectangle.
			return true;
		}
		if ((code0 & code1) != 0) {
			// Both share an outside zone: trivially rejected.
			return false;
		}

		int codeOut = (code0 != 0) ? code0 : code1;
		float x = 0.0f;
		float y = 0.0f;

		if (codeOut & kTop) {
			x = x0 + (x1 - x0) * (ymax - y0) / (y1 - y0);
			y = ymax;
		} else if (codeOut & kBottom) {
			x = x0 + (x1 - x0) * (ymin - y0) / (y1 - y0);
			y = ymin;
		} else if (codeOut & kRight) {
			y = y0 + (y1 - y0) * (xmax - x0) / (x1 - x0);
			x = xmax;
		} else { // kLeft
			y = y0 + (y1 - y0) * (xmin - x0) / (x1 - x0);
			x = xmin;
		}

		if (codeOut == code0) {
			x0 = x;
			y0 = y;
			code0 = computeOutcode(x0, y0, xmin, ymin, xmax, ymax);
		} else {
			x1 = x;
			y1 = y;
			code1 = computeOutcode(x1, y1, xmin, ymin, xmax, ymax);
		}
	}
}
