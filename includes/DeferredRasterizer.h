#pragma once
#include "Rasterizer.h"

class DeferredRasterizer : public Rasterizer {
private:
  std::vector<RGBColor> m_normal_buffer;
  std::vector<RGBColor> m_color_buffer;
  std::vector<RGBColor> m_diffuse_buffer;
  std::vector<RGBColor> m_specular_buffer;
public:
  DeferredRasterizer();
  DeferredRasterizer(const World* world);
  ~DeferredRasterizer();

  void render(const bool use_shade = true, const bool use_shadow_maps = true) override;
  void export_output(const std::string output_path) const override;

  // GBuffer read-back (used to verify that winning section caps are written
  // into the same buffers as ordinary fragments).
  const std::vector<RGBColor>& normalBuffer() const { return m_normal_buffer; }
  const std::vector<RGBColor>& colorBuffer() const { return m_color_buffer; }
  const std::vector<RGBColor>& diffuseBuffer() const { return m_diffuse_buffer; }
  const std::vector<RGBColor>& specularBuffer() const { return m_specular_buffer; }

private:

};