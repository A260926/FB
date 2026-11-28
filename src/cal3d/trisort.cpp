#include <vector>
#include <stddef.h>

#include "trisort.h"

namespace {
    template<typename T>
    T* vec_data(std::vector<T>& v) {
        return v.empty() ? 0 : &v[0];
    }

    template<typename T>
    const T* vec_data(const std::vector<const T>& v) {
        return v.empty() ? 0 : &v[0];
    }

    struct Vec3f {
        float x, y, z;

        Vec3f()
            : x(0)
            , y(0)
            , z(0)
        {}

        Vec3f& operator+=(const Vec3f& rhs) {
            x += rhs.x;
            y += rhs.y;
            z += rhs.z;
            return *this;
        }

        Vec3f& operator/=(float s) {
            x /= s;
            y /= s;
            z /= z;
            return *this;
        }
    };

     Vec3f operator+(const Vec3f& lhs, const Vec3f& rhs) {
         Vec3f rv;
         rv.x = lhs.x + rhs.x;
         rv.y = lhs.y + rhs.y;
         rv.z = lhs.z + rhs.z;
         return rv;
    }

    Vec3f operator-(const Vec3f& lhs, const Vec3f& rhs) {
        Vec3f rv;
        rv.x = lhs.x - rhs.x;
        rv.y = lhs.y - rhs.y;
        rv.z = lhs.z - rhs.z;
        return rv;
    }

    Vec3f operator/(const Vec3f& v, float f) {
        Vec3f rv;
        rv.x = v.x / f;
        rv.y = v.y / f;
        rv.z = v.z / f;
        return rv;
    }

    float lengthSquared(const Vec3f& v) {
        return v.x * v.x + v.y * v.y + v.z * v.z;
    }

    float length(const Vec3f& v) {
        return sqrtf(lengthSquared(v));
    }

    float dot(const Vec3f& a, const Vec3f& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    Vec3f cross(const Vec3f& a, const Vec3f& u) {
        Vec3f rv;
        rv.x = a.y * u.z - a.z * u.y;
        rv.y = a.z * u.x - a.x * u.z;
        rv.z = a.x * u.y - a.y * u.x;
        return rv;
    }

    struct Triangle {
        unsigned short v0, v1, v2;
    };
}

struct TriangleSorter {
    TriangleSorter(unsigned triangleCount, Triangle* triangles, const float* positions, unsigned positionStride)
        : triangleCount(triangleCount)
        , triangles(triangles)
        , positions(positions)
        , positionStride(positionStride)
    {}

    void heuristicCycleBreaker() {
        // The "value" of an edge (x,y) is area(x) * coverage(y,x).
        // Coverage is some function of area(y) and the angle of
        // incidence.  "Across all of x, integrate the percentage of
        // the hemisphere covered by y".  Computing coverage is n^2
        // and not terribly cheap so just sort triangles so smaller
        // ones come first.  Thus, the topological sort will break
        // cycles from larger triangles to smaller ones, reducing
        // overall error.  There is no correct ordering given a cycle,
        // but there is a least bad ordering, and we can get close
        // without much work.

        struct SortByArea {
            SortByArea(TriangleSorter* ts)
                : ts(ts)
            {}

            bool operator()(const Triangle& lhs, const Triangle& rhs) {
                return ts->area(lhs) < ts->area(rhs);
            }

        private:
            TriangleSorter* ts;
        };
        std::stable_sort(triangles, triangles + triangleCount, SortByArea(this));
    }

    void buildDigraph() {
        edges.resize(triangleCount);
        for (unsigned a = 0; a < triangleCount; ++a) {
            for (unsigned b = 0; b < a; ++b) {
                bool a_facing_b = isFacing(a, b);
                bool b_facing_a = isFacing(b, a);
                if (a_facing_b != b_facing_a) {
                    std::vector<unsigned>& edgelist = edges[a_facing_b ? a : b];
                    unsigned edge = a_facing_b ? b : a;
                    edgelist.push_back(edge);
                }
            }
        }
    }

    void topologicalSort() {
        // assert empty?
        fastedge_storage.reserve(triangleCount);
        for (auto i = edges.begin(); i != edges.end(); ++i) {
            FastEdge fe;
            fe.begin = vec_data(*i);
            fe.end = fe.begin + i->size();
            fastedge_storage.push_back(fe);
        }
        fastedges = vec_data(fastedge_storage);

        visited_storage.resize(triangleCount);
        visited = vec_data(visited_storage);

        output_storage.resize(triangleCount);
        output = vec_data(output_storage);

        // again, iterating from smallest triangle to largest
        for (unsigned i = 0; i < triangleCount; ++i) {
            visit(i);
        }
    }

    void writeOutput() {
        assert(output == vec_data(output_storage) + output_storage.size());

        std::vector<Triangle> newTriangles;
        newTriangles.reserve(triangleCount);

        for (auto i = output_storage.rbegin(); i != output_storage.rend(); ++i) {
            newTriangles.push_back(triangles[*i]);
        }

        std::copy(newTriangles.begin(), newTriangles.end(), triangles);
    }

    // begin second algorithm

    void sortByCoverage() {
        // precalculate areas
        std::vector<float> areas(triangleCount);
        for (unsigned i = 0; i < triangleCount; ++i) {
            areas[i] = area(triangles[i]);
        }

        // precalculate normals
        std::vector<Vec3f> normals(triangleCount);
        for (unsigned i = 0; i < triangleCount; ++i) {
            normals[i] = normal(triangles[i]);
        }

        // precalculate barycenters
        std::vector<Vec3f> barycenters(triangleCount);
        for (unsigned i = 0; i < triangleCount; ++i) {
            barycenters[i] = center(triangles[i]);
        }

        std::vector<float> coverages(triangleCount);
        for (unsigned i = 0; i < triangleCount; ++i) {
            float coverage = 0;
            for (unsigned j = 0; j < triangleCount; ++j) {
                if (i == j) {
                    continue;
                }

                Vec3f d = barycenters[j] - barycenters[i];

                float c = (areas[j] * std::max(0.0f, dot(d, normals[j])) * std::max(0.0f, dot(d, normals[i]))) / (lengthSquared(d) * lengthSquared(d));
                coverage += c;
            }
            coverages[i] = coverage;
        }

        // sort triangles by reverse coverages
        struct SortByCoverage {
            SortByCoverage(float* coverages)
                : coverages(coverages)
            {}

            bool operator()(const unsigned& lhs, const unsigned& rhs) {
                return coverages[lhs] > coverages[rhs];
            }

        private:
            float* coverages;
        };
        std::vector<unsigned> triangle_sequence(triangleCount);
        for (unsigned i = 0; i < triangleCount; ++i) {
            triangle_sequence[i] = i;
        }
        std::stable_sort(triangle_sequence.begin(), triangle_sequence.end(), SortByCoverage(vec_data(coverages)));

        std::vector<Triangle> newTriangles;
        newTriangles.reserve(triangleCount);

        for (auto i = triangle_sequence.begin(); i != triangle_sequence.end(); ++i) {
            newTriangles.push_back(triangles[*i]);
        }

        std::copy(newTriangles.begin(), newTriangles.end(), triangles);
    }

    // begin third algorithm

    void sortByDistanceToCenter() {
        // calculate barycenter
        Vec3f center;
        for (unsigned i = 0; i < triangleCount; ++i) {
            center += getPosition(triangles[i].v0);
            center += getPosition(triangles[i].v1);
            center += getPosition(triangles[i].v2);
        }
        center /= (triangleCount * 3.0f);

        // signed distance from triangle plane to parallel plane containing barycenter
        std::vector<float> distances(triangleCount);
        for (unsigned i = 0; i < triangleCount; ++i) {
            distances[i] = dot(normal(triangles[i]), center - getPosition(triangles[i].v0));
        }

        // sort triangles by signed distance
        struct SortByDistance {
            SortByDistance(float* distances)
                : distances(distances)
            {}

            bool operator()(const unsigned& lhs, const unsigned& rhs) {
                return distances[lhs] > distances[rhs];
            }

        private:
            float* distances;
        };

        std::vector<unsigned> triangle_sequence(triangleCount);
        for (unsigned i = 0; i < triangleCount; ++i) {
            triangle_sequence[i] = i;
        }
        std::stable_sort(triangle_sequence.begin(), triangle_sequence.end(), SortByDistance(vec_data(distances)));

        std::vector<Triangle> newTriangles;
        newTriangles.reserve(triangleCount);

        for (auto i = triangle_sequence.begin(); i != triangle_sequence.end(); ++i) {
            newTriangles.push_back(triangles[*i]);
        }

        std::copy(newTriangles.begin(), newTriangles.end(), triangles);
    }

private:
    const Vec3f& getPosition(unsigned short i) {
        return *reinterpret_cast<const Vec3f*>(positions + i * positionStride);
    }

    Vec3f normal(const Triangle& t) {
        return cross(getPosition(t.v1) - getPosition(t.v0), getPosition(t.v2) - getPosition(t.v0));
    }

    Vec3f center(const Triangle& t) {
        return (getPosition(t.v0) + getPosition(t.v1) + getPosition(t.v2)) / 3;
    }

    float area(const Triangle& t) {
        Vec3f v0 = getPosition(t.v0);
        Vec3f v1 = getPosition(t.v1);
        Vec3f v2 = getPosition(t.v2);
        return length(cross(v1 - v0, v2 - v0)) / 2;
    }

    bool inFront(const Vec3f& p, const Triangle& t) {
        // TODO: precalculate planes
        // TODO: vectorize

        const auto& v1 = getPosition(t.v0);
        const auto& v2 = getPosition(t.v1);
        const auto& v3 = getPosition(t.v2);

        float plane_a = v1.y * (v2.z - v3.z) + v2.y * (v3.z - v1.z) + v3.y * (v1.z - v2.z);
        float plane_b = v1.z * (v2.x - v3.x) + v2.z * (v3.x - v1.x) + v3.z * (v1.x - v2.x);
        float plane_c = v1.x * (v2.y - v3.y) + v2.x * (v3.y - v1.y) + v3.x * (v1.y - v2.y);
        float plane_d =  ( v1.x * ( v2.y * v3.z - v3.y * v2.z ) +
                            v2.x * (v3.y * v1.z - v1.y * v3.z) +
                            v3.x * (v1.y * v2.z - v2.y * v1.z) );

        return plane_a * p.x + plane_b * p.y + plane_c * p.z >= plane_d;
    }

    bool isFacing(unsigned a, unsigned b) {
        return inFront(getPosition(triangles[b].v0), triangles[a])
            && inFront(getPosition(triangles[b].v1), triangles[a])
            && inFront(getPosition(triangles[b].v2), triangles[a]);
    }

    // TODO: could make this non-recursive but it should only recurse
    // triangleCount times, which ought to fit in a 1 MB stack...
    void visit(unsigned i) {
        if (visited[i]) {
            return;
        }
        visited[i] = true;
        
        FastEdge fe = fastedges[i];
        for (unsigned* e = fe.begin; e != fe.end; ++e) {
            visit(*e);
        }

        *output++ = i;
    }

    // inputs
    const unsigned triangleCount;
    Triangle* const triangles;
    const float* const positions;
    const unsigned positionStride;

    // dense adjacency list, a -> b
    // a -> b means "b behind a", thus b should be rendered first
    // worst case storage is something like triangleCount^2 * sizeof(unsigned)
    // for 100k triangles, that's 40 GB
    std::vector<std::vector<unsigned> > edges;
    struct FastEdge {
        unsigned* begin;
        unsigned* end;
    };
    std::vector<FastEdge> fastedge_storage;
    FastEdge* fastedges; // fast access
    
    std::vector<char> visited_storage;
    char* visited; // fast access

    // reversed
    std::vector<unsigned> output_storage;
    unsigned* output; // fast access
};

void sortTrianglesTopologically(
    unsigned triangleCount,
    unsigned short* indices,
    const float* positions,
    unsigned positionStride
) {
    // so we can count on there being at least one entry in all the arrays
    if (!triangleCount) {
        return;
    }

    Triangle* triangles = reinterpret_cast<Triangle*>(indices);

    TriangleSorter ts(triangleCount, triangles, positions, positionStride);
    ts.heuristicCycleBreaker();
    ts.buildDigraph();
    ts.topologicalSort();
    ts.writeOutput();
}

void sortTrianglesByCoverage(
    unsigned triangleCount,
    unsigned short* indices,
    const float* positions,
    unsigned positionStride
) {
    // so we can count on there being at least one entry in all the arrays
    if (!triangleCount) {
        return;
    }

    Triangle* triangles = reinterpret_cast<Triangle*>(indices);

    TriangleSorter ts(triangleCount, triangles, positions, positionStride);
    ts.sortByCoverage();
}

void sortTrianglesByDistanceToCenter(
    unsigned triangleCount,
    unsigned short* indices,
    const float* positions,
    unsigned positionStride
) {
    // so we can count on there being at least one entry in all the arrays
    if (!triangleCount) {
        return;
    }

    Triangle* triangles = reinterpret_cast<Triangle*>(indices);

    TriangleSorter ts(triangleCount, triangles, positions, positionStride);
    ts.sortByDistanceToCenter();
}

