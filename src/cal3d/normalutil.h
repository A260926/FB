#pragma once

#include "cal3d/global.h"
#include "cal3d/memory.h"
#include "cal3d/vector4.h"

// Make old version of clang happy.
#if defined(__clang__) && __clang_major__ <= 3 && __clang_minor__ <= 1
  #include <stdint.h>
#else
  #include <cstdint>
#endif

#include <vector>

class CalCoreSubmesh;

class CAL3D_API CalNormalUtil {
public:
    static std::string encodeMeshNormals(const ISTD::shared_ptr<CalCoreSubmesh>& mesh);
    static ISTD::shared_ptr<CalCoreSubmesh> decodeMeshNormals(const std::string& data);

    static std::vector<uint16_t> encode(const cal3d::SSEArray<CalVector4>& normals);
    static cal3d::SSEArray<CalVector4> decode(const std::vector<uint16_t>& data);
};
