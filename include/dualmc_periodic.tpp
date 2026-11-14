// Copyright (C) 2017, Dominik Wodniok
// This software may be modified and distributed under the terms
// of the BSD 3-Clause license. See the LICENSE.txt file for details.

/// \file   dualmc_periodic.tpp
/// \brief  Three-axis periodic (tiling) variant of the Dual Marching Cubes
///         extraction. It reuses the original lookup tables, edge
///         intersection averaging and face winding, but all sample and
///         neighbor access wraps around the periodic domain.

//------------------------------------------------------------------------------

template<class T> inline
int32_t DualMC<T>::wrapCoord(int32_t const v, int32_t const n) {
    int32_t r = v % n;
    if(r < 0) r += n;
    return r;
}

//------------------------------------------------------------------------------

template<class T> inline
typename DualMC<T>::VolumeDataType
DualMC<T>::sampleAt(int32_t const x, int32_t const y, int32_t const z) const {
    return data[gA(wrapCoord(x, dims[0]),
                   wrapCoord(y, dims[1]),
                   wrapCoord(z, dims[2]))];
}

//------------------------------------------------------------------------------

template<class T> inline
int DualMC<T>::getCellCodePeriodic(
    int32_t const cx, int32_t const cy, int32_t const cz,
    VolumeDataType const iso) const {
    // determine for each cube corner if it is outside or inside.
    // Coordinates are not wrapped here: the cell lives on the infinite
    // lattice and the sample access performs the periodic wrapping.
    int code = 0;
    if(sampleAt(cx,  cy,  cz)   >= iso) code |= 1;
    if(sampleAt(cx+1,cy,  cz)   >= iso) code |= 2;
    if(sampleAt(cx,  cy+1,cz)   >= iso) code |= 4;
    if(sampleAt(cx+1,cy+1,cz)   >= iso) code |= 8;
    if(sampleAt(cx,  cy,  cz+1) >= iso) code |= 16;
    if(sampleAt(cx+1,cy,  cz+1) >= iso) code |= 32;
    if(sampleAt(cx,  cy+1,cz+1) >= iso) code |= 64;
    if(sampleAt(cx+1,cy+1,cz+1) >= iso) code |= 128;
    return code;
}

//------------------------------------------------------------------------------

template<class T> inline
int DualMC<T>::getDualPointCodePeriodic(
    int32_t const cx, int32_t const cy, int32_t const cz,
    VolumeDataType const iso, DMCEdgeCode const edge) const {
    int cubeCode = getCellCodePeriodic(cx, cy, cz, iso);

    if(generateManifold) {
        // Same Manifold Dual Marching Cubes rule as in getDualPointCode,
        // except that the neighbor lookup always succeeds: cells beyond a
        // period seam are reached through wrapped sample access.
        uint8_t const direction = problematicConfigs[uint8_t(cubeCode)];
        if(direction != 255) {
            int32_t neighborCoords[] = {cx, cy, cz};
            unsigned int const component = direction >> 1;
            int32_t const delta = (direction & 1) == 1 ? 1 : -1;
            neighborCoords[component] += delta;
            int const neighborCubeCode = getCellCodePeriodic(
                neighborCoords[0], neighborCoords[1], neighborCoords[2], iso);
            if(problematicConfigs[uint8_t(neighborCubeCode)] != 255) {
                cubeCode ^= 0xff;
            }
        }
    }

    for(int i = 0; i < 4; ++i)
        if(dualPointsList[cubeCode][i] & edge) {
            return dualPointsList[cubeCode][i];
        }
    return 0;
}

//------------------------------------------------------------------------------

template<class T> inline
void DualMC<T>::calculateDualPointPeriodic(
    int32_t const cx, int32_t const cy, int32_t const cz,
    VolumeDataType const iso, int const pointCode, Vertex & v) const {
    // Local corner coordinates follow the Morton corner numbering used by
    // the original implementation: bit 0 is x, bit 1 is y, bit 2 is z.
    static int8_t const cornerX[8] = {0,1,0,1,0,1,0,1};
    static int8_t const cornerY[8] = {0,0,1,1,0,0,1,1};
    static int8_t const cornerZ[8] = {0,0,0,0,1,1,1,1};

    // Standard marching cubes edge endpoint corners, matching EDGE0..EDGE11
    // of the original implementation.
    static int8_t const edgeFirst[12]  = {0,1,4,0,2,3,6,2,0,1,5,4};
    static int8_t const edgeSecond[12] = {1,5,5,4,3,7,7,6,2,3,7,6};

    // initialize the point with the canonical cell lower corner
    v.x = static_cast<VertexComponentsType>(cx);
    v.y = static_cast<VertexComponentsType>(cy);
    v.z = static_cast<VertexComponentsType>(cz);

    Vertex p;
    p.x = 0.0f;
    p.y = 0.0f;
    p.z = 0.0f;
    int points = 0;

    // Sum edge intersection vertices in the same edge order as the original
    // calculateDualPoint so that the averaged result is bit identical.
    for(int e = 0; e < 12; ++e) {
        if((pointCode & (1 << e)) == 0) continue;
        int const a = edgeFirst[e];
        int const b = edgeSecond[e];
        VolumeDataType const va = sampleAt(
            cx + cornerX[a], cy + cornerY[a], cz + cornerZ[a]);
        VolumeDataType const vb = sampleAt(
            cx + cornerX[b], cy + cornerY[b], cz + cornerZ[b]);
        float const t =
            (static_cast<float>(iso) - static_cast<float>(va)) /
            (static_cast<float>(vb) - static_cast<float>(va));
        p.x += static_cast<float>(cornerX[a])
             + t * (static_cast<float>(cornerX[b]) - static_cast<float>(cornerX[a]));
        p.y += static_cast<float>(cornerY[a])
             + t * (static_cast<float>(cornerY[b]) - static_cast<float>(cornerY[a]));
        p.z += static_cast<float>(cornerZ[a])
             + t * (static_cast<float>(cornerZ[b]) - static_cast<float>(cornerZ[a]));
        ++points;
    }

    float const invPoints = 1.0f / static_cast<float>(points);
    p.x *= invPoints;
    p.y *= invPoints;
    p.z *= invPoints;

    v.x += p.x;
    v.y += p.y;
    v.z += p.z;
}

//------------------------------------------------------------------------------

template<class T> inline
QuadIndexType DualMC<T>::getSharedPeriodicDualPointIndex(
    int32_t const cx, int32_t const cy, int32_t const cz,
    VolumeDataType const iso, DMCEdgeCode const edge,
    std::vector<Vertex> & vertices) {
    // Vertex identity is the canonical cell plus the corrected dual point
    // code. Floating point positions are never used for welding.
    DualPointKey key;
    key.linearizedCellID = gA(cx, cy, cz);
    key.pointCode = getDualPointCodePeriodic(cx, cy, cz, iso, edge);

    auto iterator = periodicPointToIndex.find(key);
    if(iterator != periodicPointToIndex.end()) {
        return iterator->second;
    } else {
        QuadIndexType const newVertexId = static_cast<QuadIndexType>(vertices.size());
        vertices.emplace_back();
        calculateDualPointPeriodic(cx, cy, cz, iso, key.pointCode, vertices.back());
        periodicPointToIndex[key] = newVertexId;
        return newVertexId;
    }
}

//------------------------------------------------------------------------------

template<class T> inline
QuadIndexType DualMC<T>::getPeriodicCorner(
    int32_t const ux, int32_t const uy, int32_t const uz,
    VolumeDataType const iso, DMCEdgeCode const edge,
    std::vector<Vertex> & vertices,
    int32_t offsets[3]) {
    // Lift the cell corner to its image in the canonical period and record
    // how many periods the unwrapped cell image was shifted.
    int32_t const cx = wrapCoord(ux, dims[0]);
    int32_t const cy = wrapCoord(uy, dims[1]);
    int32_t const cz = wrapCoord(uz, dims[2]);
    offsets[0] = (ux - cx) / dims[0];
    offsets[1] = (uy - cy) / dims[1];
    offsets[2] = (uz - cz) / dims[2];
    return getSharedPeriodicDualPointIndex(cx, cy, cz, iso, edge, vertices);
}

//------------------------------------------------------------------------------

template<class T> inline
void DualMC<T>::buildPeriodic(
    VolumeDataType const * data,
    int32_t const nx, int32_t const ny, int32_t const nz,
    VolumeDataType const iso,
    bool const generateManifold,
    PeriodicMesh & out) {

    this->dims[0] = nx;
    this->dims[1] = ny;
    this->dims[2] = nz;
    this->data = data;
    this->generateManifold = generateManifold;

    // every call replaces the output
    out.vertices.clear();
    out.quads.clear();
    periodicPointToIndex.clear();

    buildPeriodicQuads(iso, out);
}

//------------------------------------------------------------------------------

namespace periodic_detail {

/// One resolved quad corner: shared vertex index plus the period offset of
/// the lifted cell image.
struct Corner {
    QuadIndexType index;
    int32_t offset[3];
};

/// Build a quad from four resolved corners in the given winding order.
inline void fillQuad(PeriodicQuad & q, Corner const (& c)[4],
                     int const o0, int const o1, int const o2, int const o3) {
    int const order[4] = {o0, o1, o2, o3};
    QuadIndexType ids[4];
    for(int k = 0; k < 4; ++k) {
        ids[k] = c[order[k]].index;
        q.offset[k][0] = c[order[k]].offset[0];
        q.offset[k][1] = c[order[k]].offset[1];
        q.offset[k][2] = c[order[k]].offset[2];
    }
    q.i0 = ids[0];
    q.i1 = ids[1];
    q.i2 = ids[2];
    q.i3 = ids[3];
}

} // namespace periodic_detail

//------------------------------------------------------------------------------

template<class T> inline
void DualMC<T>::buildPeriodicQuads(
    VolumeDataType const iso,
    PeriodicMesh & out) {

    int32_t const nx = dims[0];
    int32_t const ny = dims[1];
    int32_t const nz = dims[2];

    out.vertices.reserve(4 * static_cast<size_t>(nx) * ny * nz);
    out.quads.reserve(3 * static_cast<size_t>(nx) * ny * nz);

    // Every positive grid edge of the canonical period owns exactly one
    // face. Edge endpoint samples are read through wrapped sample access,
    // so edges crossing a seam are handled like any other edge.
    for(int32_t z = 0; z < nz; ++z)
        for(int32_t y = 0; y < ny; ++y)
            for(int32_t x = 0; x < nx; ++x) {
                periodic_detail::Corner c[4];

                // construct quad for the x edge
                {
                    bool const entering = sampleAt(x, y, z) < iso
                                       && sampleAt(x + 1, y, z) >= iso;
                    bool const exiting  = sampleAt(x, y, z) >= iso
                                       && sampleAt(x + 1, y, z) < iso;
                    if(entering || exiting) {
                        c[0].index = getPeriodicCorner(x, y, z, iso, EDGE0, out.vertices, c[0].offset);
                        c[1].index = getPeriodicCorner(x, y, z - 1, iso, EDGE2, out.vertices, c[1].offset);
                        c[2].index = getPeriodicCorner(x, y - 1, z - 1, iso, EDGE6, out.vertices, c[2].offset);
                        c[3].index = getPeriodicCorner(x, y - 1, z, iso, EDGE4, out.vertices, c[3].offset);

                        // face winding follows the original entering/exiting rule
                        PeriodicQuad q;
                        if(entering) {
                            periodic_detail::fillQuad(q, c, 0, 1, 2, 3);
                        } else {
                            periodic_detail::fillQuad(q, c, 0, 3, 2, 1);
                        }
                        out.quads.push_back(q);
                    }
                }

                // construct quad for the y edge
                {
                    bool const entering = sampleAt(x, y, z) < iso
                                       && sampleAt(x, y + 1, z) >= iso;
                    bool const exiting  = sampleAt(x, y, z) >= iso
                                       && sampleAt(x, y + 1, z) < iso;
                    if(entering || exiting) {
                        c[0].index = getPeriodicCorner(x, y, z, iso, EDGE8, out.vertices, c[0].offset);
                        c[1].index = getPeriodicCorner(x, y, z - 1, iso, EDGE11, out.vertices, c[1].offset);
                        c[2].index = getPeriodicCorner(x - 1, y, z - 1, iso, EDGE10, out.vertices, c[2].offset);
                        c[3].index = getPeriodicCorner(x - 1, y, z, iso, EDGE9, out.vertices, c[3].offset);

                        // face winding follows the original entering/exiting rule
                        PeriodicQuad q;
                        if(exiting) {
                            periodic_detail::fillQuad(q, c, 0, 1, 2, 3);
                        } else {
                            periodic_detail::fillQuad(q, c, 0, 3, 2, 1);
                        }
                        out.quads.push_back(q);
                    }
                }

                // construct quad for the z edge
                {
                    bool const entering = sampleAt(x, y, z) < iso
                                       && sampleAt(x, y, z + 1) >= iso;
                    bool const exiting  = sampleAt(x, y, z) >= iso
                                       && sampleAt(x, y, z + 1) < iso;
                    if(entering || exiting) {
                        c[0].index = getPeriodicCorner(x, y, z, iso, EDGE3, out.vertices, c[0].offset);
                        c[1].index = getPeriodicCorner(x - 1, y, z, iso, EDGE1, out.vertices, c[1].offset);
                        c[2].index = getPeriodicCorner(x - 1, y - 1, z, iso, EDGE5, out.vertices, c[2].offset);
                        c[3].index = getPeriodicCorner(x, y - 1, z, iso, EDGE7, out.vertices, c[3].offset);

                        // face winding follows the original entering/exiting rule
                        PeriodicQuad q;
                        if(exiting) {
                            periodic_detail::fillQuad(q, c, 0, 1, 2, 3);
                        } else {
                            periodic_detail::fillQuad(q, c, 0, 3, 2, 1);
                        }
                        out.quads.push_back(q);
                    }
                }
            }
}
