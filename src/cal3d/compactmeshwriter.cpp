#include "cal3d/compactmeshwriter.h"

#include "cal3d/coremorphtarget.h"
#include "cal3d/coresubmesh.h"
#include "cal3d/compactutil.h"
#include "cal3d/normalutil.h"
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>

typedef rapidjson::GenericStringBuffer<rapidjson::UTF8<>> JsonBuffer;
typedef rapidjson::Writer<JsonBuffer> JsonWriter;

class BinaryWriter {
public:
    void appendString(const std::string& s) {
        data += s;
    }

    void appendUInt32(uint32_t i) {
        data += static_cast<char>(i & 0xff);
        data += static_cast<char>((i >> 8) & 0xff);
        data += static_cast<char>((i >> 16) & 0xff);
        data += static_cast<char>((i >> 24) & 0xff);
    }

    void appendUInt8(uint8_t i) {
        data += static_cast<char>(i);
    }

    void appendFloats(const std::vector<float>& floats) {
        for (auto it = floats.begin(); it != floats.end(); ++it) {
            float f = *it;
            // TODO: Endianness?
            data.append(reinterpret_cast<const char *>(&f), sizeof(float));
        }
    }

    void appendUInt16s(const std::vector<uint16_t>& shorts) {
        for (auto it = shorts.begin(); it != shorts.end(); ++it) {
            data += static_cast<char>((*it) & 255);
            data += static_cast<char>(((*it) >> 8) & 255);
        }
    }

    void appendUInt8s(const std::vector<uint8_t>& chars) {
        for (auto it = chars.begin(); it != chars.end(); ++it) {
            data += static_cast<char>(*it);
        }
    }

    std::string data;
};

struct XExtractor {
    float operator()(const CalCoreSubmesh::Vertex& v) { return v.position.x; }
};
struct YExtractor {
    float operator()(const CalCoreSubmesh::Vertex& v) { return v.position.y; }
};
struct ZExtractor {
    float operator()(const CalCoreSubmesh::Vertex& v) { return v.position.z; }
};

template <typename CoordExtractor>
struct PositionExtractor {
    CalQuantizedData operator()(const CalCoreSubmesh& submesh) {
        std::vector<float> values;
        values.reserve(submesh.getVertexCount());
        CoordExtractor coordExtractor;
        const auto& vertices = submesh.getVectorVertex();
        for (auto it = vertices.begin(); it != vertices.end(); ++it) {
            values.push_back(coordExtractor(*it));
        }

        // We quantize position coordinates to 40 microns, since this yields
        // similar z-sorting to the original coordinates under the following
        // assumptions:
        //   - the client's z near-plane/far-plane settings (0.5, 10000)
        //   - a 24-bit depth buffer
        //   - a maximum "detail" viewing distance of about 18 meters
        //   - mostly camera-facing polygons
        //
        // http://www.sjbaker.org/steve/omniv/love_your_z_buffer.html
        //
        return CalCompactUtil::quantizeConsistent(values, 0.000040f);
    }
};

struct UExtractor {
    float operator()(const CalCoreSubmesh::TextureCoordinate& uv) { return uv.u; }
};
struct VExtractor {
    float operator()(const CalCoreSubmesh::TextureCoordinate& uv) { return uv.v; }
};

template <typename CoordExtractor>
struct TexCoordExtractor {
    CalQuantizedData operator()(const CalCoreSubmesh& submesh) {
        const auto& texCoords = submesh.getTextureCoordinates();
        std::vector<float> values;
        values.reserve(texCoords.size());
        CoordExtractor coordExtractor;
        for (auto it = texCoords.begin(); it != texCoords.end(); ++it) {
            values.push_back(coordExtractor(*it));
        }
        return CalCompactUtil::quantizeToBits(values, 14);
    }
};

struct MorphPXExtractor {
    float operator()(const VertexOffset& vo) { return vo.position.x; }
};

struct MorphPYExtractor {
    float operator()(const VertexOffset& vo) { return vo.position.y; }
};

struct MorphPZExtractor {
    float operator()(const VertexOffset& vo) { return vo.position.z; }
};

struct MorphNXExtractor {
    float operator()(const VertexOffset& vo) { return vo.normal.x; }
};

struct MorphNYExtractor {
    float operator()(const VertexOffset& vo) { return vo.normal.y; }
};

struct MorphNZExtractor {
    float operator()(const VertexOffset& vo) { return vo.normal.z; }
};

template <typename CoordExtractor>
struct MorphExtractor {
    CalQuantizedData operator()(const cal3d::SSEArray<VertexOffset>& vertexOffsets) {
        // TODO - perf: can precompute and reserve count?
        std::vector<float> values;
        CoordExtractor coordExtractor;
        for (auto it = vertexOffsets.begin(); it != vertexOffsets.end(); ++it) {
            values.push_back(coordExtractor(*it));
        }
        return CalCompactUtil::quantizeToPrecision(values, 0.001f);
    }
};

struct VertexOffsetIdComparator {
    bool operator()(const VertexOffset& vo1, const VertexOffset& vo2) {
        return vo1.vertexId < vo2.vertexId;
    }
};

void s_writeOffsetLength(
    JsonWriter& jsonWriter,
    unsigned int offset,
    unsigned int length
) {
    jsonWriter.String("offset");
    jsonWriter.Uint(offset);
    jsonWriter.String("length");
    jsonWriter.Uint(length);
}

template<typename Extractor, typename DataSource>
static void s_writeCoords(
    BinaryWriter& binaryWriter,
    JsonWriter& jsonWriter,
    const DataSource& dataSource,
    const char* coordName
) {
    size_t dataOffset = binaryWriter.data.size();

    Extractor extractor;
    auto quantizedData = extractor(dataSource);

    // TODO - perf: use Andy's template-based method here to cut down on copying?
    auto outputData = CalCompactUtil::encodeMidi(CalCompactUtil::encodeDeltaZigzag(quantizedData.values));
    binaryWriter.appendString(outputData);

    jsonWriter.String(coordName);
    jsonWriter.StartObject();

    s_writeOffsetLength(jsonWriter, dataOffset, binaryWriter.data.size() - dataOffset);

    jsonWriter.String("minimum");
    jsonWriter.Double(quantizedData.minimum);
    jsonWriter.String("maximum");
    jsonWriter.Double(quantizedData.maximum);
    jsonWriter.String("maxQuantizedValue");
    jsonWriter.Uint(quantizedData.maxQuantizedValue);

    jsonWriter.EndObject();
}

const char* s_morphTypeToString(CalMorphTargetType morphTargetType) {
    switch (morphTargetType) {
        case CalMorphTargetTypeExclusive:
            return "Exclusive";
        case CalMorphTargetTypeAverage:
            return "Average";
        case CalMorphTargetTypeAdditive:
            return "Additive";
        case CalMorphTargetTypeClamped:
            return "Clamped";
        default:
            throw std::runtime_error("unrecognized morph target type");
    }
}

static void s_writeIndices(
    BinaryWriter& binaryWriter,
    JsonWriter& jsonWriter,
    const CalCoreSubmesh& submesh
) {
    size_t dataOffset = binaryWriter.data.size();

    const auto& faces = submesh.getFaces();
    std::vector<uint16_t> indices;
    indices.reserve(faces.size() * 3);
    for (auto it = faces.begin(); it != faces.end(); ++it) {
        indices.push_back(it->vertexId[0]);
        indices.push_back(it->vertexId[1]);
        indices.push_back(it->vertexId[2]);
    }
    auto outputData = CalCompactUtil::encodeMidi(CalCompactUtil::encodeHighWaterMark(indices));
    binaryWriter.appendString(outputData);

    jsonWriter.String("indices");
    jsonWriter.StartObject();

    jsonWriter.String("count");
    jsonWriter.Uint(indices.size());

    s_writeOffsetLength(jsonWriter, dataOffset, binaryWriter.data.size() - dataOffset);

    jsonWriter.EndObject();
}

static void s_writePositions(
    BinaryWriter& binaryWriter,
    JsonWriter& jsonWriter,
    const CalCoreSubmesh& submesh
) {
    jsonWriter.String("positions");
    jsonWriter.StartObject();

    jsonWriter.String("count");
    jsonWriter.Uint(submesh.getVertexCount());

    // TODO - perf: May be faster to pre-deinterleave?
    // TODO: Might be able to rewrite this into a single pass.
    s_writeCoords<PositionExtractor<XExtractor> >(binaryWriter, jsonWriter, submesh, "x");
    s_writeCoords<PositionExtractor<YExtractor> >(binaryWriter, jsonWriter, submesh, "y");
    s_writeCoords<PositionExtractor<ZExtractor> >(binaryWriter, jsonWriter, submesh, "z");

    jsonWriter.EndObject();
}

static void s_writeTexCoords(
    BinaryWriter& binaryWriter,
    JsonWriter& jsonWriter,
    const CalCoreSubmesh& submesh
) {
    jsonWriter.String("texCoords");
    jsonWriter.StartObject();

    const auto& texCoords = submesh.getTextureCoordinates();
    jsonWriter.String("count");
    jsonWriter.Uint(texCoords.size());

    if (!texCoords.empty()) {
        // TODO: Might be able to rewrite this into a single pass.
        s_writeCoords<TexCoordExtractor<UExtractor> >(binaryWriter, jsonWriter, submesh, "u");
        s_writeCoords<TexCoordExtractor<VExtractor> >(binaryWriter, jsonWriter, submesh, "v");
    }

    jsonWriter.EndObject();
}

static void s_writeNormals(
    BinaryWriter& binaryWriter,
    JsonWriter& jsonWriter,
    const CalCoreSubmesh& submesh
) {
    jsonWriter.String("normals");
    jsonWriter.StartObject();

    size_t dataOffset = binaryWriter.data.size();

    const auto& vertices = submesh.getVectorVertex();
    cal3d::SSEArray<CalVector4> normals(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
        normals[i] = vertices[i].normal;
    }
    binaryWriter.appendUInt16s(CalNormalUtil::encode(normals));

    s_writeOffsetLength(jsonWriter, dataOffset, binaryWriter.data.size() - dataOffset);

    jsonWriter.EndObject();
}

static void s_writeColors(
    BinaryWriter& binaryWriter,
    JsonWriter& jsonWriter,
    const CalCoreSubmesh& submesh
) {
    jsonWriter.String("colors");
    jsonWriter.StartObject();

    size_t dataOffset = binaryWriter.data.size();

    const auto& colors = submesh.getVertexColors();
    for (size_t i = 0; i < colors.size(); ++i) {
        CalColor32 color = colors[i];
        uint8_t a = (color >> 24) & 0xff;
        uint8_t r = (color >> 16) & 0xff;
        uint8_t g = (color >> 8) & 0xff;
        uint8_t b = (color) & 0xff;

        binaryWriter.appendUInt8(r);
        binaryWriter.appendUInt8(g);
        binaryWriter.appendUInt8(b);
        binaryWriter.appendUInt8(a);
    }

    s_writeOffsetLength(jsonWriter, dataOffset, binaryWriter.data.size() - dataOffset);

    jsonWriter.EndObject();
}

static void s_initBoneReverseMap(
    std::vector<unsigned int>& outBoneReverseMap,
    const std::vector<unsigned int>& usedBones
) {
    unsigned int maxUsedBone = 0;
    for (size_t i = 0; i < usedBones.size(); ++i) {
        maxUsedBone = std::max(maxUsedBone, usedBones[i]);
    }
    if (maxUsedBone >= (1U << 16)) {
        // Sanity check before grabbing all that memory.
        throw std::runtime_error("excessive number of bones");
    }
    outBoneReverseMap = std::vector<unsigned int>(maxUsedBone + 1, 0);
    for (size_t i = 0; i < usedBones.size(); ++i) {
        outBoneReverseMap[usedBones[i]] = i;
    }
}

static void s_writeInfluences(
    BinaryWriter& binaryWriter,
    JsonWriter& jsonWriter,
    const CalCoreSubmesh& submesh,
    unsigned int influenceLimit,
    float influenceMargin = .001) {
    size_t dataOffset = binaryWriter.data.size();

    const auto influenceData = submesh.exportInfluences(influenceLimit, influenceMargin);
    const auto& weightsBoneIdsPairs = influenceData.weightsBoneIdsPairs;
    const auto maximumInfluenceCount = influenceData.maximumInfluenceCount;

    std::vector<uint16_t> weights(maximumInfluenceCount);
    std::vector<uint8_t> boneIds(maximumInfluenceCount);

    std::vector<unsigned int> boneReverseMap;
    s_initBoneReverseMap(boneReverseMap, influenceData.usedBoneIds);

    for (size_t j = 0; j < submesh.getVertexCount(); ++j) {
        const auto& pair = weightsBoneIdsPairs[j];

        for (size_t k = 0; k < maximumInfluenceCount; ++k) {
            const auto boneId = pair.boneIds[k];
            const auto remappedBoneId = boneReverseMap.at(boneId);

            weights[k] = pair.weights[k] * 65535.0;
            boneIds[k] = remappedBoneId;
        }

        binaryWriter.appendUInt16s(weights);
        binaryWriter.appendUInt8s(boneIds);
    }

    jsonWriter.String("influences");
    jsonWriter.StartObject();

    jsonWriter.String("count");
    jsonWriter.Uint(influenceData.weightsBoneIdsPairs.size());

    jsonWriter.String("maximum");
    jsonWriter.Uint(influenceData.maximumInfluenceCount);

    s_writeOffsetLength(jsonWriter, dataOffset, binaryWriter.data.size() - dataOffset);

    jsonWriter.EndObject();
}

static void s_writeMorphTargets(
    BinaryWriter& binaryWriter,
    JsonWriter& jsonWriter,
    const CalCoreSubmesh& submesh
) {
    jsonWriter.String("morphs");
    jsonWriter.StartObject();

    const auto& morphTargets = submesh.getMorphTargets();
    size_t count = 0;
    for (auto morphIt = morphTargets.begin(); morphIt != morphTargets.end(); ++morphIt) {
        count += (*morphIt)->vertexOffsets.size();
    }
    jsonWriter.String("count");
    jsonWriter.Uint(count);

    cal3d::SSEArray<VertexOffset> vertexOffsets(count);
    size_t vertexOffsetIndex = 0;
    // TODO - perf: May be faster to pre-deinterleave?
    for (auto morphIt = morphTargets.begin(); morphIt != morphTargets.end(); ++morphIt) {
        const auto& sourceVertexOffsets = (*morphIt)->vertexOffsets;

        const auto startIt = &vertexOffsets[vertexOffsetIndex];

        for (auto voIt = sourceVertexOffsets.begin(); voIt != sourceVertexOffsets.end(); ++voIt) {
            vertexOffsets[vertexOffsetIndex++] = *voIt;
        }

        const auto endIt = &vertexOffsets[vertexOffsetIndex];
        std::sort(startIt, endIt, VertexOffsetIdComparator());
    }

    jsonWriter.String("indices");
    jsonWriter.StartObject();

    size_t dataOffset = binaryWriter.data.size();
    std::vector<uint32_t> ids;
    ids.reserve(count);
    for (auto voIt = vertexOffsets.begin(); voIt != vertexOffsets.end(); ++voIt) {
        ids.push_back(voIt->vertexId);
    }
    auto outputData = CalCompactUtil::encodeMidi(CalCompactUtil::encodeDeltaZigzag(ids));
    binaryWriter.appendString(outputData);
    s_writeOffsetLength(jsonWriter, dataOffset, binaryWriter.data.size() - dataOffset);

    jsonWriter.EndObject();

    s_writeCoords<MorphExtractor<MorphPXExtractor> >(binaryWriter, jsonWriter, vertexOffsets, "px");
    s_writeCoords<MorphExtractor<MorphPYExtractor> >(binaryWriter, jsonWriter, vertexOffsets, "py");
    s_writeCoords<MorphExtractor<MorphPZExtractor> >(binaryWriter, jsonWriter, vertexOffsets, "pz");
    s_writeCoords<MorphExtractor<MorphNXExtractor> >(binaryWriter, jsonWriter, vertexOffsets, "nx");
    s_writeCoords<MorphExtractor<MorphNYExtractor> >(binaryWriter, jsonWriter, vertexOffsets, "ny");
    s_writeCoords<MorphExtractor<MorphNZExtractor> >(binaryWriter, jsonWriter, vertexOffsets, "nz");

    jsonWriter.String("morphs");
    jsonWriter.StartArray();

    for (auto morphIt = morphTargets.begin(); morphIt != morphTargets.end(); ++morphIt) {
        const auto& morphTarget = *(*morphIt);
        jsonWriter.StartObject();

        jsonWriter.String("name");
        jsonWriter.String(CalCompactUtil::encodeLatin1AsUtf8(morphTarget.name).c_str());

        jsonWriter.String("type");
        jsonWriter.String(s_morphTypeToString(morphTarget.morphTargetType));

        jsonWriter.String("length");
        jsonWriter.Uint(morphTarget.vertexOffsets.size());

        jsonWriter.EndObject();
    }

    jsonWriter.EndArray();

    jsonWriter.EndObject();
}

CalCompactSubmeshData CalCompactMeshWriter::saveCompactCoreSubmesh(const ISTD::shared_ptr<CalCoreSubmesh>& submesh, unsigned int influenceLimit, float influenceMargin) {
    BinaryWriter binaryWriter;
    JsonBuffer jsonBuffer;
    JsonWriter jsonWriter(jsonBuffer);

    jsonWriter.StartObject();

    s_writeIndices(binaryWriter, jsonWriter, *submesh);
    s_writePositions(binaryWriter, jsonWriter, *submesh);
    s_writeTexCoords(binaryWriter, jsonWriter, *submesh);
    s_writeNormals(binaryWriter, jsonWriter, *submesh);
    s_writeColors(binaryWriter, jsonWriter, *submesh);
    s_writeInfluences(binaryWriter, jsonWriter, *submesh, influenceLimit, influenceMargin);
    s_writeMorphTargets(binaryWriter, jsonWriter, *submesh);

    jsonWriter.EndObject();

    CalCompactSubmeshData result;
    result.jsonData = jsonBuffer.GetString();
    result.binaryData = binaryWriter.data;
    return result;
}
