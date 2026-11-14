/*
  Copyright (c) 2009 Erin Catto http://www.box2d.org
  Copyright (c) 2016-2018 Lester Hedges <lester.hedges+aabbcc@gmail.com>

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

  3. This notice may not be removed or altered from any source distribution.

  This code was adapted from parts of the Box2D Physics Engine,
  http://www.box2d.org
*/

#ifndef _LIGHT_SCENE_H
#define _LIGHT_SCENE_H

#include <cstdint>
#include <set>
#include <unordered_map>
#include <vector>

#include "AABB.h"

namespace aabb
{
    /*! \brief Incremental light/object influence pairing for per-frame lighting.

        LightScene maintains the set of influence relations between local
        light sources and drawable objects for a renderer that rebuilds its
        per-frame light lists. Most objects and lights do not move between
        frames, so the scene is built around incremental updates: edits are
        staged, then a single commit() publishes the final state of a frame
        and reports exactly which (light, object) pairs appeared and which
        disappeared. This class does not compute ray intersections and it
        does not change the query semantics of the underlying aabb::Tree.

        Two worlds are tracked, each with its own ID namespace:

        - Objects: a unique uint32_t ID, a tight (minimum) world-space AABB,
          and a uint32_t mask.
        - Lights: a unique uint32_t ID (independent of the object namespace,
          so the same numeric value may be used by an object and a light),
          a world-space sphere (centre plus strictly positive radius), and
          a uint32_t mask.

        IDs must be smaller than UINT32_MAX. A mask of zero is legal; such
        an endpoint simply never pairs with anything.

        An influence relation between a light and an object exists exactly
        when (lightMask & objectMask) != 0 and the light sphere intersects
        the object's tight AABB, where touching counts as intersecting. The
        sphere/box test uses the distance from the sphere centre to the
        closest point on the tight box; neither the sphere's bounding box
        nor the fat AABBs stored inside the dynamic trees are treated as
        final geometry. The dynamic AABB trees are used as a broad phase
        only; all tight geometry is stored separately by the scene itself.

        Staging and publishing:

        - addObject / addLight / updateObject / updateLight / removeObject /
          removeLight only stage changes. They never alter the committed
          pair set visible through pairs().
        - commit() first brings both broad-phase trees up to date with the
          final staged geometry of the frame, then recomputes relations
          around the changed (dirty) endpoints only, using candidates from
          the opposite tree plus the adjacency lists of the old relations.
          It returns the added and removed pairs of this frame, each sorted
          ascending by (lightId, objectId) with every pair appearing once.
          Repeated moves of the same endpoint within a frame collapse to
          its final state, so moving out and back in produces no spurious
          events, and a pair whose two endpoints both moved is reported at
          most once.
        - Removing an object or a light dissolves all of its relations.
          Relations never reference stale endpoints: internal tree nodes
          recycled by later insertions, and external IDs reused in a later
          frame, start with no inherited relations. Each frame is reported
          as the difference between the final pair set of that frame and
          the pair set of the previous frame.
        - pairs() returns the committed pair set. It is maintained
          incrementally by commit(); reading it never recomputes anything.

        Errors: invalid dimensionality, non-finite coordinates, an inverted
        box (lower bound above upper bound), a non-positive radius, adding
        an ID that is currently alive, or modifying/removing an ID that is
        not alive all raise std::invalid_argument. A failed operation
        leaves all previously staged state untouched.
     */
    class LightScene
    {
    public:
        /*! \brief A light/object influence pair.

            Pairs are ordered lexicographically by (lightId, objectId).
         */
        struct Pair
        {
            /// Light ID (light namespace).
            uint32_t lightId;

            /// Object ID (object namespace).
            uint32_t objectId;

            //! Lexicographic ordering by (lightId, objectId).
            bool operator<(const Pair& other) const
            {
                if (lightId != other.lightId) return lightId < other.lightId;
                return objectId < other.objectId;
            }

            //! Equality comparison.
            bool operator==(const Pair& other) const
            {
                return lightId == other.lightId && objectId == other.objectId;
            }
        };

        /*! \brief Real work counters for one commit().

            The counters are incremented at the actual places where the work
            happens, never estimated:

            - treeNodeVisits: tree nodes visited by the broad-phase candidate
              queries issued against the two dynamic AABB trees.
            - pairTests: exact sphere vs. tight box influence tests.
            - adjacencyVisits: old relations enumerated from the adjacency
              lists of dirty endpoints.

            A commit with no staged changes reports zero for all three.
         */
        struct CommitStats
        {
            /// Tree nodes visited by broad-phase candidate queries.
            uint64_t treeNodeVisits = 0;

            /// Exact sphere vs. tight box tests performed.
            uint64_t pairTests = 0;

            /// Old relations enumerated from adjacency lists.
            uint64_t adjacencyVisits = 0;
        };

        /*! \brief The published result of one frame.

            added and removed are each sorted ascending by
            (lightId, objectId) and contain each pair at most once.
         */
        struct CommitResult
        {
            /// Pairs that exist now but did not exist after the last commit.
            std::vector<Pair> added;

            /// Pairs that existed after the last commit but not any more.
            std::vector<Pair> removed;

            /// Real work counters for this commit.
            CommitStats stats;
        };

        //! Constructor.
        /*! \param skinThickness
                The skin thickness used to fatten the broad-phase AABBs of
                the two internal dynamic trees, as a fraction of the AABB
                base length. This only affects broad-phase candidate
                generation, never the final relations. Must be finite and
                non-negative.

            \throw std::invalid_argument if skinThickness is not finite or
                is negative.
         */
        explicit LightScene(double skinThickness = 0.05);

        //! Stage a new object.
        /*! \param id
                The object ID. Must be < UINT32_MAX and not currently alive.

            \param lowerBound
                Lower bound of the tight world AABB (size 3, finite).

            \param upperBound
                Upper bound of the tight world AABB (size 3, finite, not
                below lowerBound in any dimension).

            \param mask
                The object mask (zero is legal).

            \throw std::invalid_argument on invalid input or if the ID is
                already alive. A failed call changes nothing.
         */
        void addObject(uint32_t id, const std::vector<double>& lowerBound,
                       const std::vector<double>& upperBound, uint32_t mask);

        //! Stage a new light.
        /*! \param id
                The light ID. Must be < UINT32_MAX and not currently alive.

            \param centre
                Centre of the world-space sphere (size 3, finite).

            \param radius
                Sphere radius. Must be finite and strictly positive.

            \param mask
                The light mask (zero is legal).

            \throw std::invalid_argument on invalid input or if the ID is
                already alive. A failed call changes nothing.
         */
        void addLight(uint32_t id, const std::vector<double>& centre,
                      double radius, uint32_t mask);

        //! Stage a move and/or mask change of an existing object.
        /*! The final staged state at commit() time is what counts: repeated
            updates within a frame collapse to the last values.

            \throw std::invalid_argument on invalid input or if the ID is
                not alive. A failed call changes nothing.
         */
        void updateObject(uint32_t id, const std::vector<double>& lowerBound,
                          const std::vector<double>& upperBound, uint32_t mask);

        //! Stage a move, radius change and/or mask change of an existing light.
        /*! \throw std::invalid_argument on invalid input or if the ID is
                not alive. A failed call changes nothing.
         */
        void updateLight(uint32_t id, const std::vector<double>& centre,
                         double radius, uint32_t mask);

        //! Stage the removal of an object, dissolving all of its relations.
        /*! \throw std::invalid_argument if the ID is not alive.
         */
        void removeObject(uint32_t id);

        //! Stage the removal of a light, dissolving all of its relations.
        /*! \throw std::invalid_argument if the ID is not alive.
         */
        void removeLight(uint32_t id);

        //! Publish the staged state of one frame.
        /*! Both broad-phase trees are first updated to the final staged
            geometry, then relations are recomputed around the dirty
            endpoints only. Stable endpoints are never re-queried and the
            old pair set is never scanned as a whole.

            \return
                The added and removed pairs of this frame plus the real
                work counters.
         */
        CommitResult commit();

        //! Read the current (committed) influence pairs.
        /*! The set is maintained incrementally by commit(); reading it is
            O(1) and never triggers recomputation. Staged but uncommitted
            changes are not visible here.

            \return
                The committed pairs, ordered by (lightId, objectId).
         */
        const std::set<Pair>& pairs() const
        {
            return pairs_;
        }

        //! Get the work counters of the most recent commit().
        const CommitStats& lastCommitStats() const
        {
            return lastStats_;
        }

    private:
        /// Staged plus committed state of one object.
        struct ObjectRec
        {
            /// Staged tight lower bound.
            std::vector<double> lower;

            /// Staged tight upper bound.
            std::vector<double> upper;

            /// Staged mask.
            uint32_t mask = 0;

            /// Whether the object is alive in the staged state.
            bool alive = false;

            /// Whether the object was alive at the last commit (in the tree).
            bool committed = false;

            /// Whether the object changed since the last commit.
            bool dirty = false;
        };

        /// Staged plus committed state of one light.
        struct LightRec
        {
            /// Staged sphere centre.
            std::vector<double> centre;

            /// Staged sphere radius.
            double radius = 0.0;

            /// Staged mask.
            uint32_t mask = 0;

            /// Whether the light is alive in the staged state.
            bool alive = false;

            /// Whether the light was alive at the last commit (in the tree).
            bool committed = false;

            /// Whether the light changed since the last commit.
            bool dirty = false;
        };

        //! Validate an external ID.
        static void validateId(uint32_t id);

        //! Validate a tight box (dimensionality, finiteness, ordering).
        static void validateBox(const std::vector<double>& lowerBound,
                                const std::vector<double>& upperBound);

        //! Validate a sphere (dimensionality, finiteness, positive radius).
        static void validateSphere(const std::vector<double>& centre, double radius);

        //! Exact influence test between two staged-alive endpoints.
        bool testPair(uint32_t lightId, uint32_t objectId) const;

        //! Insert a pair into the committed set and both adjacency lists.
        void insertPair(const Pair& pair);

        //! Erase a pair from the committed set and both adjacency lists.
        void erasePair(const Pair& pair);

        /// Skin thickness of the broad-phase trees.
        double skinThickness_;

        /// Broad-phase tree of the objects (fattened tight boxes).
        Tree objectTree_;

        /// Broad-phase tree of the lights (fattened sphere bounding boxes).
        Tree lightTree_;

        /// All object records known to the scene (staged view).
        std::unordered_map<uint32_t, ObjectRec> objects_;

        /// All light records known to the scene (staged view).
        std::unordered_map<uint32_t, LightRec> lights_;

        /// IDs of objects changed since the last commit (each once).
        std::vector<uint32_t> dirtyObjects_;

        /// IDs of lights changed since the last commit (each once).
        std::vector<uint32_t> dirtyLights_;

        /// The committed pair set, ordered by (lightId, objectId).
        std::set<Pair> pairs_;

        /// Committed adjacency: object ID -> paired light IDs.
        std::unordered_map<uint32_t, std::vector<uint32_t>> objectLights_;

        /// Committed adjacency: light ID -> paired object IDs.
        std::unordered_map<uint32_t, std::vector<uint32_t>> lightObjects_;

        /// Work counters of the most recent commit.
        CommitStats lastStats_;
    };
}

#endif /* _LIGHT_SCENE_H */
