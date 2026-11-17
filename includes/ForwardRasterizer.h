#pragma once

#include "Rasterizer.h"

class ForwardRasterizer : public Rasterizer {
public:
  ForwardRasterizer();
  ForwardRasterizer(const World* world);
  ~ForwardRasterizer();

  void render(const bool use_shade = true, const bool use_shadow_maps = true) override;
  void export_output(const std::string output_path) const override;
};

