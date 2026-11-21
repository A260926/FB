#include "ImageWriter.h"
#include <fstream>
#include <iostream>

ImageWriter::ImageWriter(const std::vector<Radiance>& pixelData, int width, int height, const std::string& filename)
    : pixelData(pixelData), width(width), height(height), filename(filename) {}

// Function to validate width, height, and pixel data size
bool ImageWriter::validateParameters() const {
    if (width <= 0 || height <= 0) {
        std::cerr << "Error: Width and height must be positive." << std::endl;
        return false;
    }
    if (pixelData.size() != static_cast<size_t>(width) * static_cast<size_t>(height)) {
        std::cerr << "Error: Pixel data size does not match width and height." << std::endl;
        return false;
    }
    return true;
}

// Writes the PPM image data to a file
bool ImageWriter::writePPM() const {
    // Validate parameters before writing
    if (!validateParameters()) {
        return false;
    }

    // Open file in binary mode
    std::ofstream outFile(filename, std::ios::binary);
    if (!outFile) {
        std::cerr << "Error: Could not open file " << filename << " for writing." << std::endl;
        return false;
    }

    // Write the header
    outFile << getPPMHeader();
    // Write the pixel data in ASCII format
    for (int j = 0; j < height; j++) {
        for (int i = width - 1; i >= 0; i--) {
            size_t index = static_cast<size_t>(j) * static_cast<size_t>(width) + static_cast<size_t>(i);
            outFile << get_color_string(pixelData[index]) << '\n';
        }
    }

    // Check if data was written successfully
    if (!outFile) {
        std::cerr << "Error: Writing to file failed." << std::endl;
        return false;
    }

    outFile.close();
    return true;
}

// Generates the PPM file header
std::string ImageWriter::getPPMHeader() const {
    return "P3\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
}
