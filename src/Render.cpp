#include <base/Render.hpp>
#include <fstream>
#ifdef SOFT_RASTERIZER_OPENCV
#include <opencv2/highgui.hpp>
#endif
SoftRasterizer::RenderingPipeline::RenderingPipeline(std::size_t width,
                                                     std::size_t height)
    : m_width(width), m_height(height) {
  // Fixed-point coverage uses eight subpixel bits; bound arithmetic and
  // allocations.
  if (!width || !height || width > 16384 || height > 16384) {
    throw std::invalid_argument("resolution must be in [1,16384]");
  }
  m_color.resize(width * height);
  m_depth.resize(width * height);
  clear();
}

bool SoftRasterizer::RenderingPipeline::addScene(std::shared_ptr<Scene> scene) {
  if (!scene ||
      std::find(m_scenes.begin(), m_scenes.end(), scene) != m_scenes.end()) {
    return false;
  }
  m_scenes.push_back(std::move(scene));
  return true;
}

void SoftRasterizer::RenderingPipeline::clear(Buffers flags) {
  if (static_cast<int>(flags) & static_cast<int>(Buffers::Color)) {
    std::fill(m_color.begin(), m_color.end(), glm::vec3(0));
  }
  if (static_cast<int>(flags) & static_cast<int>(Buffers::Depth)) {
    std::fill(m_depth.begin(), m_depth.end(),
              std::numeric_limits<float>::infinity());
  }
}

void SoftRasterizer::RenderingPipeline::save(const std::string &path,
                                             float exposure) const {
  if (!std::isfinite(exposure) || exposure <= 0) {
    throw std::invalid_argument("invalid exposure");
  }
  std::ofstream outputFile(path, std::ios::binary);
  if (!outputFile) {
    throw std::runtime_error("Cannot write " + path);
  }
  outputFile << "P6\n" << m_width << ' ' << m_height << "\n255\n";
  for (const auto &pixelColor : m_color) {
    for (int c = 0; c < 3; ++c) {
      float channelValue =
          std::isfinite(pixelColor[c])
              ? std::clamp(linearToSrgb(pixelColor[c] * exposure), 0.f, 1.f)
              : 0;
      outputFile.put(static_cast<char>(
          static_cast<unsigned char>(std::lround(channelValue * 255))));
    }
  }
  if (!outputFile) {
    throw std::runtime_error("Failed to write " + path);
  }
}

void SoftRasterizer::RenderingPipeline::display(Primitive type) {
  draw(type);
#ifdef SOFT_RASTERIZER_OPENCV
  cv::Mat image(int(m_height), int(m_width), CV_8UC3);
  for (std::size_t y = 0; y < m_height; ++y) {
    for (std::size_t x = 0; x < m_width; ++x) {
      for (int c = 0; c < 3; ++c) {
        image.at<cv::Vec3b>(int(y), int(x))[2 - c] =
            static_cast<unsigned char>(std::lround(
                255 * std::clamp(linearToSrgb(m_color[y * m_width + x][c]), 0.f,
                                 1.f)));
      }
    }
  }
  cv::imshow("Software Rasterizer", image);
  cv::waitKey(0);
#else
  throw std::runtime_error("Display needs SOFT_RASTERIZER_ENABLE_GUI; use "
                           "save() for headless rendering");
#endif
}
