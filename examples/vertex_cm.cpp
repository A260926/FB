// Renders a small PPM using the real Camera and the VertexCM integrator.
//
// The scene contains two differently sized, differently coloured finite area
// emitters, a diffuse floor and back wall, a mirror sphere (producing a caustic
// through the mirror at a diffuse receiver) and a diffuse sphere. It exercises
// the able-to-combine normal indirect light and mirror caustics path in a single
// image.

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "../source/camera/camera.hpp"
#include "../source/common/option.hpp"
#include "../source/scene/scene.hpp"

namespace
{
    void writePPM(const std::string& path, const Image& image)
    {
        std::ofstream out(path, std::ios::binary);
        out << "P6\n" << image.width << " " << image.height << "\n255\n";
        for (size_t y = 0; y < image.height; ++y)
        {
            for (size_t x = 0; x < image.width; ++x)
            {
                glm::dvec3 c = image(x, y);
                // Simple Reinhard tonemap and sRGB-ish encode for a stable file.
                c = c / (1.0 + glm::max(c, glm::dvec3(0.0)));
                c = glm::pow(glm::clamp(c, glm::dvec3(0.0), glm::dvec3(1.0)),
                             glm::dvec3(1.0 / 2.2));
                unsigned char rgb[3] = {
                    static_cast<unsigned char>(c.r * 255.0 + 0.5),
                    static_cast<unsigned char>(c.g * 255.0 + 0.5),
                    static_cast<unsigned char>(c.b * 255.0 + 0.5)
                };
                out.write(reinterpret_cast<const char*>(rgb), 3);
            }
        }
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: " << argv[0] << " output.ppm\n";
        return 1;
    }

    // Scene: a diffuse room with a mirror sphere and two finite emitters. The
    // camera looks at the corner so both the direct light, the indirect bounce
    // and the mirror caustic are visible in the same image.
    nlohmann::json j = nlohmann::json::parse(R"({
      "num_render_threads": 1,
      "materials": {
        "default":  { "reflectance": [0.72, 0.72, 0.72] },
        "red":      { "reflectance": [0.75, 0.2, 0.2] },
        "mirror":   { "perfect_mirror": true, "reflectance": [1, 1, 1] },
        "big_lamp": { "reflectance": [0, 0, 0], "emittance": [18, 16, 12] },
        "small_lamp": { "reflectance": [0, 0, 0], "emittance": [4, 8, 20] }
      },
      "vcm": {
        "light_paths": 8192,
        "max_vertices": 8,
        "radius": 0.08,
        "alpha": 0.5,
        "iterations": 3,
        "seed": 2024,
        "mode": "combined"
      },
      "cameras": [
        {
          "focal_length": 24,
          "sensor_width": 36,
          "eye": [0, 1.6, 4.2],
          "look_at": [0, 1.0, -1.0],
          "image": { "width": 160, "height": 120, "plain": true },
          "sqrtspp": 2,
          "savename": "vertex_cm"
        }
      ],
      "surfaces": [
        { "type": "triangle", "material": "default",
          "vertices": [[-4, 0, -4], [4, 0, -4], [4, 0, 4]] },
        { "type": "triangle", "material": "default",
          "vertices": [[-4, 0, -4], [4, 0, 4], [-4, 0, 4]] },
        { "type": "triangle", "material": "red",
          "vertices": [[-4, 0, -4], [-4, 5, -4], [4, 5, -4]] },
        { "type": "triangle", "material": "red",
          "vertices": [[-4, 0, -4], [4, 5, -4], [4, 0, -4]] },
        { "type": "sphere", "material": "mirror",
          "radius": 0.85, "position": [-1.1, 0.95, -1.2] },
        { "type": "sphere", "material": "default",
          "radius": 0.7, "position": [1.2, 0.8, -1.4] },
        { "type": "triangle", "material": "big_lamp",
          "vertices": [[-2.5, 4.98, -2.5], [-0.5, 4.98, -3.0], [-0.5, 4.98, -1.5]] },
        { "type": "triangle", "material": "small_lamp",
          "vertices": [[1.0, 4.98, -3.0], [2.4, 4.98, -3.0], [1.7, 4.98, -1.7]] }
      ]
    })");

    Option option("", "example", 0, false);

    try
    {
        Camera camera(j, option);
        camera.capture();
        writePPM(argv[1], camera.image);
    }
    catch (const std::exception& ex)
    {
        std::cerr << "vertex-cm example failed: " << ex.what() << "\n";
        return 2;
    }

    std::cout << "Wrote " << argv[1] << "\n";
    return 0;
}
