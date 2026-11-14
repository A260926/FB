// Tests for aabb::LightScene, registered with the task driver via task_cases.
//
// Groups:
//   membership - full-set membership cross-checked against an independent
//                pairwise sphere/box distance model, plus input validation.
//   epochs     - per-frame diff semantics: simultaneous moves, out-and-back
//                moves, removals, ID/node reuse, sub-fat-AABB moves.
//   sparse     - incremental work scaling: a 2048-pair grid with tiny
//                per-frame changes and a deterministic random frame
//                sequence checked against a naive set difference.

#include "task_support/check.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

#include "LightScene.h"

using aabb::LightScene;
using aabb::Tree;
using Pair = LightScene::Pair;

namespace
{
    // ------------------------------------------------------------------
    // Small helpers.
    // ------------------------------------------------------------------

    std::vector<double> vec3(double x, double y, double z)
    {
        return std::vector<double>{x, y, z};
    }

    Pair mkp(uint32_t lightId, uint32_t objectId)
    {
        Pair p;
        p.lightId = lightId;
        p.objectId = objectId;
        return p;
    }

    template <typename F>
    bool throwsInvalid(F&& fn)
    {
        try { fn(); }
        catch (const std::invalid_argument&) { return true; }
        return false;
    }

    bool strictlySorted(const std::vector<Pair>& v)
    {
        for (size_t i = 1; i < v.size(); ++i)
            if (!(v[i - 1] < v[i])) return false;
        return true;
    }

    // ------------------------------------------------------------------
    // Independent naive model: brute-force all-pairs sphere/box tests.
    // ------------------------------------------------------------------

    struct NaiveObject
    {
        std::array<double, 3> lo, hi;
        uint32_t mask = 0;
    };

    struct NaiveLight
    {
        std::array<double, 3> c;
        double r = 0.0;
        uint32_t mask = 0;
    };

    bool naiveHit(const NaiveLight& L, const NaiveObject& O)
    {
        if ((L.mask & O.mask) == 0) return false;
        double d2 = 0.0;
        for (int i = 0; i < 3; ++i)
        {
            if (L.c[i] < O.lo[i]) { double d = O.lo[i] - L.c[i]; d2 += d * d; }
            else if (L.c[i] > O.hi[i]) { double d = L.c[i] - O.hi[i]; d2 += d * d; }
        }
        return d2 <= L.r * L.r;
    }

    using PairSet = std::set<Pair>;

    PairSet naivePairs(const std::map<uint32_t, NaiveLight>& lights,
                       const std::map<uint32_t, NaiveObject>& objects)
    {
        PairSet out;
        for (const auto& l : lights)
            for (const auto& o : objects)
                if (naiveHit(l.second, o.second))
                    out.insert(mkp(l.first, o.first));
        return out;
    }

    PairSet toSet(const std::vector<Pair>& v)
    {
        return PairSet(v.begin(), v.end());
    }

    PairSet setDiff(const PairSet& a, const PairSet& b)
    {
        PairSet out;
        for (const Pair& p : a)
            if (b.find(p) == b.end()) out.insert(p);
        return out;
    }

    // ------------------------------------------------------------------
    // membership: full-set cross-check against independent pairwise tests.
    // ------------------------------------------------------------------

    int membership_full_set()
    {
        LightScene scene;
        std::map<uint32_t, NaiveLight> lights;
        std::map<uint32_t, NaiveObject> objects;

        auto addL = [&](uint32_t id, double x, double y, double z, double r, uint32_t m)
        {
            scene.addLight(id, vec3(x, y, z), r, m);
            lights[id] = NaiveLight{{x, y, z}, r, m};
        };
        auto addO = [&](uint32_t id, std::array<double, 3> lo, std::array<double, 3> hi, uint32_t m)
        {
            scene.addObject(id, vec3(lo[0], lo[1], lo[2]), vec3(hi[0], hi[1], hi[2]), m);
            objects[id] = NaiveObject{lo, hi, m};
        };

        // Lights are 10 units apart along x so their fat AABBs never touch.
        addL(101, 0, 0, 0, 1.0, 0x1);
        addL(102, 10, 0, 0, 2.0, 0x2);
        addL(103, 20, 0, 0, 1.5, 0x1);
        addL(104, 30, 0, 0, 1.0, 0x0); // zero mask is legal
        addL(5, 40, 0, 0, 3.0, 0x4);   // same numeric ID as object 5 below

        addO(201, {0.5, -0.5, -0.5}, {1.5, 0.5, 0.5}, 0x1);      // hits light 101
        addO(202, {1.06, -0.05, -0.05}, {1.16, 0.05, 0.05}, 0x1);// fat AABBs overlap, tight miss
        addO(203, {11.5, 1.9, -0.1}, {11.7, 2.1, 0.1}, 0x2);     // sphere bbox hits, sphere misses
        addO(204, {21.5, -0.5, -0.5}, {22.5, 0.5, 0.5}, 0x1);    // exactly touches light 103
        addO(205, {0.9, -0.1, -0.1}, {1.2, 0.1, 0.1}, 0x2);      // geometry hits 101, mask rejects
        addO(5, {40.5, -0.5, -0.5}, {41.5, 0.5, 0.5}, 0x4);      // pairs with light 5
        addO(207, {0.2, 0.2, 0.2}, {0.8, 0.8, 0.8}, 0x0);        // zero mask is legal
        addO(208, {30.2, -0.4, -0.4}, {30.8, 0.4, 0.4}, 0xF);    // light 104 has zero mask

        LightScene::CommitResult r1 = scene.commit();

        // The full committed set matches the independent pairwise model.
        TASK_CHECK(scene.pairs() == naivePairs(lights, objects));

        // Exactly the three expected pairs were added, sorted and unique.
        PairSet expected1;
        expected1.insert(mkp(101, 201));
        expected1.insert(mkp(103, 204));
        expected1.insert(mkp(5, 5));
        TASK_CHECK(r1.added.size() == 3);
        TASK_CHECK(r1.removed.empty());
        TASK_CHECK(strictlySorted(r1.added));
        TASK_CHECK(toSet(r1.added) == expected1);

        // The required non-memberships: the fat false positive, the pair
        // where only the sphere's bounding box reaches the object, the
        // mask mismatches and the zero masks must not produce pairs.
        TASK_CHECK(scene.pairs().find(mkp(101, 202)) == scene.pairs().end());
        TASK_CHECK(scene.pairs().find(mkp(102, 203)) == scene.pairs().end());
        TASK_CHECK(scene.pairs().find(mkp(101, 205)) == scene.pairs().end());
        TASK_CHECK(scene.pairs().find(mkp(101, 207)) == scene.pairs().end());
        TASK_CHECK(scene.pairs().find(mkp(104, 208)) == scene.pairs().end());

        // Touching counts: (103, 204) is a pure contact pair.
        TASK_CHECK(scene.pairs().find(mkp(103, 204)) != scene.pairs().end());

        // Real work counters: 8 unique broad-phase candidates were precise
        // tested (5 of them rejected), no old relations existed yet.
        TASK_CHECK(r1.stats.pairTests == 8);
        TASK_CHECK(r1.stats.pairTests > scene.pairs().size());
        TASK_CHECK(r1.stats.adjacencyVisits == 0);
        TASK_CHECK(r1.stats.treeNodeVisits > 0);

        // Mask flips on existing endpoints create the previously rejected
        // pairs, verified against the naive model again.
        scene.updateObject(205, vec3(0.9, -0.1, -0.1), vec3(1.2, 0.1, 0.1), 0x1);
        objects[205].mask = 0x1;
        scene.updateLight(104, vec3(30, 0, 0), 1.0, 0x8);
        lights[104].mask = 0x8;

        LightScene::CommitResult r2 = scene.commit();
        PairSet expected2;
        expected2.insert(mkp(101, 205));
        expected2.insert(mkp(104, 208));
        TASK_CHECK(toSet(r2.added) == expected2);
        TASK_CHECK(r2.removed.empty());
        TASK_CHECK(strictlySorted(r2.added));
        TASK_CHECK(scene.pairs() == naivePairs(lights, objects));
        return 0;
    }

    int membership_invalid_inputs()
    {
        // Bad constructor arguments.
        TASK_CHECK(throwsInvalid([] { LightScene bad(-0.1); (void)bad; }));
        TASK_CHECK(throwsInvalid([] { LightScene bad(std::nan("")); (void)bad; }));

        LightScene scene;
        std::vector<double> twoLo{0, 0};
        std::vector<double> twoHi{1, 1};
        double nan = std::nan("");
        double inf = std::numeric_limits<double>::infinity();
        uint32_t maxId = 0xffffffffu;

        // Dimensionality.
        TASK_CHECK(throwsInvalid([&] { scene.addObject(1, twoLo, twoHi, 1); }));
        TASK_CHECK(throwsInvalid([&] { scene.addObject(1, vec3(0, 0, 0), twoHi, 1); }));
        TASK_CHECK(throwsInvalid([&] { scene.addLight(1, twoLo, 1.0, 1); }));

        // Non-finite values.
        TASK_CHECK(throwsInvalid([&] { scene.addObject(1, vec3(0, 0, nan), vec3(1, 1, 1), 1); }));
        TASK_CHECK(throwsInvalid([&] { scene.addObject(1, vec3(0, 0, 0), vec3(1, inf, 1), 1); }));
        TASK_CHECK(throwsInvalid([&] { scene.addLight(1, vec3(0, nan, 0), 1.0, 1); }));
        TASK_CHECK(throwsInvalid([&] { scene.addLight(1, vec3(0, 0, 0), inf, 1); }));
        TASK_CHECK(throwsInvalid([&] { scene.addLight(1, vec3(0, 0, 0), nan, 1); }));

        // Inverted box and non-positive radius.
        TASK_CHECK(throwsInvalid([&] { scene.addObject(1, vec3(1, 0, 0), vec3(0, 1, 1), 1); }));
        TASK_CHECK(throwsInvalid([&] { scene.addLight(1, vec3(0, 0, 0), 0.0, 1); }));
        TASK_CHECK(throwsInvalid([&] { scene.addLight(1, vec3(0, 0, 0), -2.0, 1); }));

        // Out-of-range IDs.
        TASK_CHECK(throwsInvalid([&] { scene.addObject(maxId, vec3(0, 0, 0), vec3(1, 1, 1), 1); }));
        TASK_CHECK(throwsInvalid([&] { scene.addLight(maxId, vec3(0, 0, 0), 1.0, 1); }));

        // Nothing was staged by any of the failed calls.
        LightScene::CommitResult r0 = scene.commit();
        TASK_CHECK(r0.added.empty());
        TASK_CHECK(r0.removed.empty());
        TASK_CHECK(scene.pairs().empty());

        // Valid endpoints.
        scene.addObject(1, vec3(0, 0, 0), vec3(1, 1, 1), 0x1);
        scene.addLight(1, vec3(0.5, 0.5, 0.5), 2.0, 0x1);

        // Duplicate adds of live IDs.
        TASK_CHECK(throwsInvalid([&] { scene.addObject(1, vec3(0, 0, 0), vec3(2, 2, 2), 1); }));
        TASK_CHECK(throwsInvalid([&] { scene.addLight(1, vec3(9, 9, 9), 1.0, 1); }));

        // Modifying or removing IDs that are not alive.
        TASK_CHECK(throwsInvalid([&] { scene.updateObject(999, vec3(0, 0, 0), vec3(1, 1, 1), 1); }));
        TASK_CHECK(throwsInvalid([&] { scene.updateLight(999, vec3(0, 0, 0), 1.0, 1); }));
        TASK_CHECK(throwsInvalid([&] { scene.removeObject(999); }));
        TASK_CHECK(throwsInvalid([&] { scene.removeLight(999); }));

        // A failed update must not clobber the staged valid state.
        TASK_CHECK(throwsInvalid([&] { scene.updateObject(1, vec3(5, 0, 0), vec3(1, 1, 1), 1); }));
        TASK_CHECK(throwsInvalid([&] { scene.updateLight(1, vec3(0, 0, 0), -1.0, 1); }));

        LightScene::CommitResult r1 = scene.commit();
        TASK_CHECK(r1.added.size() == 1);
        TASK_CHECK(r1.added[0] == mkp(1, 1));
        TASK_CHECK(scene.pairs().size() == 1);

        // Removing twice is an error and changes nothing.
        scene.removeObject(1);
        TASK_CHECK(throwsInvalid([&] { scene.removeObject(1); }));
        TASK_CHECK(throwsInvalid([&] { scene.updateObject(1, vec3(0, 0, 0), vec3(1, 1, 1), 1); }));
        LightScene::CommitResult r2 = scene.commit();
        TASK_CHECK(r2.removed.size() == 1);
        TASK_CHECK(r2.removed[0] == mkp(1, 1));
        TASK_CHECK(scene.pairs().empty());
        return 0;
    }

    // ------------------------------------------------------------------
    // epochs: per-frame diff semantics.
    // ------------------------------------------------------------------

    int epochs_both_endpoints_move()
    {
        LightScene scene;
        scene.addLight(11, vec3(0, 0, 0), 2.0, 1);
        scene.addObject(21, vec3(1, -0.5, -0.5), vec3(2, 0.5, 0.5), 1);
        scene.addLight(12, vec3(100, 0, 0), 2.0, 1);
        scene.addObject(22, vec3(80, -0.5, -0.5), vec3(81, 0.5, 0.5), 1);

        LightScene::CommitResult r1 = scene.commit();
        TASK_CHECK(r1.added.size() == 1);
        TASK_CHECK(r1.added[0] == mkp(11, 21));
        TASK_CHECK(scene.pairs().size() == 1);

        // Both endpoints of the new pair move in the same frame: the pair
        // (11, 22) appears and (11, 21) disappears, each reported once.
        scene.updateLight(11, vec3(85, 0, 0), 2.0, 1);
        scene.updateObject(22, vec3(84, -0.5, -0.5), vec3(85, 0.5, 0.5), 1);
        LightScene::CommitResult r2 = scene.commit();
        TASK_CHECK(r2.added.size() == 1);
        TASK_CHECK(r2.added[0] == mkp(11, 22));
        TASK_CHECK(r2.removed.size() == 1);
        TASK_CHECK(r2.removed[0] == mkp(11, 21));
        TASK_CHECK(scene.pairs().size() == 1);
        TASK_CHECK(scene.pairs().find(mkp(11, 22)) != scene.pairs().end());

        // Move both back in one frame.
        scene.updateLight(11, vec3(0, 0, 0), 2.0, 1);
        scene.updateObject(22, vec3(80, -0.5, -0.5), vec3(81, 0.5, 0.5), 1);
        LightScene::CommitResult r3 = scene.commit();
        TASK_CHECK(r3.added.size() == 1);
        TASK_CHECK(r3.added[0] == mkp(11, 21));
        TASK_CHECK(r3.removed.size() == 1);
        TASK_CHECK(r3.removed[0] == mkp(11, 22));

        // Both endpoints of an existing pair move together and stay paired:
        // no events at all.
        scene.updateLight(11, vec3(50, 0, 0), 2.0, 1);
        scene.updateObject(21, vec3(51, -0.5, -0.5), vec3(52, 0.5, 0.5), 1);
        LightScene::CommitResult r4 = scene.commit();
        TASK_CHECK(r4.added.empty());
        TASK_CHECK(r4.removed.empty());
        TASK_CHECK(scene.pairs().size() == 1);
        TASK_CHECK(scene.pairs().find(mkp(11, 21)) != scene.pairs().end());
        return 0;
    }

    int epochs_move_out_and_back()
    {
        LightScene scene;
        scene.addLight(31, vec3(0, 0, 0), 2.0, 1);
        scene.addLight(32, vec3(101, 0, 0), 1.5, 1);
        scene.addObject(41, vec3(1, -0.5, -0.5), vec3(2, 0.5, 0.5), 1);

        LightScene::CommitResult r1 = scene.commit();
        TASK_CHECK(r1.added.size() == 1);
        TASK_CHECK(r1.added[0] == mkp(31, 41));

        // Move out (into light 32's range) and back within one frame:
        // no events and no phantom pair with light 32.
        scene.updateObject(41, vec3(100, -0.5, -0.5), vec3(101, 0.5, 0.5), 1);
        // The staged move must not affect reads of the committed pairs.
        TASK_CHECK(scene.pairs().size() == 1);
        TASK_CHECK(scene.pairs().find(mkp(31, 41)) != scene.pairs().end());
        scene.updateObject(41, vec3(1, -0.5, -0.5), vec3(2, 0.5, 0.5), 1);
        LightScene::CommitResult r2 = scene.commit();
        TASK_CHECK(r2.added.empty());
        TASK_CHECK(r2.removed.empty());
        TASK_CHECK(scene.pairs().size() == 1);
        TASK_CHECK(scene.pairs().find(mkp(31, 41)) != scene.pairs().end());

        // Same for the light endpoint.
        scene.updateLight(31, vec3(100, 0, 0), 2.0, 1);
        scene.updateLight(31, vec3(0, 0, 0), 2.0, 1);
        LightScene::CommitResult r3 = scene.commit();
        TASK_CHECK(r3.added.empty());
        TASK_CHECK(r3.removed.empty());

        // Across two commits the move is real: out, then back.
        scene.updateObject(41, vec3(100, -0.5, -0.5), vec3(101, 0.5, 0.5), 1);
        LightScene::CommitResult r4 = scene.commit();
        TASK_CHECK(r4.removed.size() == 1);
        TASK_CHECK(r4.removed[0] == mkp(31, 41));
        TASK_CHECK(r4.added.size() == 1);
        TASK_CHECK(r4.added[0] == mkp(32, 41));

        scene.updateObject(41, vec3(1, -0.5, -0.5), vec3(2, 0.5, 0.5), 1);
        LightScene::CommitResult r5 = scene.commit();
        TASK_CHECK(r5.added.size() == 1);
        TASK_CHECK(r5.added[0] == mkp(31, 41));
        TASK_CHECK(r5.removed.size() == 1);
        TASK_CHECK(r5.removed[0] == mkp(32, 41));
        return 0;
    }

    int epochs_remove_and_id_reuse()
    {
        LightScene scene;
        for (uint32_t i = 1; i <= 4; ++i)
        {
            double x = 100.0 * (i - 1);
            scene.addLight(i, vec3(x, 0, 0), 2.0, 1);
            scene.addObject(10 + i, vec3(x + 1, -0.5, -0.5), vec3(x + 2, 0.5, 0.5), 1);
        }
        LightScene::CommitResult r1 = scene.commit();
        TASK_CHECK(r1.added.size() == 4);
        TASK_CHECK(scene.pairs().size() == 4);

        // Removing an object dissolves exactly its own relations.
        scene.removeObject(12);
        LightScene::CommitResult r2 = scene.commit();
        TASK_CHECK(r2.added.empty());
        TASK_CHECK(r2.removed.size() == 1);
        TASK_CHECK(r2.removed[0] == mkp(2, 12));
        TASK_CHECK(scene.pairs().size() == 3);

        // Removing a light dissolves exactly its own relations.
        scene.removeLight(3);
        LightScene::CommitResult r3 = scene.commit();
        TASK_CHECK(r3.removed.size() == 1);
        TASK_CHECK(r3.removed[0] == mkp(3, 13));
        TASK_CHECK(scene.pairs().size() == 2);

        // A new object reusing the freed tree nodes inherits nothing.
        scene.addObject(15, vec3(101, -0.5, -0.5), vec3(102, 0.5, 0.5), 1);
        LightScene::CommitResult r4 = scene.commit();
        TASK_CHECK(r4.added.size() == 1);
        TASK_CHECK(r4.added[0] == mkp(2, 15));
        TASK_CHECK(scene.pairs().find(mkp(2, 12)) == scene.pairs().end());

        // Reusing the deleted external ID in a later frame is legal and
        // reports only the new final relations of that ID.
        scene.addObject(12, vec3(301, -0.5, -0.5), vec3(302, 0.5, 0.5), 1);
        LightScene::CommitResult r5 = scene.commit();
        TASK_CHECK(r5.added.size() == 1);
        TASK_CHECK(r5.added[0] == mkp(4, 12));
        TASK_CHECK(scene.pairs().find(mkp(2, 12)) == scene.pairs().end());
        TASK_CHECK(scene.pairs().find(mkp(4, 12)) != scene.pairs().end());

        // Simultaneous removal of both endpoints of pairs: each pair is
        // reported exactly once.
        scene.removeLight(4);
        scene.removeObject(14);
        LightScene::CommitResult r6 = scene.commit();
        TASK_CHECK(r6.added.empty());
        TASK_CHECK(r6.removed.size() == 2);
        TASK_CHECK(strictlySorted(r6.removed));
        TASK_CHECK(r6.removed[0] == mkp(4, 12));
        TASK_CHECK(r6.removed[1] == mkp(4, 14));
        TASK_CHECK(scene.pairs().size() == 2);

        // A fresh object far away reuses freed nodes but gains no relations.
        scene.addObject(16, vec3(1000, 0, 0), vec3(1001, 1, 1), 1);
        LightScene::CommitResult r7 = scene.commit();
        TASK_CHECK(r7.added.empty());
        TASK_CHECK(r7.removed.empty());
        TASK_CHECK(scene.pairs().size() == 2);
        return 0;
    }

    int epochs_fat_boundary_moves()
    {
        LightScene scene(0.05);
        scene.addLight(51, vec3(0, 0, 0), 0.5, 1);
        scene.addObject(61, vec3(0.5, -0.5, -0.5), vec3(1.5, 0.5, 0.5), 1);

        LightScene::CommitResult r1 = scene.commit();
        TASK_CHECK(r1.added.size() == 1);
        TASK_CHECK(r1.added[0] == mkp(51, 61));

        // The object's fat AABB is [0.45,1.55]x[-0.55,0.55]^2 and the
        // light's is [-0.55,0.55]^3. Every change below stays inside those
        // boundaries, so the tree never reinserts the node; the relations
        // must still be updated.
        scene.updateObject(61, vec3(0.54, -0.5, -0.5), vec3(1.54, 0.5, 0.5), 1);
        LightScene::CommitResult r2 = scene.commit();
        TASK_CHECK(r2.added.empty());
        TASK_CHECK(r2.removed.size() == 1);
        TASK_CHECK(r2.removed[0] == mkp(51, 61));

        scene.updateObject(61, vec3(0.46, -0.5, -0.5), vec3(1.46, 0.5, 0.5), 1);
        LightScene::CommitResult r3 = scene.commit();
        TASK_CHECK(r3.added.size() == 1);
        TASK_CHECK(r3.added[0] == mkp(51, 61));
        TASK_CHECK(r3.removed.empty());

        // A nudge inside the fat boundary that keeps the relation: no events.
        scene.updateObject(61, vec3(0.49, -0.5, -0.5), vec3(1.49, 0.5, 0.5), 1);
        LightScene::CommitResult r4 = scene.commit();
        TASK_CHECK(r4.added.empty());
        TASK_CHECK(r4.removed.empty());

        scene.updateObject(61, vec3(0.52, -0.5, -0.5), vec3(1.52, 0.5, 0.5), 1);
        LightScene::CommitResult r5 = scene.commit();
        TASK_CHECK(r5.removed.size() == 1);
        TASK_CHECK(r5.removed[0] == mkp(51, 61));

        // Radius changes that stay inside the light's fat AABB.
        scene.updateLight(51, vec3(0, 0, 0), 0.55, 1);
        LightScene::CommitResult r6 = scene.commit();
        TASK_CHECK(r6.added.size() == 1);
        TASK_CHECK(r6.added[0] == mkp(51, 61));

        scene.updateLight(51, vec3(0, 0, 0), 0.5, 1);
        LightScene::CommitResult r7 = scene.commit();
        TASK_CHECK(r7.removed.size() == 1);
        TASK_CHECK(r7.removed[0] == mkp(51, 61));

        // Mask-only changes on a geometrically touching pair.
        scene.updateObject(61, vec3(0.46, -0.5, -0.5), vec3(1.46, 0.5, 0.5), 1);
        LightScene::CommitResult r8 = scene.commit();
        TASK_CHECK(r8.added.size() == 1);

        scene.updateObject(61, vec3(0.46, -0.5, -0.5), vec3(1.46, 0.5, 0.5), 0);
        LightScene::CommitResult r9 = scene.commit();
        TASK_CHECK(r9.removed.size() == 1);
        TASK_CHECK(r9.removed[0] == mkp(51, 61));

        scene.updateObject(61, vec3(0.46, -0.5, -0.5), vec3(1.46, 0.5, 0.5), 1);
        LightScene::CommitResult r10 = scene.commit();
        TASK_CHECK(r10.added.size() == 1);

        scene.updateLight(51, vec3(0, 0, 0), 0.5, 0);
        LightScene::CommitResult r11 = scene.commit();
        TASK_CHECK(r11.removed.size() == 1);

        scene.updateLight(51, vec3(0, 0, 0), 0.5, 1);
        LightScene::CommitResult r12 = scene.commit();
        TASK_CHECK(r12.added.size() == 1);
        TASK_CHECK(scene.pairs().size() == 1);
        TASK_CHECK(scene.pairs().find(mkp(51, 61)) != scene.pairs().end());

        // A commit with no staged changes does no work at all.
        LightScene::CommitResult r13 = scene.commit();
        TASK_CHECK(r13.added.empty());
        TASK_CHECK(r13.removed.empty());
        TASK_CHECK(r13.stats.treeNodeVisits == 0);
        TASK_CHECK(r13.stats.pairTests == 0);
        TASK_CHECK(r13.stats.adjacencyVisits == 0);
        return 0;
    }

    // ------------------------------------------------------------------
    // sparse: incremental work scaling.
    // ------------------------------------------------------------------

    int sparse_grid_2048_incremental()
    {
        const uint32_t N = 2048;
        LightScene scene;
        std::map<uint32_t, NaiveLight> lights;
        std::map<uint32_t, NaiveObject> objects;

        // 2048 object/light pairs on a 16x16x8 grid with spacing 100, so
        // different pairs are far apart and every change is spatially local.
        for (uint32_t i = 0; i < N; ++i)
        {
            double gx = 100.0 * (i % 16);
            double gy = 100.0 * ((i / 16) % 16);
            double gz = 100.0 * (i / 256);

            scene.addObject(i, vec3(gx + 4.0, gy - 0.5, gz - 0.5), vec3(gx + 5.0, gy + 0.5, gz + 0.5), 1);
            objects[i] = NaiveObject{{gx + 4.0, gy - 0.5, gz - 0.5}, {gx + 5.0, gy + 0.5, gz + 0.5}, 1};

            scene.addLight(i, vec3(gx + 4.5, gy, gz), 1.0, 1);
            lights[i] = NaiveLight{{gx + 4.5, gy, gz}, 1.0, 1};
        }

        // Build the full frame.
        LightScene::CommitResult r1 = scene.commit();
        TASK_CHECK(r1.added.size() == N);
        TASK_CHECK(r1.removed.empty());
        TASK_CHECK(r1.stats.pairTests == N);
        TASK_CHECK(r1.stats.adjacencyVisits == 0);
        TASK_CHECK(scene.pairs().size() == N);
        TASK_CHECK(scene.pairs() == naivePairs(lights, objects));

        // A commit without changes does no work at all.
        LightScene::CommitResult r2 = scene.commit();
        TASK_CHECK(r2.added.empty());
        TASK_CHECK(r2.removed.empty());
        TASK_CHECK(r2.stats.treeNodeVisits == 0);
        TASK_CHECK(r2.stats.pairTests == 0);
        TASK_CHECK(r2.stats.adjacencyVisits == 0);

        // Move a single object into the range of its neighbouring light.
        scene.updateObject(0, vec3(104.0, -0.5, -0.5), vec3(105.0, 0.5, 0.5), 1);
        objects[0] = NaiveObject{{104.0, -0.5, -0.5}, {105.0, 0.5, 0.5}, 1};

        LightScene::CommitResult r3 = scene.commit();

        // Exactly one pair leaves and one pair enters.
        TASK_CHECK(r3.added.size() == 1);
        TASK_CHECK(r3.added[0] == mkp(1, 0));
        TASK_CHECK(r3.removed.size() == 1);
        TASK_CHECK(r3.removed[0] == mkp(0, 0));

        // The real work counters stay tiny and local.
        TASK_CHECK(r3.stats.treeNodeVisits < 512);
        TASK_CHECK(r3.stats.pairTests < 16);
        TASK_CHECK(r3.stats.adjacencyVisits < 16);
        TASK_CHECK(r3.stats.treeNodeVisits > 0);
        TASK_CHECK(r3.stats.pairTests >= 1);
        TASK_CHECK(r3.stats.adjacencyVisits >= 1);

        // The full set is still exact.
        TASK_CHECK(scene.pairs() == naivePairs(lights, objects));
        TASK_CHECK(scene.pairs().find(mkp(1, 0)) != scene.pairs().end());
        TASK_CHECK(scene.pairs().find(mkp(1, 1)) != scene.pairs().end());
        TASK_CHECK(scene.pairs().find(mkp(0, 0)) == scene.pairs().end());

        // Steady again.
        LightScene::CommitResult r4 = scene.commit();
        TASK_CHECK(r4.stats.treeNodeVisits == 0);
        TASK_CHECK(r4.stats.pairTests == 0);
        TASK_CHECK(r4.stats.adjacencyVisits == 0);
        return 0;
    }

    // Deterministic 64-bit LCG for procedural test data.
    struct Lcg
    {
        uint64_t s;
        explicit Lcg(uint64_t seed) : s(seed) {}
        uint64_t next() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return s; }
        uint32_t below(uint32_t n) { return static_cast<uint32_t>((next() >> 33) % n); }
        double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
    };

    // Mirror of the broad-phase fat boxes kept by the trees, used to
    // independently predict the candidate pairs (and hence pairTests)
    // of a commit.
    struct FatBox
    {
        std::array<double, 3> lo{{0, 0, 0}}, hi{{0, 0, 0}};
        bool valid = false;
    };

    void fattenBox(const std::array<double, 3>& lo, const std::array<double, 3>& hi,
                   double skin, FatBox& out)
    {
        for (int i = 0; i < 3; ++i)
        {
            double size = hi[i] - lo[i];
            out.lo[i] = lo[i] - skin * size;
            out.hi[i] = hi[i] + skin * size;
        }
        out.valid = true;
    }

    // Mirror of Tree::updateParticle: keep the old fat box while it still
    // contains the new tight box.
    void updateFatBox(FatBox& fat, const std::array<double, 3>& lo,
                      const std::array<double, 3>& hi, double skin)
    {
        if (fat.valid)
        {
            bool contains = true;
            for (int i = 0; i < 3; ++i)
            {
                if (lo[i] < fat.lo[i] || hi[i] > fat.hi[i]) { contains = false; break; }
            }
            if (contains) return;
        }
        fattenBox(lo, hi, skin, fat);
    }

    bool fatOverlap(const std::array<double, 3>& aLo, const std::array<double, 3>& aHi,
                    const std::array<double, 3>& bLo, const std::array<double, 3>& bHi)
    {
        for (int i = 0; i < 3; ++i)
        {
            if (aHi[i] < bLo[i] || bHi[i] < aLo[i]) return false;
        }
        return true;
    }

    void lightBounds(const NaiveLight& l, std::array<double, 3>& lo, std::array<double, 3>& hi)
    {
        for (int i = 0; i < 3; ++i) { lo[i] = l.c[i] - l.r; hi[i] = l.c[i] + l.r; }
    }

    int sparse_random_frames_vs_naive()
    {
        const double SKIN = 0.05;
        LightScene scene(SKIN);
        Lcg lcg(0x123456789abcdefULL);

        std::map<uint32_t, NaiveObject> objs;
        std::map<uint32_t, NaiveLight> lts;
        std::map<uint32_t, FatBox> objFat, lightFat;
        std::vector<uint32_t> retiredObjs, retiredLts;
        uint32_t nextObjId = 0, nextLightId = 0;

        auto randomObject = [&]()
        {
            NaiveObject o;
            for (int k = 0; k < 3; ++k)
            {
                double c = lcg.uniform() * 60.0;
                double h = 0.25 + lcg.uniform() * 0.75;
                o.lo[k] = c - h;
                o.hi[k] = c + h;
            }
            o.mask = lcg.below(4);
            return o;
        };
        auto randomLight = [&]()
        {
            NaiveLight l;
            for (int k = 0; k < 3; ++k) l.c[k] = lcg.uniform() * 60.0;
            l.r = 0.5 + lcg.uniform() * 2.5;
            l.mask = lcg.below(4);
            return l;
        };

        // Initial population.
        for (int i = 0; i < 120; ++i)
        {
            NaiveObject o = randomObject();
            scene.addObject(nextObjId, vec3(o.lo[0], o.lo[1], o.lo[2]),
                            vec3(o.hi[0], o.hi[1], o.hi[2]), o.mask);
            objs[nextObjId++] = o;
        }
        for (int i = 0; i < 80; ++i)
        {
            NaiveLight l = randomLight();
            scene.addLight(nextLightId, vec3(l.c[0], l.c[1], l.c[2]), l.r, l.mask);
            lts[nextLightId++] = l;
        }

        // Mirror the broad-phase fat boxes exactly as the trees maintain
        // them for the dirty endpoints of one commit.
        auto mirrorFat = [&](const std::set<uint32_t>& dirtyO, const std::set<uint32_t>& dirtyL)
        {
            for (uint32_t id : dirtyO)
            {
                FatBox& fat = objFat[id];
                auto it = objs.find(id);
                if (fat.valid && it != objs.end())
                    updateFatBox(fat, it->second.lo, it->second.hi, SKIN);
                else if (fat.valid)
                    fat.valid = false;
                else if (it != objs.end())
                    fattenBox(it->second.lo, it->second.hi, SKIN, fat);
                if (it == objs.end()) objFat.erase(id);
            }
            for (uint32_t id : dirtyL)
            {
                FatBox& fat = lightFat[id];
                auto it = lts.find(id);
                std::array<double, 3> lo, hi;
                if (it != lts.end()) lightBounds(it->second, lo, hi);
                if (fat.valid && it != lts.end())
                    updateFatBox(fat, lo, hi, SKIN);
                else if (fat.valid)
                    fat.valid = false;
                else if (it != lts.end())
                    fattenBox(lo, hi, SKIN, fat);
                if (it == lts.end()) lightFat.erase(id);
            }
        };

        // Publish the initial population.
        {
            std::set<uint32_t> initO, initL;
            for (const auto& kv : objs) initO.insert(kv.first);
            for (const auto& kv : lts) initL.insert(kv.first);
            mirrorFat(initO, initL);
            scene.commit();
        }
        PairSet prevPairs = naivePairs(lts, objs);
        TASK_CHECK(scene.pairs() == prevPairs);

        for (int frame = 0; frame < 40; ++frame)
        {
            std::set<uint32_t> dirtyO, dirtyL;
            uint32_t nOps = (frame % 7 == 0) ? 0 : 1 + lcg.below(5);

            for (uint32_t op = 0; op < nOps; ++op)
            {
                uint32_t kind = lcg.below(10);
                if (kind <= 3 && !objs.empty())
                {
                    // Move (sometimes teleport) a random object.
                    auto it = objs.begin();
                    std::advance(it, lcg.below(static_cast<uint32_t>(objs.size())));
                    uint32_t id = it->first;
                    NaiveObject o = it->second;
                    for (int k = 0; k < 3; ++k)
                    {
                        double c = 0.5 * (o.lo[k] + o.hi[k]);
                        double h = 0.5 * (o.hi[k] - o.lo[k]);
                        c += (lcg.uniform() * 4.0 - 2.0);
                        if (lcg.below(6) == 0) c = lcg.uniform() * 60.0;
                        o.lo[k] = c - h;
                        o.hi[k] = c + h;
                    }
                    o.mask = lcg.below(4);
                    scene.updateObject(id, vec3(o.lo[0], o.lo[1], o.lo[2]),
                                       vec3(o.hi[0], o.hi[1], o.hi[2]), o.mask);
                    objs[id] = o;
                    dirtyO.insert(id);
                }
                else if (kind <= 6 && !lts.empty())
                {
                    // Move (sometimes teleport) a random light.
                    auto it = lts.begin();
                    std::advance(it, lcg.below(static_cast<uint32_t>(lts.size())));
                    uint32_t id = it->first;
                    NaiveLight l = it->second;
                    for (int k = 0; k < 3; ++k)
                    {
                        l.c[k] += (lcg.uniform() * 4.0 - 2.0);
                        if (lcg.below(6) == 0) l.c[k] = lcg.uniform() * 60.0;
                    }
                    l.r = 0.5 + lcg.uniform() * 2.5;
                    l.mask = lcg.below(4);
                    scene.updateLight(id, vec3(l.c[0], l.c[1], l.c[2]), l.r, l.mask);
                    lts[id] = l;
                    dirtyL.insert(id);
                }
                else if (kind == 7)
                {
                    // Add an object, sometimes reusing a retired ID.
                    if (objs.size() < 140)
                    {
                        uint32_t id;
                        if (!retiredObjs.empty() && lcg.below(2) == 0)
                        {
                            id = retiredObjs.back();
                            retiredObjs.pop_back();
                        }
                        else id = nextObjId++;
                        NaiveObject o = randomObject();
                        scene.addObject(id, vec3(o.lo[0], o.lo[1], o.lo[2]),
                                        vec3(o.hi[0], o.hi[1], o.hi[2]), o.mask);
                        objs[id] = o;
                        dirtyO.insert(id);
                    }
                }
                else if (kind == 8)
                {
                    // Add a light, sometimes reusing a retired ID.
                    if (lts.size() < 100)
                    {
                        uint32_t id;
                        if (!retiredLts.empty() && lcg.below(2) == 0)
                        {
                            id = retiredLts.back();
                            retiredLts.pop_back();
                        }
                        else id = nextLightId++;
                        NaiveLight l = randomLight();
                        scene.addLight(id, vec3(l.c[0], l.c[1], l.c[2]), l.r, l.mask);
                        lts[id] = l;
                        dirtyL.insert(id);
                    }
                }
                else
                {
                    // Remove a random endpoint.
                    if (lcg.below(2) == 0 && objs.size() > 60)
                    {
                        auto it = objs.begin();
                        std::advance(it, lcg.below(static_cast<uint32_t>(objs.size())));
                        uint32_t id = it->first;
                        scene.removeObject(id);
                        objs.erase(it);
                        retiredObjs.push_back(id);
                        dirtyO.insert(id);
                    }
                    else if (lts.size() > 40)
                    {
                        auto it = lts.begin();
                        std::advance(it, lcg.below(static_cast<uint32_t>(lts.size())));
                        uint32_t id = it->first;
                        scene.removeLight(id);
                        lts.erase(it);
                        retiredLts.push_back(id);
                        dirtyL.insert(id);
                    }
                }
            }

            // Mirror the trees' fat boxes for this frame's dirty endpoints.
            mirrorFat(dirtyO, dirtyL);

            LightScene::CommitResult res = scene.commit();
            PairSet curPairs = naivePairs(lts, objs);

            // Full-set equivalence with the naive model.
            TASK_CHECK(scene.pairs() == curPairs);

            // The reported diff matches the naive set difference.
            TASK_CHECK(toSet(res.added) == setDiff(curPairs, prevPairs));
            TASK_CHECK(toSet(res.removed) == setDiff(prevPairs, curPairs));
            TASK_CHECK(strictlySorted(res.added));
            TASK_CHECK(strictlySorted(res.removed));

            // adjacencyVisits: old relations of the dirty endpoints,
            // counted independently from the previous naive pair set.
            uint64_t expectedAdj = 0;
            for (const Pair& p : prevPairs)
            {
                if (dirtyO.count(p.objectId)) ++expectedAdj;
                if (dirtyL.count(p.lightId)) ++expectedAdj;
            }
            TASK_CHECK(res.stats.adjacencyVisits == expectedAdj);

            // pairTests: unique candidate pairs predicted independently
            // from the mirrored fat boxes.
            PairSet expectedCand;
            for (uint32_t id : dirtyO)
            {
                auto oi = objs.find(id);
                if (oi == objs.end()) continue;
                for (const auto& l : lts)
                {
                    const FatBox& fat = lightFat.at(l.first);
                    TASK_CHECK(fat.valid);
                    if (fatOverlap(oi->second.lo, oi->second.hi, fat.lo, fat.hi))
                        expectedCand.insert(mkp(l.first, id));
                }
            }
            for (uint32_t id : dirtyL)
            {
                auto li = lts.find(id);
                if (li == lts.end()) continue;
                std::array<double, 3> lo, hi;
                lightBounds(li->second, lo, hi);
                for (const auto& o : objs)
                {
                    const FatBox& fat = objFat.at(o.first);
                    TASK_CHECK(fat.valid);
                    if (fatOverlap(lo, hi, fat.lo, fat.hi))
                        expectedCand.insert(mkp(id, o.first));
                }
            }
            TASK_CHECK(res.stats.pairTests == expectedCand.size());

            // treeNodeVisits: zero exactly when no dirty endpoint is alive.
            bool anyQuery = false;
            for (uint32_t id : dirtyO) if (objs.count(id)) anyQuery = true;
            for (uint32_t id : dirtyL) if (lts.count(id)) anyQuery = true;
            if (anyQuery) TASK_CHECK(res.stats.treeNodeVisits > 0);
            else          TASK_CHECK(res.stats.treeNodeVisits == 0);

            // A commit with no staged changes does no work at all.
            if (nOps == 0)
            {
                TASK_CHECK(res.stats.treeNodeVisits == 0);
                TASK_CHECK(res.stats.pairTests == 0);
                TASK_CHECK(res.stats.adjacencyVisits == 0);
                TASK_CHECK(res.added.empty());
                TASK_CHECK(res.removed.empty());
            }

            prevPairs = curPairs;
        }
        return 0;
    }
}

extern const task_case task_cases[] = {
    {"membership", "full_set_cross_check", membership_full_set},
    {"membership", "invalid_inputs", membership_invalid_inputs},
    {"epochs", "both_endpoints_move", epochs_both_endpoints_move},
    {"epochs", "move_out_and_back", epochs_move_out_and_back},
    {"epochs", "remove_and_id_reuse", epochs_remove_and_id_reuse},
    {"epochs", "fat_boundary_moves", epochs_fat_boundary_moves},
    {"sparse", "grid_2048_incremental", sparse_grid_2048_incremental},
    {"sparse", "random_frames_vs_naive", sparse_random_frames_vs_naive},
};
extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
