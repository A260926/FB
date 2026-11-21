/*
Copyright (c) 2013 Benedikt Bitterli

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.

   2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

   3. This notice may not be removed or altered from any source
   distribution.
*/

#ifndef VOXELINTERFACES_HPP_
#define VOXELINTERFACES_HPP_

#include "VoxelOctree.hpp"
#include "math/Vec3.hpp"
#include "IntTypes.hpp"

#include <atomic>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

struct VoxelInterfaceCursorState;

/*
 * Dielectric description of one medium.  Absorption is linear RGB per unit
 * world length.  Medium 0 is always the canonical air medium (ior 1,
 * absorption 0); the entries supplied for slot 0 are validated but must
 * describe air.
 */
struct DielectricMaterial {
    float ior;
    Vec3 absorption;

    DielectricMaterial() : ior(1.0f), absorption(0.0f) {}
    DielectricMaterial(float i, const Vec3 &a) : ior(i), absorption(a) {}
};

/*
 * Double-precision vector used by the precision entry points.  Float Vec3
 * coordinates near 1 have a relative step of about 1e-7, which is not enough
 * to resolve a layer of thickness 1e-6; use these overloads when sub-micron
 * geometry matters.
 */
struct Vec3d {
    double x, y, z;

    Vec3d() : x(0.0), y(0.0), z(0.0) {}
    Vec3d(double _x, double _y, double _z) : x(_x), y(_y), z(_z) {}
};

/*
 * Read-only interface queries over an existing compact VoxelOctree.
 *
 * Ownership / lifetime
 * --------------------
 * The resource borrows the tree.  The caller must keep the VoxelOctree alive
 * and must not modify or destroy it while the resource or any cursor created
 * from it is in use.  The material table and the complete 32 bit payload to
 * medium mapping are copied into the resource at construction time.
 *
 * Geometry
 * --------
 * The octree is read directly in its compact hierarchical form.  The root is
 * the world cube [1,2]^3 (the same placement used by VoxelOctree::raymarch);
 * internally u = p - 1 places the root at [0,1]^3.  An empty leaf and every
 * point outside the root are air.  Leaves may occur at any depth, a node may
 * mix leaf and internal children, and both short and long (two-word) child
 * addresses are understood.  A coarse leaf is never subdivided.
 *
 * Traversal
 * ---------
 * A cursor keeps only O(tree depth) state.  It re-locates the current
 * neighbourhood from the root when it advances, but descends only into
 * children that the ray actually intersects, so it never scans subtrees the
 * ray misses.  Crossing into the next cell uses explicit boundary-side state
 * (crossed axis, travel sign and the exact dyadic face coordinate); there is
 * no fixed epsilon, no nextafter coordinate nudging, and VoxelOctree::raymarch
 * is not consulted.  The first ray origin is assumed not to lie on a leaf
 * boundary, and subsequent crossings are assumed not to land on edges,
 * corners or grazing faces; those degenerate cases introduce no extra rules.
 *
 * Seams between two leaves with the same medium number are skipped, so air is
 * never inserted between two touching non-air media.
 */
class VoxelInterfaces {
public:
    static const uint32 AirMedium = 0;

    /* Side of the most recently reported interface.  NearSide is the side the
     * incident ray came from (mediumBefore); FarSide is the side it was
     * heading into (mediumAfter). */
    enum Side {
        NearSide = 0,
        FarSide  = 1
    };

    /*
     * One medium-number change along a cursor ray.
     *
     *   t              ray parameter in this cursor's frame
     *   segmentStartT  parameter where the same-medium segment started
     *   segmentLength  physical length of that segment (|direction| * dt)
     *   mediumBefore   medium on the incident side
     *   mediumAfter    medium on the far side
     *   payload        raw 32 bit payload on the far side (0 for air/outside)
     *   normal         axial unit normal pointing towards the incident medium
     *   escapedRoot    true when the far side is outside the root cube
     *
     * position is the event position in world space (float convenience);
     * positionX/Y/Z repeat it in double precision.
     */
    struct InterfaceEvent {
        double t;
        double segmentStartT;
        double segmentLength;
        double positionX, positionY, positionZ;
        Vec3 position;
        uint32 mediumBefore;
        uint32 mediumAfter;
        uint32 payload;
        Vec3 normal;
        bool escapedRoot;

        InterfaceEvent()
        : t(0.0), segmentStartT(0.0), segmentLength(0.0),
          positionX(0.0), positionY(0.0), positionZ(0.0), position(),
          mediumBefore(0), mediumAfter(0), payload(0), normal(),
          escapedRoot(false) {}
    };

    /*
     * Description of one maximal same-medium segment.  The first segment is
     * available before next() is called.  A terminal segment either reaches
     * the background by escaping the root (reachedBackground) or stops at the
     * parameter window inside the root (endsAtWindow).
     */
    struct SegmentInfo {
        uint32 medium;
        uint32 payload;
        double startT;
        double endT;
        double length;
        bool endsAtInterface;
        bool reachedBackground;
        bool endsAtWindow;

        SegmentInfo()
        : medium(0), payload(0), startT(0.0), endT(0.0), length(0.0),
          endsAtInterface(false), reachedBackground(false), endsAtWindow(false) {}
    };

    /*
     * Independent traversal cursor.  Copying produces an independent
     * traversal; two cursors never share mutable state.  descriptorReads()
     * counts the actual 32 bit octree words read by this cursor.
     */
    class Cursor {
    public:
        Cursor(const Cursor &other);
        Cursor &operator=(const Cursor &other);
        Cursor(Cursor &&other);
        Cursor &operator=(Cursor &&other);
        ~Cursor();

        const VoxelInterfaces &resource() const;

        /* Medium, payload and length of the first segment.  Valid immediately
         * after begin() and before the first next(). */
        const SegmentInfo &firstSegment() const;

        /* Advance to the next medium change.  Returns false when no further
         * interface exists.  On false, check reachedBackground() to decide
         * whether the direction background should be evaluated. */
        bool next(InterfaceEvent &event);

        bool finished() const;
        bool reachedBackground() const;

        /* Unit direction at the background escape (only meaningful when
         * reachedBackground() is true). */
        Vec3 backgroundDirection() const;

        /* Segment that ended at the most recently returned event, or the
         * terminal segment once next() has returned false. */
        const SegmentInfo &lastSegment() const;

        const InterfaceEvent &lastEvent() const;
        bool hasEvent() const;

        /*
         * Fork at the most recently returned interface.  The new cursor starts
         * exactly at the interface position with local t = 0 on the requested
         * explicit side of the boundary.  Reflection uses NearSide, refraction
         * uses FarSide.  The new direction must point into the chosen side
         * (dot(direction, normal) > 0 for NearSide, < 0 for FarSide), so the
         * cursor cannot immediately re-hit the boundary it left.
         *
         * Throws std::logic_error if no interface has been returned yet and
         * std::invalid_argument for a non-finite/zero direction, an invalid
         * window, or a direction pointing out of the requested side.
         */
        Cursor branch(Side side, const Vec3 &newDirection, double tMax) const;

        uint64 descriptorReads() const;

    private:
        friend class VoxelInterfaces;
        explicit Cursor(std::unique_ptr<VoxelInterfaceCursorState> state);
        std::unique_ptr<VoxelInterfaceCursorState> _state;
    };

    /*
     * Both constructors throw std::invalid_argument when:
     *   - materials is empty, materials[0] is not air, or any other material
     *     has a non-finite ior <= 0 or a non-finite/negative absorption
     *     channel;
     *   - the payload map refers to a medium outside the material table, or
     *     maps payload 0 to a non-air medium;
     *   - a non-zero payload stored in the tree is not mapped;
     *   - the descriptor hierarchy is malformed.
     */
    VoxelInterfaces(const VoxelOctree &tree,
                    const std::vector<DielectricMaterial> &materials,
                    const std::unordered_map<uint32, uint32> &payloadMedium);

    const VoxelOctree &tree() const;
    const std::vector<DielectricMaterial> &materials() const;
    size_t mediumCount() const;

    bool hasPayload(uint32 payload) const;

    /* Payload 0 is air.  Throws std::invalid_argument for an unmapped non-zero
     * payload. */
    uint32 mediumForPayload(uint32 payload) const;

    /* Cumulative number of octree words read by all queries on this resource.
     * Initialisation validation is not counted. */
    uint64 descriptorReads() const;

    /*
     * Start a query: p(t) = origin + direction * t.  direction must be finite
     * and non-zero (it need not be unit length); t is the original ray
     * parameter.  The window [tMin,tMax] must be finite with tMax > tMin.
     * [begin(origin, direction, tMax)] uses tMin = 0.
     *
     * Throws std::invalid_argument for non-finite inputs, a zero direction or
     * an invalid window.
     */
    Cursor begin(const Vec3 &origin, const Vec3 &direction, double tMax) const;
    Cursor begin(const Vec3 &origin, const Vec3 &direction,
                 double tMin, double tMax) const;

    /* Double-precision entry points; ideally suited to sub-micron layers. */
    Cursor begin(const Vec3d &origin, const Vec3d &direction, double tMax) const;
    Cursor begin(const Vec3d &origin, const Vec3d &direction,
                 double tMin, double tMax) const;

private:
    friend struct VoxelInterfaceCursorState;

    void validateInputs();
    void validateTree();
    uint32 lookupMedium(uint32 payload) const;
    void noteDescriptorRead() const;

    const VoxelOctree *_tree;
    std::vector<DielectricMaterial> _materials;
    std::unordered_map<uint32, uint32> _payloadMedium;
    mutable std::atomic<uint64> _descriptorReads;
};

#endif /* VOXELINTERFACES_HPP_ */
