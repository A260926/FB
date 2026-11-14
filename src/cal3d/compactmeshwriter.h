#pragma once

#include "cal3d/global.h"

#include <string>
#include <vector>

class CalCoreSubmesh;

class CAL3D_API CalCompactSubmeshData {
public:
    std::string jsonData;
    std::string binaryData;
};

class CAL3D_API CalCompactMeshWriter {
public:
    static CalCompactSubmeshData saveCompactCoreSubmesh(const ISTD::shared_ptr<CalCoreSubmesh>&, unsigned int influenceLimit=4, float influenceMargin=0.001);
};
