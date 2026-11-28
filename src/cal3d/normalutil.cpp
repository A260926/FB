#include <cal3d/normalutil.h>
#include <cal3d/coresubmesh.h>

#include <normal_encoding/normal_encoding.hpp>
#include <cstring>

std::string CalNormalUtil::encodeMeshNormals(const ISTD::shared_ptr<CalCoreSubmesh>& mesh) {
    const auto& vertices = mesh->getVectorVertex();
    cal3d::SSEArray<CalVector4> normals(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
        normals[i] = vertices[i].normal;
    }
    auto data = encode(normals);
    std::string s;
    for (size_t i = 0; i < data.size(); ++i) {
        uint16_t value = data[i];
        unsigned char lowByte = value & 0xff;
        unsigned char highByte = (value >> 8) & 0xff;
        s += static_cast<char>(lowByte);
        s += static_cast<char>(highByte);
    }
    return s;
}

ISTD::shared_ptr<CalCoreSubmesh> CalNormalUtil::decodeMeshNormals(const std::string& s) {
    std::vector<uint16_t> data;
    size_t count = s.size() / 2;
    for (size_t i = 0 ; i < count; ++i) {
        unsigned char lowByte = s[i*2];
        unsigned char highByte = s[i*2 + 1];
        uint16_t value = (static_cast<uint16_t>(highByte) << 8) | static_cast<uint16_t>(lowByte);
        data.push_back(value);
    }
    auto normals = decode(data);
    ISTD::shared_ptr<CalCoreSubmesh> mesh = ISTD::make_shared<CalCoreSubmesh>(normals.size(), false, 0);
    for (size_t i = 0; i < normals.size(); ++i) {
        CalCoreSubmesh::Vertex vertex;
        vertex.normal = normals[i];
        mesh->addVertex(vertex, CalColor32(), CalCoreSubmesh::InfluenceVector());
    }
    return mesh;
}

std::vector<uint16_t> CalNormalUtil::encode(const cal3d::SSEArray<CalVector4>& normals) {
    std::vector<uint16_t> data;
    data.reserve(normals.size());

    for (auto it = normals.begin(); it != normals.end(); ++it) {
        // TODO: perf - this may run faster if we rewrite normal_encoding to
        // use CalVector4.

        // NOTE: Sanity-checking/normalization may no longer be necessary now that
        // mesh endpoint is doing a normalization pass.
        float length = it->length();
        if (length > 0.001f) {
            float3 normal;
            normal.x = it->x / length;
            normal.y = it->y / length;
            normal.z = it->z / length;
            data.push_back(::encode(normal));
        } else {
            data.push_back(0);
        }
    }

    return data;
}

cal3d::SSEArray<CalVector4> CalNormalUtil::decode(const std::vector<uint16_t>& data) {
    cal3d::SSEArray<CalVector4> normals;
    //normals.reserve(data.size()); // Not supported in SSEArray.

    for (auto it = data.begin(); it != data.end(); ++it) {
        // TODO: perf - this may run faster if we rewrite normal_encoding to
        // use CalVector4.
        float3 normal = ::decode(*it);
        normals.push_back(CalVector4(normal.x, normal.y, normal.z));
    }

    return normals;
}
