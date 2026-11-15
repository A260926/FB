#pragma once

#include <cstdint>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <vector>
#include "Utils.h"

class TextureMap {
public:
	size_t width;
	size_t height;
	std::vector<uint32_t> pixels;

	TextureMap();
	TextureMap(const std::string &filename);
	friend std::ostream &operator<<(std::ostream &os, const TextureMap &point);
};
