#pragma once

#include <cstdint>
#include <iostream>
#include <string>

struct Colour {
	std::string name;
	int red{};
	int green{};
	int blue{};
	Colour();
	Colour(int r, int g, int b);
	Colour(std::string n, int r, int g, int b);
	uint32_t toUint32() const {
		uint8_t r = static_cast<uint8_t>(red);
		uint8_t g = static_cast<uint8_t>(green);
		uint8_t b = static_cast<uint8_t>(blue);
		return (255u << 24u) + (r << 16u) + (g << 8u) + b;
	}
};

std::ostream &operator<<(std::ostream &os, const Colour &colour);
