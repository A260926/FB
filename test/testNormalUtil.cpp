#include "TestPrologue.h"
#include <cal3d/normalutil.h>

#include <limits>

FIXTURE(NormalUtilFixture) {
};

TEST_F(NormalUtilFixture, can_compress_bunch_of_observed_normals) {
    CalVector4 normalsArr[] = {
        CalVector4(-5.168229972945474e-15, 1.0, 0.0),
        CalVector4(-1.0, 0.0, 0.0),
        CalVector4(5.168229972945474e-15, 1.0, 6.428999914118854e-23),
        CalVector4(5.168229972945474e-15, 1.0, 0.0),
        CalVector4(0.0, -1.0, 0.0),
        CalVector4(0.0, -1.0, 0.0),
        CalVector4(1.0, 0.0, 1.2439500096661504e-08),
        CalVector4(1.0, 1.1469600025293403e-07, 1.7594299350776055e-08),
        CalVector4(1.0, 0.0, 0.0)
    };
    auto normals = arrayToSSEArray(normalsArr);
    auto decoded = CalNormalUtil::decode(CalNormalUtil::encode(normals));
    CHECK_VECTOR_CLOSE(normals, decoded, 0.01f);
}

TEST_F(NormalUtilFixture, can_compress_gap_normal) {
    // This is one of the normals that happens to fall after the indexing
    // "gap" in Wu Yu Wen's (atyuwen) normal_encoding implementation.
    CalVector4 normalsArr[] = {
        CalVector4(-0.707106781187f, 0.0f, -0.707106781187f)
    };
    auto normals = arrayToSSEArray(normalsArr);
    auto decoded = CalNormalUtil::decode(CalNormalUtil::encode(normals));
    CHECK_VECTOR_CLOSE(normals, decoded, 0.01f);
}

TEST_F(NormalUtilFixture, returns_something_reasonable_for_abnormal_normals) {
    // as seen in pid 3888337 ("Night Light Bedroom Mesh") (NSTAR-3551)
    CalVector4 badNormalsArr[] = {
        CalVector4(1000000.0f, 0.0f, 1000000.0f),
        CalVector4(0.0f, -1000000.0f, 1000000.0f),
        CalVector4(0.0f, -1000000.0f, -1000000.0f),
        CalVector4(1000000.0f, 0.0f, -1000000.0f),
        CalVector4(0.0f, 1000000.0f, 1000000.0f),
        CalVector4(0.0f, -1000000.0f, 1000000.0f),
        CalVector4(0.0f, 1000000.0f, -1000000.0f),
        CalVector4(0.0f, -1000000.0f, -1000000.0f),
        CalVector4(-1000000.0f, 0.0f, 1000000.0f),
        CalVector4(-1000000.0f, 0.0f, -1000000.0f)
    };
    auto badNormals = arrayToSSEArray(badNormalsArr);

    cal3d::SSEArray<CalVector4> normalizedNormals;
    for (auto it = badNormals.begin(); it != badNormals.end(); ++it) {
        auto normalizedNormal = it->asCalVector();
        normalizedNormal.normalize();
        normalizedNormals.push_back(normalizedNormal.asCalVector4());
    }

    auto decoded = CalNormalUtil::decode(CalNormalUtil::encode(badNormals));
    CHECK_VECTOR_CLOSE(normalizedNormals, decoded, 0.01f);
}

TEST_F(NormalUtilFixture, does_not_crash_on_bad_normals) {
    const float nanValue = std::numeric_limits<float>::quiet_NaN();
    const float infValue = std::numeric_limits<float>::infinity();

    // Paranoia: Haven't actually seen these in assets yet.
    CalVector4 badNormalsArr[] = {
        CalVector4(0.0f, 0.0f, 0.0f),
        CalVector4(1.2439500096661504e-08f, 1.2439500096661504e-08f, 1.2439500096661504e-08f),
        CalVector4(nanValue, 1.0f, 0.0f),
        CalVector4(infValue, 1.0f, 0.0f),
    };
    auto badNormals = arrayToSSEArray(badNormalsArr);
    auto decoded = CalNormalUtil::decode(CalNormalUtil::encode(badNormals));

    CHECK_EQUAL(badNormals.size(), decoded.size());
    CHECK_CLOSE(1.0f, decoded[0].length(), 0.001f);
    CHECK_CLOSE(1.0f, decoded[1].length(), 0.001f);
    CHECK_CLOSE(1.0f, decoded[2].length(), 0.001f);
    CHECK_CLOSE(1.0f, decoded[3].length(), 0.001f);
}
