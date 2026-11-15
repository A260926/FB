#include "model.h"

#include <fstream>
#include <sstream>
#include <deque>
#include <optional>

namespace gfx {

Model::Model(const std::string& filename) {
    std::ifstream file(filename);

    if (!file) return;

    std::string token;
    while (file >> token) {
        if (token == "v") {
            Vertex v{{0, 0, 0, 1}, Vec(4)};
            file >> v.pos[0] >> v.pos[1] >> v.pos[2];
            vertices_.push_back(std::make_shared<Vertex>(v));
        } else if (token == "f") {
            std::string line;
            std::getline(file, line);
            std::stringstream ss(line);

            std::deque<std::shared_ptr<Vertex>> face;

            auto parse_vertex = [&]() -> std::optional<std::shared_ptr<Vertex>> {
                std::string number;
                if (!(ss >> number)) return std::nullopt;
                return vertices_[std::stoi(number.substr(0, number.find('/'))) - 1];
            };

            auto base = parse_vertex().value();

            face.push_back(parse_vertex().value());

            // Load triangle fan
            while (true) {
                auto v = parse_vertex();
                if (!v) break;
                face.push_back(v.value());
                faces_.push_back({base, face.front(), face.back()});
                face.pop_front();
            }

        }
    }

    ComputeNormals();
    loaded_ = true;
}

void Model::ComputeNormals() {
    for (auto& face : faces_) {
        auto v0 = face[0], v1 = face[1], v2 = face[2];
        Vec n = cross(v1->pos - v0->pos, v2->pos - v0->pos);
        v0->normal += n; v1->normal += n; v2->normal += n;
    }
}

auto Model::ComputeBoundingBox() const -> std::tuple<Vec, Vec> {
    Vec min(std::numeric_limits<float>::max(), 3),
        max(std::numeric_limits<float>::lowest(), 3);

    for (const auto& v : vertices_) {
        for (auto i = 0; i < 3; i++) {
            min[i] = std::min(min[i], v->pos[i]);
            max[i] = std::max(max[i], v->pos[i]);
        }
    }

    return std::make_tuple(min, max);
}

}
