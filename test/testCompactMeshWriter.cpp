#include "TestPrologue.h"
#include <cal3d/compactmeshwriter.h>
#include <cal3d/compactutil.h>
#include <cal3d/coremorphtarget.h>
#include <cal3d/coresubmesh.h>
#include <cal3d/json.h>
#include <cal3d/normalutil.h>

#include <cmath>

struct MorphSummary {
    std::string name;
    CalMorphTargetType type;
    unsigned int length;
};

namespace cal3d {
    template<>
    MaybeError jsonGet(const Json& val, CalQuantizedData& result) {
        if (auto error = jsonGet(val, "minimum", result.minimum)) {
            return error;
        }
        if (auto error = jsonGet(val, "maximum", result.maximum)) {
            return error;
        }
        if (auto error = jsonGet(val, "maxQuantizedValue", result.maxQuantizedValue)) {
            return error;
        }
        return jsonSuccess();
    }

    template<>
    MaybeError jsonGet(const Json& val, MorphSummary& result) {
        if (auto error = jsonGet(val, "name", result.name)) {
            return error;
        }
        std::string type;
        if (auto error = jsonGet(val, "type", type)) {
            return error;
        }
        if (type == "Exclusive") {
            result.type = CalMorphTargetTypeExclusive;
        } else if (type == "Average") {
            result.type = CalMorphTargetTypeAverage;
        } else if (type == "Additive") {
            result.type = CalMorphTargetTypeAdditive;
        } else if (type == "Clamped") {
            result.type = CalMorphTargetTypeClamped;
        } else {
            return MaybeError("Unrecognized morph type value \"" + type + "\"");
        }
        if (auto error = jsonGet(val, "length", result.length)) {
            return error;
        }
        return jsonSuccess();
    }
}

#define JSON_GET_KEY(json, key, outValue) \
    do { \
        if (auto error = cal3d::jsonGet((json), (key), (outValue))) { \
            throw std::runtime_error(*error); \
        } \
    } while (0)

#define JSON_GET(json, outValue) \
    do { \
        if (auto error = cal3d::jsonGet((json), (outValue))) { \
            throw std::runtime_error(*error); \
        } \
    } while (0)

inline bool AreClose(
    const CalCoreSubmesh::TextureCoordinate& uv1,
    const CalCoreSubmesh::TextureCoordinate& uv2,
    float tolerance
) {
    return (
        std::fabs(uv1.u - uv2.u) < tolerance
        && std::fabs(uv1.v - uv2.v) < tolerance
    );
}

FIXTURE(CompactMeshWriterFixture) {
    ISTD::shared_ptr<CalCoreSubmesh> makeSubmesh(
        const cal3d::SSEArray<CalPoint4>& positions,
        const cal3d::SSEArray<CalVector4>& normals,
        const std::vector<CalColor32>& colors,
        const std::vector<CalCoreSubmesh::Face>& faces,
        const std::vector<CalCoreSubmesh::InfluenceVector>& influenceVectors,
        const std::vector<CalCoreSubmesh::TextureCoordinate>& texCoords
    ) {
        ISTD::shared_ptr<CalCoreSubmesh> submesh = ISTD::make_shared<CalCoreSubmesh>(positions.size(), !texCoords.empty(), faces.size());
        for (auto it = faces.begin(); it != faces.end(); ++it) {
            submesh->addFace(*it);
        }

        for (size_t i = 0; i < texCoords.size(); ++i) {
            submesh->setTextureCoordinate(i, texCoords[i]);
        }

        for (size_t i = 0; i < positions.size(); ++i) {
            CalCoreSubmesh::Vertex vertex;
            vertex.position = positions[i];

            if (normals.size() != 0) {
                vertex.normal = normals[i];
            }

            CalCoreSubmesh::InfluenceVector influenceVector;
            if (!influenceVectors.empty()) {
                influenceVector = influenceVectors[i];
            }

            CalColor32 color = 0;
            if (!colors.empty()) {
                color = colors[i];
            }

            submesh->addVertex(vertex, color, influenceVector);
        }

        return submesh;
    }

    std::string extractDescribedData(const cal3d::Json& json, const std::string& binaryData) {
        size_t offset;
        size_t length;
        JSON_GET_KEY(json, "offset", offset);
        JSON_GET_KEY(json, "length", length);
        return binaryData.substr(offset, length);
    }

    std::vector<CalIndex> extractIndices(const CalCompactSubmeshData& data) {
        rapidjson::Document json;
        json.Parse<0>(data.jsonData.c_str());
        if (json.HasParseError()) {
            throw std::runtime_error("Could not parse JSON");
        }

        const cal3d::Json* indicesJson;
        JSON_GET_KEY(json, "indices", indicesJson);

        size_t count;
        JSON_GET_KEY(*indicesJson, "count", count);

        auto indicesData = extractDescribedData(*indicesJson, data.binaryData);
        auto indicesAsInts = CalCompactUtil::decodeHighWaterMark(CalCompactUtil::decodeMidiToShorts(indicesData));

        return std::vector<CalIndex>(indicesAsInts.begin(), indicesAsInts.end());
    }

    std::vector<uint32_t> extractDescribedIndexValues(
        const cal3d::Json* enclosingJson,
        const char* coordName,
        const std::string& binaryData
    ) {
        const cal3d::Json* coordJson;
        JSON_GET_KEY(*enclosingJson, coordName, coordJson);
        auto coordData = extractDescribedData(*coordJson, binaryData);
        return CalCompactUtil::decodeDeltaZigzag(CalCompactUtil::decodeMidi(coordData));
    }

    std::vector<float> extractDescribedQuantizedValues(
        const cal3d::Json* enclosingJson,
        const char* coordName,
        const std::string& binaryData
    ) {
        const cal3d::Json* coordJson;
        JSON_GET_KEY(*enclosingJson, coordName, coordJson);
        auto coordData = extractDescribedData(*coordJson, binaryData);

        CalQuantizedData coordQuantizedData;
        JSON_GET(*coordJson, coordQuantizedData);
        coordQuantizedData.values = CalCompactUtil::decodeDeltaZigzag(CalCompactUtil::decodeMidi(coordData));
        return CalCompactUtil::unquantize(coordQuantizedData);
    }

    cal3d::SSEArray<CalPoint4> extractPositions(const CalCompactSubmeshData& data) {
        rapidjson::Document json;
        json.Parse<0>(data.jsonData.c_str());
        if (json.HasParseError()) {
            throw std::runtime_error("Could not parse JSON");
        }

        const cal3d::Json* positionsJson;
        JSON_GET_KEY(json, "positions", positionsJson);

        size_t count;
        JSON_GET_KEY(*positionsJson, "count", count);

        std::vector<std::vector<float> > coords;
        coords.push_back(extractDescribedQuantizedValues(positionsJson, "x", data.binaryData));
        coords.push_back(extractDescribedQuantizedValues(positionsJson, "y", data.binaryData));
        coords.push_back(extractDescribedQuantizedValues(positionsJson, "z", data.binaryData));

        for (auto it = coords.begin(); it != coords.end(); ++it) {
            assert(count == (*it).size());
        }

        cal3d::SSEArray<CalPoint4> points;
        for (size_t i = 0; i < count; ++i) {
            points.push_back(CalPoint4(coords[0][i], coords[1][i], coords[2][i]));
        }

        return points;
    }

    std::vector<std::pair<float, uint8_t> > extractInfluences(const CalCompactSubmeshData& data) {
        rapidjson::Document json;
        json.Parse<0>(data.jsonData.c_str());
        if (json.HasParseError()) {
            throw std::runtime_error("Could not parse JSON");
        }

        const cal3d::Json* influencesJson;
        JSON_GET_KEY(json, "influences", influencesJson);

        size_t count;
        JSON_GET_KEY(*influencesJson, "count", count);

        unsigned int maximum;
        JSON_GET_KEY(*influencesJson, "maximum", maximum);

        std::vector<std::pair<float, uint8_t> > pairs;
        auto influenceData = extractDescribedData(*influencesJson, data.binaryData);
        const char* influenceDataPtr = influenceData.c_str();
        const char* influenceDataEnd = influenceDataPtr + influenceData.size() * sizeof(char);
        for (size_t i = 0; i < count; ++i) {
            const uint16_t* weightsDataPtr = reinterpret_cast<const uint16_t*>(influenceDataPtr);
            influenceDataPtr += sizeof(uint16_t) * maximum;
            const uint8_t* boneIdsDataPtr = reinterpret_cast<const uint8_t*>(influenceDataPtr);
            influenceDataPtr += sizeof(uint8_t) * maximum;
            if (influenceDataPtr > influenceDataEnd) {
                throw std::runtime_error("Truncated influence data");
            }
            for (size_t j = 0; j < maximum; ++j) {
                float weight = *(weightsDataPtr++) / 65535.0;
                uint8_t boneId = *(boneIdsDataPtr++);
                pairs.push_back(std::pair<float, uint8_t>(weight, boneId));
            }
        }
        if (influenceDataPtr != influenceDataEnd) {
            throw std::runtime_error("Garbage at end of influence data");
        }

        return pairs;
    }

    std::vector<CalCoreSubmesh::TextureCoordinate> extractTexCoords(const CalCompactSubmeshData& data) {
        rapidjson::Document json;
        json.Parse<0>(data.jsonData.c_str());
        if (json.HasParseError()) {
            throw std::runtime_error("Could not parse JSON");
        }

        const cal3d::Json* texCoordsJson;
        JSON_GET_KEY(json, "texCoords", texCoordsJson);

        size_t count;
        JSON_GET_KEY(*texCoordsJson, "count", count);

        std::vector<CalCoreSubmesh::TextureCoordinate> uvs;

        if (count == 0) {
            return uvs;
        }

        auto us = extractDescribedQuantizedValues(texCoordsJson, "u", data.binaryData);
        auto vs = extractDescribedQuantizedValues(texCoordsJson, "v", data.binaryData);
        if (us.size() != count) {
            throw std::runtime_error("U texture coord size mismatch");
        }
        if (vs.size() != count) {
            throw std::runtime_error("V texture coord size mismatch");
        }

        for (size_t i = 0; i < count; ++i) {
            uvs.push_back(CalCoreSubmesh::TextureCoordinate(us[i], vs[i]));
        }

        return uvs;
    }

    cal3d::SSEArray<CalVector4> extractNormals(const CalCompactSubmeshData& data) {
        rapidjson::Document json;
        json.Parse<0>(data.jsonData.c_str());
        if (json.HasParseError()) {
            throw std::runtime_error("Could not parse JSON");
        }

        const cal3d::Json* normalsJson;
        JSON_GET_KEY(json, "normals", normalsJson);

        auto normalsData = extractDescribedData(*normalsJson, data.binaryData);

        std::vector<uint16_t> encodedNormals;
        size_t count = normalsData.size() / 2;
        for (size_t i = 0 ; i < count; ++i) {
            unsigned char lowByte = normalsData[i*2];
            unsigned char highByte = normalsData[i*2 + 1];
            uint16_t value = (static_cast<uint16_t>(highByte) << 8) | static_cast<uint16_t>(lowByte);
            encodedNormals.push_back(value);
        }

        return CalNormalUtil::decode(encodedNormals);
    }

    std::vector<CalColor32> extractColors(const CalCompactSubmeshData& data) {
        rapidjson::Document json;
        json.Parse<0>(data.jsonData.c_str());
        if (json.HasParseError()) {
            throw std::runtime_error("Could not parse JSON");
        }

        const cal3d::Json* colorsJson;
        JSON_GET_KEY(json, "colors", colorsJson);

        auto colorsData = extractDescribedData(*colorsJson, data.binaryData);

        std::vector<CalColor32> colors;
        size_t count = colorsData.size() / 4;
        for (size_t i = 0 ; i < count; ++i) {
            unsigned char r = colorsData[i*4];
            unsigned char g = colorsData[i*4 + 1];
            unsigned char b = colorsData[i*4 + 2];
            unsigned char a = colorsData[i*4 + 3];
            uint32_t value = (
                  (static_cast<uint32_t>(a) << 24)
                | (static_cast<uint32_t>(r) << 16)
                | (static_cast<uint32_t>(g) << 8)
                | (static_cast<uint32_t>(b))
            );
            colors.push_back(value);
        }

        return colors;
    }

    std::vector<ISTD::shared_ptr<CalCoreMorphTarget>> extractMorphTargets(
        const CalCompactSubmeshData& data,
        size_t vertexCount,
        std::vector<MorphSummary>& outMorphSummaries
    ) {
        rapidjson::Document json;
        json.Parse<0>(data.jsonData.c_str());
        if (json.HasParseError()) {
            throw std::runtime_error("Could not parse JSON");
        }

        const cal3d::Json* morphsJson;
        JSON_GET_KEY(json, "morphs", morphsJson);

        size_t count;
        JSON_GET_KEY(*morphsJson, "count", count);

        auto ids = extractDescribedIndexValues(morphsJson, "indices", data.binaryData);
        auto px = extractDescribedQuantizedValues(morphsJson, "px", data.binaryData);
        auto py = extractDescribedQuantizedValues(morphsJson, "py", data.binaryData);
        auto pz = extractDescribedQuantizedValues(morphsJson, "pz", data.binaryData);
        auto nx = extractDescribedQuantizedValues(morphsJson, "nx", data.binaryData);
        auto ny = extractDescribedQuantizedValues(morphsJson, "ny", data.binaryData);
        auto nz = extractDescribedQuantizedValues(morphsJson, "nz", data.binaryData);

        assert(ids.size() == count);
        assert(px.size() == count);
        assert(py.size() == count);
        assert(pz.size() == count);
        assert(nx.size() == count);
        assert(ny.size() == count);
        assert(nz.size() == count);

        const cal3d::Json* morphsSummariesJson;
        JSON_GET_KEY(*morphsJson, "morphs", morphsSummariesJson);

        std::vector<MorphSummary> morphSummaries;
        JSON_GET(*morphsSummariesJson, morphSummaries);

        size_t morphIndex = 0;
        std::vector<ISTD::shared_ptr<CalCoreMorphTarget>> morphTargets;
        for (size_t i = 0; i < morphSummaries.size(); ++i) {
            const auto& morphSummary = morphSummaries[i];
            CalCoreMorphTarget::VertexOffsetArray vertexOffsets(morphSummary.length);
            for (size_t j = 0; j < morphSummary.length; ++j) {
                assert(morphIndex < count);
                vertexOffsets[j] = VertexOffset(
                    ids[morphIndex],
                    CalPoint4(px[morphIndex], py[morphIndex], pz[morphIndex]),
                    CalVector4(nx[morphIndex], ny[morphIndex], nz[morphIndex])
                );
                ++morphIndex;
            }
            ISTD::shared_ptr<CalCoreMorphTarget> morphTarget = ISTD::make_shared<CalCoreMorphTarget>(
                morphSummary.name,
                vertexCount,
                vertexOffsets
            );
            morphTargets.push_back(morphTarget);
        }
        outMorphSummaries = morphSummaries;
        return morphTargets;
    }
};

TEST_F(CompactMeshWriterFixture, encodes_indices) {
    cal3d::SSEArray<CalPoint4> positions(4);
    std::vector<CalCoreSubmesh::Face> faces;
    faces.push_back(CalCoreSubmesh::Face(0, 1, 2));
    faces.push_back(CalCoreSubmesh::Face(3, 1, 2));
    auto submesh = makeSubmesh(
        positions,
        cal3d::SSEArray<CalVector4>(),
        std::vector<CalColor32>(),
        faces,
        std::vector<CalCoreSubmesh::InfluenceVector>(positions.size()),
        std::vector<CalCoreSubmesh::TextureCoordinate>()
    );
    auto data = CalCompactMeshWriter::saveCompactCoreSubmesh(submesh, 6);
    auto indices = extractIndices(data);
    CalIndex expIndicesArr[] = {
        0, 1, 2, 3, 1, 2
    };
    auto expIndices = arrayToVector(expIndicesArr);
    CHECK_EQUAL(expIndices, indices);
}

TEST_F(CompactMeshWriterFixture, encodes_positions) {
    CalPoint4 positionsArr[] = {
        CalPoint4(0.06187180057168007, 0.066046804189682, 0.0769588053226471),
        CalPoint4(0.05021680146455765, 0.07639560103416443, 0.05728520080447197),
        CalPoint4(0.08780300617218018, 0.07711940258741379, 0.09079520404338837),
        CalPoint4(0.0964265987277031, 0.07549019902944565, 0.10219040513038635),
        CalPoint4(0.05733200162649155, 0.09348020702600479, 0.04277700185775757),
        CalPoint4(0.055307403206825256, 0.04079320281744003, 0.03469120338559151),
        CalPoint4(0.07342280447483063, 0.04373280331492424, 0.03270900249481201),
        CalPoint4(0.05239860340952873, 0.034591201692819595, 0.043652601540088654),
        CalPoint4(0.05233820155262947, 0.032610803842544556, 0.04249660298228264),
        CalPoint4(0.04999219998717308, 0.05711580067873001, 0.04051860421895981),
        CalPoint4(0.05684160068631172, 0.06149600073695183, 0.06578540056943893),
        CalPoint4(0.05486540123820305, 0.07479240000247955, 0.07586260885000229),
        CalPoint4(0.0766294077038765, 0.07472020387649536, 0.09316800534725189),
        CalPoint4(0.09037500619888306, 0.08750060945749283, 0.09108120203018188),
        CalPoint4(0.10732700675725937, 0.1019144058227539, 0.09624540060758591),
        CalPoint4(0.10551140457391739, 0.11532800644636154, 0.10839840769767761),
        CalPoint4(0.10113180428743362, 0.10119220614433289, 0.1084786057472229),
        CalPoint4(0.10767080634832382, 0.11542800813913345, 0.09162480384111404),
        CalPoint4(0.10596480220556259, 0.11355200409889221, 0.11334740370512009),
        CalPoint4(0.0977260023355484, 0.10386820882558823, 0.10368020087480545),
        CalPoint4(0.09721160680055618, 0.08801540732383728, 0.08562420308589935),
        CalPoint4(0.08798060566186905, 0.08439160883426666, 0.07118620723485947),
        CalPoint4(0.0779586061835289, 0.06951980292797089, 0.06995560228824615),
        CalPoint4(0.05679420381784439, 0.061967603862285614, 0.04528360068798065),
        CalPoint4(0.055577002465724945, 0.03893440216779709, 0.05204940214753151),
        CalPoint4(0.03900440037250519, 0.05208360031247139, 0.045479804277420044),
        CalPoint4(0.055672600865364075, 0.0570778027176857, 0.06210600212216377),
        CalPoint4(0.07154100388288498, 0.07010900229215622, 0.08613880723714828),
        CalPoint4(0.07811000198125839, 0.08450900763273239, -0.07252540439367294),
        CalPoint4(-0.061624202877283096, -0.05740680173039436, -0.07191580533981323),
        CalPoint4(-0.04576040059328079, -0.08335640281438828, -0.05286940187215805),
        CalPoint4(-0.07270120829343796, -0.03927840292453766, -0.04798440262675285),
        CalPoint4(-0.030171401798725128, -0.039192404597997665, -0.03825460374355316),
        CalPoint4(-0.030064400285482407, -0.05279900133609772, -0.027512801811099052),
        CalPoint4(-0.035592999309301376, -0.07093840092420578, -0.02740740217268467),
        CalPoint4(-0.050090402364730835, -0.033236801624298096, -0.086297407746315),
        CalPoint4(-0.039707001298666, -0.09196720272302628, -0.09767580032348633),
        CalPoint4(-0.08890680223703384, -0.06817880272865295, -0.09672380238771439),
        CalPoint4(-0.10307880491018295, -0.10395240783691406, -0.09665920585393906),
        CalPoint4(-0.08634380996227264, -0.11081840097904205, -0.05129219964146614),
        CalPoint4(-0.06572740525007248, -0.10066420584917068, -0.08028560131788254),
        CalPoint4(-0.05581200122833252, -0.06380560249090195, -0.049386002123355865),
        CalPoint4(-0.06300580501556396, -0.045800402760505676, -0.07178881019353867),
        CalPoint4(-0.045765601098537445, -0.09185520559549332, -0.07817739993333817),
        CalPoint4(-0.10824200510978699, -0.09800020605325699, -0.08168860524892807),
        CalPoint4(-0.10803240537643433, -0.09782260656356812, -0.08165380358695984),
        CalPoint4(-0.11071141064167023, -0.10015140473842621, -0.10386640578508377),
        CalPoint4(-0.10270200669765472, -0.09738420695066452, -0.09177420288324356),
        CalPoint4(-0.0830364003777504, -0.0858554020524025, -0.08856180310249329),
        CalPoint4(-0.07217640429735184, -0.07136020064353943, -0.06134520098567009),
        CalPoint4(-0.052688002586364746, -0.05700720101594925, -0.07021600753068924),
        CalPoint4(-0.04791960492730141, -0.04551979899406433, -0.03795440122485161),
        CalPoint4(-0.052278801798820496, -0.03529740124940872, -0.049619801342487335),
        CalPoint4(-0.06943219900131226, -0.0857074037194252, -0.06536560505628586),
        CalPoint4(-0.09135320782661438, -0.07976999878883362, -0.07806839793920517),
        CalPoint4(-0.07164260745048523, -0.0636489987373352, -0.050993602722883224),
        CalPoint4(-0.05567080155014992, -0.03950060158967972, -0.04928839951753616),
    };
    auto positions = arrayToSSEArray(positionsArr);
    std::vector<CalCoreSubmesh::Face> faces;
    faces.push_back(CalCoreSubmesh::Face(0, 0, 0));
    auto submesh = makeSubmesh(
        positions,
        cal3d::SSEArray<CalVector4>(),
        std::vector<CalColor32>(),
        faces,
        std::vector<CalCoreSubmesh::InfluenceVector>(),
        std::vector<CalCoreSubmesh::TextureCoordinate>()
    );

    auto data = CalCompactMeshWriter::saveCompactCoreSubmesh(submesh, 6);
    auto extractedPositions = extractPositions(data);
    CHECK_VECTOR_CLOSE(positions, extractedPositions, 0.001f);
}

TEST_F(CompactMeshWriterFixture, sends_normals) {
    const float sqrt2 = std::sqrt(2.0f);
    const float sqrt3 = std::sqrt(3.0f);

    CalVector4 normalsArr[] = {
        CalVector4(1.0f, 0.0f, 0.0f),
        CalVector4(0.0f, -1.0f / sqrt2, 1.0f / sqrt2),
        CalVector4(1.0f / sqrt3, 1.0f / sqrt3, -1.0f / sqrt3),
    };
    auto normals = arrayToSSEArray(normalsArr);

    cal3d::SSEArray<CalPoint4> positions(3);
    std::vector<CalCoreSubmesh::Face> faces;
    faces.push_back(CalCoreSubmesh::Face(0, 1, 2));
    auto submesh = makeSubmesh(
        positions,
        normals,
        std::vector<CalColor32>(),
        faces,
        std::vector<CalCoreSubmesh::InfluenceVector>(),
        std::vector<CalCoreSubmesh::TextureCoordinate>()
    );
    auto data = CalCompactMeshWriter::saveCompactCoreSubmesh(submesh, 6);
    auto extractedNormals = extractNormals(data);
    CHECK_VECTOR_CLOSE(normals, extractedNormals, 0.01f);
}

TEST_F(CompactMeshWriterFixture, sends_colors) {
    // Vertex color technically doesn't include alpha, even though we send it?
    CalColor32 colorsArr[] = {
        CalMakeColor(CalVector4(1.0f, 0.0f, 0.0f, 1.f)),
        CalMakeColor(CalVector4(0.1f, 0.2f, 0.3f, 1.f)),
        CalMakeColor(CalVector4(0.5f, 0.6f, 0.7f, 1.f))
    };
    auto colors = arrayToVector(colorsArr);

    cal3d::SSEArray<CalPoint4> positions(3);
    std::vector<CalCoreSubmesh::Face> faces;
    faces.push_back(CalCoreSubmesh::Face(0, 1, 2));
    auto submesh = makeSubmesh(
        positions,
        cal3d::SSEArray<CalVector4>(),
        colors,
        faces,
        std::vector<CalCoreSubmesh::InfluenceVector>(),
        std::vector<CalCoreSubmesh::TextureCoordinate>()
    );
    auto data = CalCompactMeshWriter::saveCompactCoreSubmesh(submesh, 6);
    auto extractedColors = extractColors(data);
    CHECK_EQUAL(colors, extractedColors);
}

TEST_F(CompactMeshWriterFixture, sends_tex_coords) {
    CalCoreSubmesh::TextureCoordinate texCoordsArr[] = {
        CalCoreSubmesh::TextureCoordinate(0.0f, 0.1f),
        CalCoreSubmesh::TextureCoordinate(0.2f, 0.3f),
        CalCoreSubmesh::TextureCoordinate(-10.0f, 20.0f),
    };
    auto texCoords = arrayToVector(texCoordsArr);

    cal3d::SSEArray<CalPoint4> positions(3);
    std::vector<CalCoreSubmesh::Face> faces;
    faces.push_back(CalCoreSubmesh::Face(0, 1, 2));
    auto submesh = makeSubmesh(
        positions,
        cal3d::SSEArray<CalVector4>(),
        std::vector<CalColor32>(),
        faces,
        std::vector<CalCoreSubmesh::InfluenceVector>(),
        texCoords
    );
    auto data = CalCompactMeshWriter::saveCompactCoreSubmesh(submesh, 6);
    auto extractedTexCoords = extractTexCoords(data);
    CHECK_VECTOR_CLOSE(texCoords, extractedTexCoords, 0.02f); // approx 20.0 / (1 << 10)
}

TEST_F(CompactMeshWriterFixture, does_not_send_metadata_if_there_are_no_texCoords) {
    cal3d::SSEArray<CalPoint4> positions(3);
    std::vector<CalCoreSubmesh::Face> faces;
    faces.push_back(CalCoreSubmesh::Face(0, 1, 2));
    auto submesh = makeSubmesh(
        positions,
        cal3d::SSEArray<CalVector4>(),
        std::vector<CalColor32>(),
        faces,
        std::vector<CalCoreSubmesh::InfluenceVector>(),
        std::vector<CalCoreSubmesh::TextureCoordinate>()
    );
    auto data = CalCompactMeshWriter::saveCompactCoreSubmesh(submesh, 6);

    rapidjson::Document json;
    json.Parse<0>(data.jsonData.c_str());
    if (json.HasParseError()) {
        throw std::runtime_error("Could not parse JSON");
    }

    const cal3d::Json* texCoordJson;
    JSON_GET_KEY(json, "texCoords", texCoordJson);

    size_t count;
    JSON_GET_KEY(*texCoordJson, "count", count);
    CHECK_EQUAL(0U, count);
    CHECK_EQUAL(1U, cal3d::getJsonMemberCount(*texCoordJson));
}

TEST_F(CompactMeshWriterFixture, sends_influence_weights) {
    cal3d::SSEArray<CalPoint4> positions(3);

    std::vector<CalCoreSubmesh::Face> faces;
    faces.push_back(CalCoreSubmesh::Face(0, 1, 2));

    CalCoreSubmesh::InfluenceVector justBone0;
    CalCoreSubmesh::InfluenceVector bones1And2;
    justBone0.push_back(CalCoreSubmesh::Influence(0, 1.0f, true));
    bones1And2.push_back(CalCoreSubmesh::Influence(1, 0.9f, false));
    bones1And2.push_back(CalCoreSubmesh::Influence(2, 0.1f, true));
    std::vector<CalCoreSubmesh::InfluenceVector> influenceVectors;
    influenceVectors.push_back(justBone0);
    influenceVectors.push_back(bones1And2);
    influenceVectors.push_back(bones1And2);
    auto submesh = makeSubmesh(
        positions,
        cal3d::SSEArray<CalVector4>(),
        std::vector<CalColor32>(),
        faces,
        influenceVectors,
        std::vector<CalCoreSubmesh::TextureCoordinate>()
    );

    auto data = CalCompactMeshWriter::saveCompactCoreSubmesh(submesh, 6);
    auto pairs = extractInfluences(data);
    std::vector<float> weights;
    for (size_t i = 0; i < pairs.size(); ++i) {
        weights.push_back(pairs[i].first);
    }
    float expWeightsArr[] = {
        1.0f, 0.0f,
        0.9f, 0.1f,
        0.9f, 0.1f
    };
    auto expWeights = arrayToVector(expWeightsArr);
    CHECK_VECTOR_CLOSE(expWeights, weights, 0.01);
}

TEST_F(CompactMeshWriterFixture, trims_negligible_influences) {
    cal3d::SSEArray<CalPoint4> positions(3);

    std::vector<CalCoreSubmesh::Face> faces;
    faces.push_back(CalCoreSubmesh::Face(0, 1, 2));

    CalCoreSubmesh::InfluenceVector justBone0;
    CalCoreSubmesh::InfluenceVector bones1And2;
    justBone0.push_back(CalCoreSubmesh::Influence(0, 1.0f, true));
    bones1And2.push_back(CalCoreSubmesh::Influence(1, 0.9f, false));
    bones1And2.push_back(CalCoreSubmesh::Influence(2, 0.0999f, true));
    std::vector<CalCoreSubmesh::InfluenceVector> influenceVectors;
    influenceVectors.push_back(justBone0);
    influenceVectors.push_back(bones1And2);
    influenceVectors.push_back(bones1And2);
    auto submesh = makeSubmesh(
        positions,
        cal3d::SSEArray<CalVector4>(),
        std::vector<CalColor32>(),
        faces,
        influenceVectors,
        std::vector<CalCoreSubmesh::TextureCoordinate>()
    );

    auto data = CalCompactMeshWriter::saveCompactCoreSubmesh(submesh, 6, 0.1f);
    auto pairs = extractInfluences(data);
    std::vector<float> weights;
    for (size_t i = 0; i < pairs.size(); ++i) {
        weights.push_back(pairs[i].first);
    }
    float expWeightsArr[] = {
        1.0f, 1.0f, 1.0f
    };
    auto expWeights = arrayToVector(expWeightsArr);
    CHECK_VECTOR_CLOSE(expWeights, weights, 0.01);
}

TEST_F(CompactMeshWriterFixture, remaps_influence_bone_index_to_batch_used_bone_index) {
    cal3d::SSEArray<CalPoint4> positions(6);

    std::vector<CalCoreSubmesh::Face> faces;
    faces.push_back(CalCoreSubmesh::Face(0, 1, 2));
    faces.push_back(CalCoreSubmesh::Face(3, 4, 5));

    CalCoreSubmesh::InfluenceVector justBone0;
    CalCoreSubmesh::InfluenceVector justBone1;
    CalCoreSubmesh::InfluenceVector justBone2;
    justBone0.push_back(CalCoreSubmesh::Influence(1, 1.0, true));
    justBone1.push_back(CalCoreSubmesh::Influence(2, 1.0, true));
    justBone2.push_back(CalCoreSubmesh::Influence(3, 1.0, true));
    std::vector<CalCoreSubmesh::InfluenceVector> influenceVectors;
    // Batch 1 vertex influences
    influenceVectors.push_back(justBone0);
    influenceVectors.push_back(justBone1);
    influenceVectors.push_back(justBone1);
    // Batch 2 vertex influences
    influenceVectors.push_back(justBone1);
    influenceVectors.push_back(justBone1);
    influenceVectors.push_back(justBone2);

    auto submesh = makeSubmesh(
        positions,
        cal3d::SSEArray<CalVector4>(),
        std::vector<CalColor32>(),
        faces,
        influenceVectors,
        std::vector<CalCoreSubmesh::TextureCoordinate>()
    );

    auto data = CalCompactMeshWriter::saveCompactCoreSubmesh(submesh, 6);
    auto pairs = extractInfluences(data);
    std::vector<unsigned int> boneIds;
    for (size_t i = 0; i < pairs.size(); ++i) {
        boneIds.push_back(pairs[i].second);
    }
    unsigned int expIndicesArr[] = {
        0, 1, 1,
        1, 1, 2
    };
    auto expIndices = arrayToVector(expIndicesArr);
    CHECK_EQUAL(expIndices, boneIds);
}

TEST_F(CompactMeshWriterFixture, can_encode_morph_target_types) {
    cal3d::SSEArray<CalPoint4> positions(1);
    std::vector<CalCoreSubmesh::Face> faces;
    faces.push_back(CalCoreSubmesh::Face(0, 0, 0));
    auto submesh = makeSubmesh(
        positions,
        cal3d::SSEArray<CalVector4>(),
        std::vector<CalColor32>(),
        faces,
        std::vector<CalCoreSubmesh::InfluenceVector>(positions.size()),
        std::vector<CalCoreSubmesh::TextureCoordinate>()
    );

    VertexOffset aVO;
    aVO.vertexId = 0;
    aVO.position = CalVector4(1, 0, 0);
    aVO.normal = CalVector4(1, 0, 1);
    CalCoreMorphTarget::VertexOffsetArray vertexOffsets;
    vertexOffsets.push_back(aVO);

    ISTD::shared_ptr<CalCoreMorphTarget> morphTarget0 = ISTD::make_shared<CalCoreMorphTarget>("name.exclusive", 1, vertexOffsets);
    submesh->addMorphTarget(morphTarget0);

    ISTD::shared_ptr<CalCoreMorphTarget> morphTarget1 = ISTD::make_shared<CalCoreMorphTarget>("name.additive", 1, vertexOffsets);
    submesh->addMorphTarget(morphTarget1);

    ISTD::shared_ptr<CalCoreMorphTarget> morphTarget2 = ISTD::make_shared<CalCoreMorphTarget>("name.clamped", 1, vertexOffsets);
    submesh->addMorphTarget(morphTarget2);

    ISTD::shared_ptr<CalCoreMorphTarget> morphTarget3 = ISTD::make_shared<CalCoreMorphTarget>("name.average", 1, vertexOffsets);
    submesh->addMorphTarget(morphTarget3);

    ISTD::shared_ptr<CalCoreMorphTarget> morphTarget4 = ISTD::make_shared<CalCoreMorphTarget>("name", 1, vertexOffsets);
    submesh->addMorphTarget(morphTarget4);

    auto data = CalCompactMeshWriter::saveCompactCoreSubmesh(submesh, 6);
    std::vector<MorphSummary> morphSummaries;
    auto extractedMorphTargets = extractMorphTargets(data, positions.size(), morphSummaries);
    CHECK_EQUAL(5U, morphSummaries.size());

    CHECK_EQUAL(CalMorphTargetTypeExclusive, morphSummaries[0].type);
    CHECK_EQUAL(CalMorphTargetTypeAdditive, morphSummaries[1].type);
    CHECK_EQUAL(CalMorphTargetTypeClamped, morphSummaries[2].type);
    CHECK_EQUAL(CalMorphTargetTypeAverage, morphSummaries[3].type);
    CHECK_EQUAL(CalMorphTargetTypeAdditive, morphSummaries[4].type);
}

TEST_F(CompactMeshWriterFixture, sorts_morph_blendVertices_by_id) {
    cal3d::SSEArray<CalPoint4> positions(2);
    std::vector<CalCoreSubmesh::Face> faces;
    faces.push_back(CalCoreSubmesh::Face(0, 1, 0));
    auto submesh = makeSubmesh(
        positions,
        cal3d::SSEArray<CalVector4>(),
        std::vector<CalColor32>(),
        faces,
        std::vector<CalCoreSubmesh::InfluenceVector>(positions.size()),
        std::vector<CalCoreSubmesh::TextureCoordinate>()
    );

    VertexOffset aVO;
    aVO.vertexId = 1;
    aVO.position = CalVector4(1, 0, 0);
    aVO.normal = CalVector4(1, 0, 1);

    VertexOffset bVO;
    bVO.vertexId = 0;
    bVO.position = CalVector4(0, 1, 0);
    bVO.normal = CalVector4(0, 1, 1);

    CalCoreMorphTarget::VertexOffsetArray vertexOffsets;
    vertexOffsets.push_back(aVO);
    vertexOffsets.push_back(bVO);
    ISTD::shared_ptr<CalCoreMorphTarget> morphTarget = ISTD::make_shared<CalCoreMorphTarget>("name", 2, vertexOffsets);

    submesh->addMorphTarget(morphTarget);

    auto data = CalCompactMeshWriter::saveCompactCoreSubmesh(submesh, 6);
    std::vector<MorphSummary> morphSummaries;
    auto extractedMorphTargets = extractMorphTargets(data, positions.size(), morphSummaries);
    CHECK_EQUAL(1U, extractedMorphTargets.size());
    CHECK_EQUAL(2U, extractedMorphTargets[0]->vertexOffsets.size());

    const auto& extractedVO0 = extractedMorphTargets[0]->vertexOffsets[0];
    CHECK_EQUAL(bVO.vertexId, extractedVO0.vertexId);
    CHECK_CLOSE(bVO.position, extractedVO0.position, 0.001f);
    CHECK_CLOSE(bVO.normal, extractedVO0.normal, 0.001f);

    const auto& extractedVO1 = extractedMorphTargets[0]->vertexOffsets[1];
    CHECK_EQUAL(aVO.vertexId, extractedVO1.vertexId);
    CHECK_CLOSE(aVO.position, extractedVO1.position, 0.001f);
    CHECK_CLOSE(aVO.normal, extractedVO1.normal, 0.001f);
}
