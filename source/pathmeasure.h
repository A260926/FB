#ifndef LUNASVG_PATHMEASURE_H
#define LUNASVG_PATHMEASURE_H

#include "graphics.h"

namespace lunasvg {

// Arc length measurement and arc-length parameterization for a single open
// subpath. The transform is applied to the geometry before measuring.
class PathMeasure {
public:
    PathMeasure(const Path& path, const Transform& transform);

    bool valid() const { return m_valid; }
    double length() const { return m_length; }

    // Samples the point on the path at the given arc length and its unit
    // tangent. Distances in [0, length()] are accepted; out of range or
    // non-finite distances fail without modifying the outputs.
    bool sample(double distance, Point& position, Point& unitTangent) const;

private:
    struct Segment {
        Point p0;
        Point p1;
        Point p2;
        Point p3;
        bool line;
        double length;      // arc length of this segment
        double start;       // cumulative arc length at the segment start
    };

    bool m_valid = false;
    double m_length = 0.0;
    std::vector<Segment> m_segments;
};

} // namespace lunasvg

#endif // LUNASVG_PATHMEASURE_H
