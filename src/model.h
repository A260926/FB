#pragma once

#include <array>
#include <vector>
#include <memory>
#include <tuple>

#include "math.h"

namespace gfx {

struct Vertex {
    Vec pos, normal;
};

typedef std::array<std::shared_ptr<Vertex>, 3> Face;

class Model {
public:
    Model(const std::string& filename);

    auto ComputeBoundingBox() const -> std::tuple<Vec, Vec>;
    const auto& get_faces() const { return faces_; }
    auto is_loaded() const { return loaded_; }

private:
    bool loaded_ = false;
    std::vector<Face> faces_;
    std::vector<std::shared_ptr<Vertex>> vertices_;

    void ComputeNormals();
};


}
