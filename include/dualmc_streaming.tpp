// Copyright (C) 2017, Dominik Wodniok
// This software may be modified and distributed under the terms
// of the BSD 3-Clause license. See the LICENSE.txt file for details.

/// \file   dualmc_streaming.tpp
/// \brief  Plane streaming variant of the (manifold) dual marching cubes
///         extraction. Geometry is emitted through callbacks instead of
///         materializing the volume and the mesh.
///
/// The extraction reproduces build(..., generateManifold, false, ...)
/// exactly while only keeping O(dimX*dimY) working storage:
///
/// - A ring of four sample planes is kept. While the quads of vertex
///   layer z are generated, the planes z-1 .. z+2 are resident. Dual
///   points of cell layers z-1 and z need the samples of planes z-1 .. z+1
///   and the manifold dual marching cubes correction of cell layer z can
///   look one additional plane ahead (C16/C19 neighbor in +z direction).
/// - The raw 8-bit cell codes of one additional cell layer (z-2) are
///   cached. Its sample planes have already been evicted from the ring,
///   but its codes are still required for the manifold correction of cell
///   layer z-1 (C16/C19 neighbor in -z direction).
/// - Shared vertex indices are tracked per cell layer. A dual point of
///   cell layer L can only be referenced by quads of vertex layers L and
///   L+1, so only the key records of two cell layers are active at any
///   time (at most four dual points per cell and layer).

//------------------------------------------------------------------------------

template<class T>
template<class ReadPlane, class EmitVertex, class EmitQuad>
bool DualMC<T>::buildStreaming(
    int32_t const dimX, int32_t const dimY, int32_t const dimZ,
    VolumeDataType const iso,
    bool const generateManifold,
    ReadPlane readPlane,
    EmitVertex emitVertex,
    EmitQuad emitQuad,
    StreamStats & stats
    ) const {

    // reset statistics
    stats.peakSampleValues = 0;
    stats.peakVertexKeys = 0;
    stats.emittedVertices = 0;
    stats.emittedQuads = 0;

    // reduced iteration extents, identical to buildSharedVerticesQuads
    int32_t const reducedX = dimX - 2;
    int32_t const reducedY = dimY - 2;
    int32_t const reducedZ = dimZ - 2;

    // number of samples per z-plane
    size_t const planeSize = size_t(dimX) * size_t(dimY);

    // ring of four sample planes, plane p resides in slot (p & 3)
    std::vector<VolumeDataType> planes(4 * planeSize);
    stats.peakSampleValues = planes.capacity();

    // cache of the raw 8-bit cell codes of a single cell layer and the
    // layer it currently holds (-1: none)
    std::vector<uint8_t> cellCodeCache(size_t(dimX - 1) * size_t(dimY - 1));
    int32_t cachedCellLayer = -1;

    // key of a dual point inside one z cell layer for shared vertex lookup.
    // Dual points of the same cell with different point codes are distinct
    // vertices and must not be merged.
    struct LayerPointKey {
        int32_t cellId;
        int pointCode;
        bool operator==(LayerPointKey const & other) const {
            return cellId == other.cellId && pointCode == other.pointCode;
        }
    };
    struct LayerPointKeyHash {
        size_t operator()(LayerPointKey const & k) const {
            return size_t(uint32_t(k.cellId)) | (size_t(uint32_t(k.pointCode)) << 32u);
        }
    };
    typedef std::unordered_map<LayerPointKey,QuadIndexType,LayerPointKeyHash> LayerPointMap;

    // active dual point indices of the previous (z-1) and current (z)
    // cell layer
    LayerPointMap prevLayerPoints;
    LayerPointMap currLayerPoints;

    // compact index of the next emitted vertex
    QuadIndexType nextVertexId = 0;

    // vertex layer which is currently being processed
    int32_t currentLayerZ = 0;

    // access a sample of a resident plane
    auto sampleAt = [&](int32_t const x, int32_t const y, int32_t const z) -> VolumeDataType {
        return planes[size_t(z & 3) * planeSize + size_t(y) * size_t(dimX) + size_t(x)];
    };

    // get the 8-bit in-out mask for the voxel corners of the cell cube at
    // (cx,cy,cz), identical to getCellCode. Codes of the cached cell layer
    // are read from the cache, all other codes are computed from the
    // resident sample planes.
    auto cellCodeAt = [&](int32_t const cx, int32_t const cy, int32_t const cz) -> int {
        if(cz == cachedCellLayer) {
            return cellCodeCache[size_t(cx) + size_t(dimX - 1) * size_t(cy)];
        }
        int code = 0;
        if(sampleAt(cx,cy,cz) >= iso)
            code |= 1;
        if(sampleAt(cx+1,cy,cz) >= iso)
            code |= 2;
        if(sampleAt(cx,cy+1,cz) >= iso)
            code |= 4;
        if(sampleAt(cx+1,cy+1,cz) >= iso)
            code |= 8;
        if(sampleAt(cx,cy,cz+1) >= iso)
            code |= 16;
        if(sampleAt(cx+1,cy,cz+1) >= iso)
            code |= 32;
        if(sampleAt(cx,cy+1,cz+1) >= iso)
            code |= 64;
        if(sampleAt(cx+1,cy+1,cz+1) >= iso)
            code |= 128;
        return code;
    };

    // get the 12-bit dual point code mask, identical to getDualPointCode.
    // This is also where the manifold dual marching cubes correction is
    // applied. The neighboring C16/C19 cell may live in the previous or
    // the next cell layer; both are resolved through cellCodeAt.
    auto dualPointCodeAt = [&](int32_t const cx, int32_t const cy, int32_t const cz,
      DMCEdgeCode const edge) -> int {
        int cubeCode = cellCodeAt(cx, cy, cz);

        if(generateManifold) {
            uint8_t const direction = problematicConfigs[uint8_t(cubeCode)];
            if(direction != 255) {
                int32_t neighborCoords[] = {cx,cy,cz};
                unsigned int const component = direction >> 1;
                int32_t const delta = (direction & 1) == 1 ? 1 : -1;
                neighborCoords[component] += delta;
                int32_t const dims[] = {dimX,dimY,dimZ};
                if(neighborCoords[component] >= 0 && neighborCoords[component] < (dims[component]-1)) {
                    int const neighborCubeCode = cellCodeAt(neighborCoords[0], neighborCoords[1], neighborCoords[2]);
                    if(problematicConfigs[uint8_t(neighborCubeCode)] != 255) {
                        cubeCode ^= 0xff;
                    }
                }
            }
        }
        for(int i = 0; i < 4; ++i)
            if(dualPointsList[cubeCode][i] & edge) {
                return dualPointsList[cubeCode][i];
            }
        return 0;
    };

    // compute a dual point, identical to calculateDualPoint
    auto calculateStreamingDualPoint = [&](int32_t const cx, int32_t const cy, int32_t const cz,
      int const pointCode, Vertex & v) {
        v.x = cx;
        v.y = cy;
        v.z = cz;

        Vertex p;
        p.x=0;
        p.y=0;
        p.z=0;
        int points = 0;

        if(pointCode & EDGE0) {
            p.x += ((float)iso - (float)sampleAt(cx,cy,cz))/((float)sampleAt(cx+1,cy,cz)-(float)sampleAt(cx,cy,cz));
            points++;
        }

        if(pointCode & EDGE1) {
            p.x += 1.0f;
            p.z += ((float)iso - (float)sampleAt(cx+1,cy,cz))/((float)sampleAt(cx+1,cy,cz+1)-(float)sampleAt(cx+1,cy,cz));
            points++;
        }

        if(pointCode & EDGE2) {
            p.x += ((float)iso - (float)sampleAt(cx,cy,cz+1))/((float)sampleAt(cx+1,cy,cz+1)-(float)sampleAt(cx,cy,cz+1));
            p.z += 1.0f;
            points++;
        }

        if(pointCode & EDGE3) {
            p.z += ((float)iso - (float)sampleAt(cx,cy,cz))/((float)sampleAt(cx,cy,cz+1)-(float)sampleAt(cx,cy,cz));
            points++;
        }

        if(pointCode & EDGE4) {
            p.x += ((float)iso - (float)sampleAt(cx,cy+1,cz))/((float)sampleAt(cx+1,cy+1,cz)-(float)sampleAt(cx,cy+1,cz));
            p.y += 1.0f;
            points++;
        }

        if(pointCode & EDGE5) {
            p.x += 1.0f;
            p.z += ((float)iso - (float)sampleAt(cx+1,cy+1,cz))/((float)sampleAt(cx+1,cy+1,cz+1)-(float)sampleAt(cx+1,cy+1,cz));
            p.y += 1.0f;
            points++;
        }

        if(pointCode & EDGE6) {
            p.x += ((float)iso - (float)sampleAt(cx,cy+1,cz+1))/((float)sampleAt(cx+1,cy+1,cz+1)-(float)sampleAt(cx,cy+1,cz+1));
            p.z += 1.0f;
            p.y += 1.0f;
            points++;
        }

        if(pointCode & EDGE7) {
            p.z += ((float)iso - (float)sampleAt(cx,cy+1,cz))/((float)sampleAt(cx,cy+1,cz+1)-(float)sampleAt(cx,cy+1,cz));
            p.y += 1.0f;
            points++;
        }

        if(pointCode & EDGE8) {
            p.y += ((float)iso - (float)sampleAt(cx,cy,cz))/((float)sampleAt(cx,cy+1,cz)-(float)sampleAt(cx,cy,cz));
            points++;
        }

        if(pointCode & EDGE9) {
            p.x += 1.0f;
            p.y += ((float)iso - (float)sampleAt(cx+1,cy,cz))/((float)sampleAt(cx+1,cy+1,cz)-(float)sampleAt(cx+1,cy,cz));
            points++;
        }

        if(pointCode & EDGE10) {
            p.x += 1.0f;
            p.y += ((float)iso - (float)sampleAt(cx+1,cy,cz+1))/((float)sampleAt(cx+1,cy+1,cz+1)-(float)sampleAt(cx+1,cy,cz+1));
            p.z += 1.0f;
            points++;
        }

        if(pointCode & EDGE11) {
            p.z += 1.0f;
            p.y += ((float)iso - (float)sampleAt(cx,cy,cz+1))/((float)sampleAt(cx,cy+1,cz+1)-(float)sampleAt(cx,cy,cz+1));
            points++;
        }

        float invPoints = 1.0f / (float)points;
        p.x*= invPoints;
        p.y*= invPoints;
        p.z*= invPoints;

        v.x += p.x;
        v.y += p.y;
        v.z += p.z;
    };

    // Get the shared index of the dual point identified by its cell and a
    // cube edge, emitting the vertex first if it has not been emitted
    // before. Returns false if the emitVertex callback aborted.
    auto emitDualPoint = [&](int32_t const cx, int32_t const cy, int32_t const cz,
      DMCEdgeCode const edge, QuadIndexType & index) -> bool {
        LayerPointKey key;
        key.cellId = cx + dimX * cy;
        key.pointCode = dualPointCodeAt(cx, cy, cz, edge);
        LayerPointMap & layerPoints = (cz == currentLayerZ) ? currLayerPoints : prevLayerPoints;
        auto const iterator = layerPoints.find(key);
        if(iterator != layerPoints.end()) {
            index = iterator->second;
            return true;
        }
        QuadIndexType const newVertexId = nextVertexId++;
        Vertex vertex;
        calculateStreamingDualPoint(cx, cy, cz, key.pointCode, vertex);
        if(!emitVertex(newVertexId, vertex)) {
            return false;
        }
        layerPoints.emplace(key, newVertexId);
        ++stats.emittedVertices;
        size_t const activeKeys = prevLayerPoints.size() + currLayerPoints.size();
        if(activeKeys > stats.peakVertexKeys) {
            stats.peakVertexKeys = activeKeys;
        }
        index = newVertexId;
        return true;
    };

    // sweep the volume plane by plane
    for(int32_t p = 0; p < dimZ; ++p) {
        // fetch the next sample plane
        if(!readPlane(p, planes.data() + size_t(p & 3) * planeSize, planeSize)) {
            return false;
        }

        // Once plane z+2 has arrived, all quads of vertex layer z can be
        // generated: cell layers z-1 and z contribute dual points and the
        // manifold correction can look at cell layers z-2 .. z+1.
        int32_t const z = p - 2;
        if(z >= 0 && z < reducedZ) {
            currentLayerZ = z;
            for(int32_t y = 0; y < reducedY; ++y)
                for(int32_t x = 0; x < reducedX; ++x) {
                    QuadIndexType i0,i1,i2,i3;

                    // construct quad for x edge
                    if(z > 0 && y > 0) {
                        bool const entering = sampleAt(x,y,z) < iso && sampleAt(x+1,y,z) >= iso;
                        bool const exiting  = sampleAt(x,y,z) >= iso && sampleAt(x+1,y,z) < iso;
                        if(entering || exiting){
                            if(!emitDualPoint(x,y,z,EDGE0,i0)) return false;
                            if(!emitDualPoint(x,y,z-1,EDGE2,i1)) return false;
                            if(!emitDualPoint(x,y-1,z-1,EDGE6,i2)) return false;
                            if(!emitDualPoint(x,y-1,z,EDGE4,i3)) return false;

                            if(entering) {
                                if(!emitQuad(Quad(i0,i1,i2,i3))) return false;
                            } else {
                                if(!emitQuad(Quad(i0,i3,i2,i1))) return false;
                            }
                            ++stats.emittedQuads;
                        }
                    }

                    // construct quad for y edge
                    if(z > 0 && x > 0) {
                        bool const entering = sampleAt(x,y,z) < iso && sampleAt(x,y+1,z) >= iso;
                        bool const exiting  = sampleAt(x,y,z) >= iso && sampleAt(x,y+1,z) < iso;
                        if(entering || exiting){
                            if(!emitDualPoint(x,y,z,EDGE8,i0)) return false;
                            if(!emitDualPoint(x,y,z-1,EDGE11,i1)) return false;
                            if(!emitDualPoint(x-1,y,z-1,EDGE10,i2)) return false;
                            if(!emitDualPoint(x-1,y,z,EDGE9,i3)) return false;

                            if(exiting) {
                                if(!emitQuad(Quad(i0,i1,i2,i3))) return false;
                            } else {
                                if(!emitQuad(Quad(i0,i3,i2,i1))) return false;
                            }
                            ++stats.emittedQuads;
                        }
                    }

                    // construct quad for z edge
                    if(x > 0 && y > 0) {
                        bool const entering = sampleAt(x,y,z) < iso && sampleAt(x,y,z+1) >= iso;
                        bool const exiting  = sampleAt(x,y,z) >= iso && sampleAt(x,y,z+1) < iso;
                        if(entering || exiting){
                            if(!emitDualPoint(x,y,z,EDGE3,i0)) return false;
                            if(!emitDualPoint(x-1,y,z,EDGE1,i1)) return false;
                            if(!emitDualPoint(x-1,y-1,z,EDGE5,i2)) return false;
                            if(!emitDualPoint(x,y-1,z,EDGE7,i3)) return false;

                            if(exiting) {
                                if(!emitQuad(Quad(i0,i1,i2,i3))) return false;
                            } else {
                                if(!emitQuad(Quad(i0,i3,i2,i1))) return false;
                            }
                            ++stats.emittedQuads;
                        }
                    }
                }

            // dual points of cell layer z-1 cannot be referenced by any
            // following quad anymore; retire their key records
            prevLayerPoints.clear();
            prevLayerPoints.swap(currLayerPoints);
        }

        // Cache the raw cell codes of the cell layer whose bottom sample
        // plane is evicted from the ring next. Its codes are still needed
        // for the manifold correction of the succeeding cell layer.
        int32_t const cacheLayer = p - 3;
        if(cacheLayer >= 0 && cacheLayer <= dimZ - 2) {
            for(int32_t cy = 0; cy < dimY - 1; ++cy)
                for(int32_t cx = 0; cx < dimX - 1; ++cx) {
                    int code = 0;
                    if(sampleAt(cx,cy,cacheLayer) >= iso)
                        code |= 1;
                    if(sampleAt(cx+1,cy,cacheLayer) >= iso)
                        code |= 2;
                    if(sampleAt(cx,cy+1,cacheLayer) >= iso)
                        code |= 4;
                    if(sampleAt(cx+1,cy+1,cacheLayer) >= iso)
                        code |= 8;
                    if(sampleAt(cx,cy,cacheLayer+1) >= iso)
                        code |= 16;
                    if(sampleAt(cx+1,cy,cacheLayer+1) >= iso)
                        code |= 32;
                    if(sampleAt(cx,cy+1,cacheLayer+1) >= iso)
                        code |= 64;
                    if(sampleAt(cx+1,cy+1,cacheLayer+1) >= iso)
                        code |= 128;
                    cellCodeCache[size_t(cx) + size_t(dimX - 1) * size_t(cy)] = uint8_t(code);
                }
            cachedCellLayer = cacheLayer;
        }
    }
    return true;
}

//------------------------------------------------------------------------------
