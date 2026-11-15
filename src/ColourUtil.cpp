#include "ColourUtil.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>

glm::vec3 rgbToHsv(glm::vec3 rgb) {
	float r = rgb.r;
	float g = rgb.g;
	float b = rgb.b;

	float maxC = std::max({r, g, b});
	float minC = std::min({r, g, b});
	float delta = maxC - minC;

	float h = 0.0f;
	if (delta > 0.0f) {
		if (maxC == r) {
			h = 60.0f * std::fmod((g - b) / delta, 6.0f);
		} else if (maxC == g) {
			h = 60.0f * ((b - r) / delta + 2.0f);
		} else {
			h = 60.0f * ((r - g) / delta + 4.0f);
		}
	}
	if (h < 0.0f) {
		h += 360.0f;
	}

	float s = (maxC > 0.0f) ? (delta / maxC) : 0.0f;
	float v = maxC;

	return glm::vec3(h, s, v);
}

glm::vec3 hsvToRgb(glm::vec3 hsv) {
	float h = hsv.x;
	float s = hsv.y;
	float v = hsv.z;

	// Wrap hue into [0, 360).
	h = std::fmod(h, 360.0f);
	if (h < 0.0f) {
		h += 360.0f;
	}

	float c = v * s;
	float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
	float m = v - c;

	float r = 0.0f;
	float g = 0.0f;
	float b = 0.0f;
	if (h < 60.0f) {
		r = c;
		g = x;
	} else if (h < 120.0f) {
		r = x;
		g = c;
	} else if (h < 180.0f) {
		g = c;
		b = x;
	} else if (h < 240.0f) {
		g = x;
		b = c;
	} else if (h < 300.0f) {
		r = x;
		b = c;
	} else {
		r = c;
		b = x;
	}

	return glm::vec3(r + m, g + m, b + m);
}

namespace {

struct RGB8 {
	uint8_t r;
	uint8_t g;
	uint8_t b;
};

inline RGB8 unpack(uint32_t pixel) {
	RGB8 c;
	c.r = static_cast<uint8_t>((pixel >> 16u) & 0xFFu);
	c.g = static_cast<uint8_t>((pixel >> 8u) & 0xFFu);
	c.b = static_cast<uint8_t>(pixel & 0xFFu);
	return c;
}

struct Box {
	std::vector<RGB8> colours;

	// Return the channel (0=r,1=g,2=b) with the largest value range.
	int longestAxis(int &outRange) const {
		std::array<uint8_t, 3> minC = {255, 255, 255};
		std::array<uint8_t, 3> maxC = {0, 0, 0};
		for (const RGB8 &c : colours) {
			minC[0] = std::min(minC[0], c.r);
			minC[1] = std::min(minC[1], c.g);
			minC[2] = std::min(minC[2], c.b);
			maxC[0] = std::max(maxC[0], c.r);
			maxC[1] = std::max(maxC[1], c.g);
			maxC[2] = std::max(maxC[2], c.b);
		}
		int rangeR = static_cast<int>(maxC[0]) - static_cast<int>(minC[0]);
		int rangeG = static_cast<int>(maxC[1]) - static_cast<int>(minC[1]);
		int rangeB = static_cast<int>(maxC[2]) - static_cast<int>(minC[2]);
		int axis = 0;
		int best = rangeR;
		if (rangeG > best) {
			axis = 1;
			best = rangeG;
		}
		if (rangeB > best) {
			axis = 2;
			best = rangeB;
		}
		outRange = best;
		return axis;
	}

	Colour average() const {
		if (colours.empty()) {
			return Colour(0, 0, 0);
		}
		unsigned long long sr = 0;
		unsigned long long sg = 0;
		unsigned long long sb = 0;
		for (const RGB8 &c : colours) {
			sr += c.r;
			sg += c.g;
			sb += c.b;
		}
		auto n = static_cast<unsigned long long>(colours.size());
		return Colour(static_cast<int>(sr / n), static_cast<int>(sg / n), static_cast<int>(sb / n));
	}
};

inline uint8_t channel(const RGB8 &c, int axis) {
	if (axis == 0) {
		return c.r;
	}
	if (axis == 1) {
		return c.g;
	}
	return c.b;
}

} // namespace

std::vector<Colour> buildPalette(const std::vector<uint32_t> &pixels, int paletteSize) {
	std::vector<Colour> palette;
	if (paletteSize <= 0 || pixels.empty()) {
		return palette;
	}

	Box initial;
	initial.colours.reserve(pixels.size());
	for (uint32_t p : pixels) {
		initial.colours.push_back(unpack(p));
	}

	std::vector<Box> boxes;
	boxes.push_back(std::move(initial));

	// Repeatedly split the box with the largest colour range until we reach the
	// requested number of boxes or nothing more can be split.
	while (static_cast<int>(boxes.size()) < paletteSize) {
		int targetIdx = -1;
		int targetRange = -1;
		for (size_t i = 0; i < boxes.size(); ++i) {
			if (boxes[i].colours.size() < 2) {
				continue;
			}
			int range = 0;
			boxes[i].longestAxis(range);
			if (range > targetRange) {
				targetRange = range;
				targetIdx = static_cast<int>(i);
			}
		}

		if (targetIdx < 0 || targetRange <= 0) {
			break; // Nothing left worth splitting.
		}

		Box &box = boxes[static_cast<size_t>(targetIdx)];
		int axis = 0;
		int range = 0;
		axis = box.longestAxis(range);

		std::sort(box.colours.begin(), box.colours.end(),
		          [axis](const RGB8 &a, const RGB8 &b) { return channel(a, axis) < channel(b, axis); });

		size_t mid = box.colours.size() / 2;
		Box lower;
		Box upper;
		lower.colours.assign(box.colours.begin(), box.colours.begin() + static_cast<std::ptrdiff_t>(mid));
		upper.colours.assign(box.colours.begin() + static_cast<std::ptrdiff_t>(mid), box.colours.end());

		boxes[static_cast<size_t>(targetIdx)] = std::move(lower);
		boxes.push_back(std::move(upper));
	}

	palette.reserve(boxes.size());
	for (const Box &box : boxes) {
		if (!box.colours.empty()) {
			palette.push_back(box.average());
		}
	}
	return palette;
}

std::vector<uint8_t> quantizeToPalette(const std::vector<uint32_t> &pixels, const std::vector<Colour> &palette) {
	std::vector<uint8_t> indices(pixels.size(), 0);
	if (palette.empty()) {
		return indices;
	}

	for (size_t i = 0; i < pixels.size(); ++i) {
		RGB8 c = unpack(pixels[i]);
		long bestDist = std::numeric_limits<long>::max();
		size_t bestIdx = 0;
		for (size_t p = 0; p < palette.size(); ++p) {
			long dr = static_cast<long>(c.r) - palette[p].red;
			long dg = static_cast<long>(c.g) - palette[p].green;
			long db = static_cast<long>(c.b) - palette[p].blue;
			long dist = dr * dr + dg * dg + db * db;
			if (dist < bestDist) {
				bestDist = dist;
				bestIdx = p;
			}
		}
		indices[i] = static_cast<uint8_t>(bestIdx);
	}
	return indices;
}

void savePalettePPM(const char *path, int width, int height, const std::vector<uint8_t> &indices,
                    const std::vector<Colour> &palette) {
	if (path == nullptr || width <= 0 || height <= 0) {
		return;
	}

	std::ofstream out(path, std::ios::out | std::ios::binary);
	if (!out) {
		return;
	}

	out << "P6\n" << width << ' ' << height << "\n255\n";

	const size_t total = static_cast<size_t>(width) * static_cast<size_t>(height);
	for (size_t i = 0; i < total; ++i) {
		uint8_t r = 0;
		uint8_t g = 0;
		uint8_t b = 0;
		if (i < indices.size()) {
			uint8_t idx = indices[i];
			if (idx < palette.size()) {
				const Colour &col = palette[idx];
				r = static_cast<uint8_t>(std::clamp(col.red, 0, 255));
				g = static_cast<uint8_t>(std::clamp(col.green, 0, 255));
				b = static_cast<uint8_t>(std::clamp(col.blue, 0, 255));
			}
		}
		char rgb[3] = {static_cast<char>(r), static_cast<char>(g), static_cast<char>(b)};
		out.write(rgb, 3);
	}
}
