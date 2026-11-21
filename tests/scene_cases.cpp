//
// scene_cases.cpp
//
// Regression cases for the incremental VoxelScene.  Every deformation batch is
// checked three ways: the returned patch is replayed into a local map and must
// match snapshot(), the scene exports are compared against the original
// vx_voxelize_pc / vx_voxelize outputs on the same mesh state, and the work
// report (triangleIds, colorTriangleIds, candidateTests) is checked against
// direct single-triangle enumeration.
//

#include "voxelizer.h"
#include "voxel_scene.hpp"
#include "task_support/check.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <vector>

using namespace voxel_scene;

namespace {

using Key = CellKey;

struct MeshHolder {
    vx_mesh_t* m;
    explicit MeshHolder(vx_mesh_t* mesh) : m(mesh) {}
    ~MeshHolder() { if (m) { vx_mesh_free(m); } }
};

vx_mesh_t* makeMesh(unsigned nvertices, unsigned nindices, bool colored)
{
    vx_mesh_t* m = vx_mesh_alloc((int) nvertices, (int) nindices);
    if (!colored) {
        std::free(m->colors);
        m->colors = NULL;
    }
    return m;
}

void setVertex(vx_mesh_t* m, unsigned i,
               float x, float y, float z,
               float r, float g, float b)
{
    m->vertices[i].x = x;
    m->vertices[i].y = y;
    m->vertices[i].z = z;
    if (m->colors) {
        m->colors[i].r = r;
        m->colors[i].g = g;
        m->colors[i].b = b;
    }
}

Key keyOfCenter(vx_vertex_t const& v, float const step[3])
{
    return Key{{ (int) std::lround(v.x / step[0]),
                 (int) std::lround(v.y / step[1]),
                 (int) std::lround(v.z / step[2]) }};
}

bool colorEquals(vx_color_t const& a, vx_color_t const& b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

// Count the triangle/box tests a triangle currently costs, without a
// callback: this is the same shared entry the scene uses internally.
unsigned long long directTests(vx_mesh_t const* m, unsigned triangle,
                               float const step[3], float precision)
{
    unsigned i0 = m->indices[3 * triangle + 0];
    unsigned i1 = m->indices[3 * triangle + 1];
    unsigned i2 = m->indices[3 * triangle + 2];
    vx_vertex_t p[3] = { m->vertices[i0], m->vertices[i1], m->vertices[i2] };
    int degenerate = 0;
    return vx__tri_enumerate_cells(p, step, precision, NULL, NULL, &degenerate);
}

using Replay = std::map<Key, VoxelRecord>;

void replayPatch(Replay& replay, VoxelPatch const& patch)
{
    for (Key const& key : patch.remove) {
        replay.erase(key);
    }
    for (VoxelRecord const& record : patch.upsert) {
        replay[record.key] = record;
    }
}

int checkReplay(Replay const& replay, VoxelScene& scene)
{
    std::vector<VoxelRecord> snap = scene.snapshot();
    TASK_CHECK(snap.size() == replay.size());
    TASK_CHECK(std::is_sorted(snap.begin(), snap.end(),
        [](VoxelRecord const& a, VoxelRecord const& b) { return a.key < b.key; }));
    std::size_t i = 0;
    for (Replay::const_iterator it = replay.begin(); it != replay.end(); ++it, ++i) {
        TASK_CHECK(it->first == snap[i].key);
        TASK_CHECK(it->second.winnerTriangle == snap[i].winnerTriangle);
        TASK_CHECK(it->second.contributorCount == snap[i].contributorCount);
        if (scene.colored()) {
            TASK_CHECK(colorEquals(it->second.color, snap[i].color));
        }
    }
    return 0;
}

// Full comparison against the original public voxelizers on the same state.
int checkAgainstReference(VoxelScene& scene, vx_mesh_t const* ref,
                          float sx, float sy, float sz, float precision)
{
    float const step[3] = { sx, sy, sz };

    vx_point_cloud_t* pc = vx_voxelize_pc(ref, sx, sy, sz, precision);
    TASK_CHECK(pc != NULL);
    TASK_CHECK((pc->colors != NULL) == scene.colored());
    std::map<Key, vx_color_t> refCloud;
    for (std::size_t i = 0; i < pc->nvertices; ++i) {
        Key k = keyOfCenter(pc->vertices[i], step);
        vx_color_t c = pc->colors ? pc->colors[i] : vx_color_t{};
        TASK_CHECK(refCloud.insert(std::make_pair(k, c)).second);
    }

    std::vector<VoxelRecord> snap = scene.snapshot();
    TASK_CHECK(snap.size() == pc->nvertices);
    for (VoxelRecord const& record : snap) {
        std::map<Key, vx_color_t>::const_iterator it = refCloud.find(record.key);
        TASK_CHECK(it != refCloud.end());
        if (scene.colored()) {
            TASK_CHECK(colorEquals(record.color, it->second));
        }
    }

    vx_point_cloud_t* scenePc = scene.pointCloud();
    TASK_CHECK(scenePc != NULL);
    TASK_CHECK(scenePc->nvertices == pc->nvertices);
    TASK_CHECK((scenePc->colors != NULL) == scene.colored());
    std::map<Key, int> sceneCloud;
    for (std::size_t i = 0; i < scenePc->nvertices; ++i) {
        TASK_CHECK(sceneCloud.insert(std::make_pair(
            keyOfCenter(scenePc->vertices[i], step), 1)).second);
        if (scene.colored()) {
            TASK_CHECK(colorEquals(scenePc->colors[i], refCloud.at(
                keyOfCenter(scenePc->vertices[i], step))));
        }
    }
    vx_point_cloud_free(scenePc);
    vx_point_cloud_free(pc);

    vx_mesh_t* cubes = vx_voxelize(ref, sx, sy, sz, precision);
    vx_mesh_t* sceneCubes = scene.cubeMesh();
    TASK_CHECK(cubes != NULL && sceneCubes != NULL);
    std::size_t voxels = scene.voxelCount();
    TASK_CHECK(sceneCubes->nvertices == voxels * 8);
    TASK_CHECK(sceneCubes->nindices == voxels * VOXELIZER_INDICES_SIZE);
    TASK_CHECK(sceneCubes->nnormals == VOXELIZER_NORMAL_INDICES_SIZE);
    TASK_CHECK(sceneCubes->nvertices == cubes->nvertices);
    TASK_CHECK(sceneCubes->nindices == cubes->nindices);
    TASK_CHECK((sceneCubes->colors != NULL) == scene.colored());
    TASK_CHECK((cubes->colors != NULL) == scene.colored());
    for (int n = 0; n < 18; ++n) {
        TASK_CHECK(sceneCubes->normals[n / 3].v[n % 3] ==
                   cubes->normals[n / 3].v[n % 3]);
    }

    std::map<Key, vx_color_t> refCubes;
    for (std::size_t b = 0; b < voxels; ++b) {
        vx_vertex_t const& corner = cubes->vertices[8 * b + 0];
        vx_vertex_t center;
        center.x = corner.x + sx * 0.5f;
        center.y = corner.y - sy * 0.5f;
        center.z = corner.z - sz * 0.5f;
        Key k = keyOfCenter(center, step);
        vx_color_t blockColor = cubes->colors ? cubes->colors[8 * b]
                                              : vx_color_t{};
        TASK_CHECK(refCubes.insert(std::make_pair(k, blockColor)).second);
    }
    for (std::size_t b = 0; b < voxels; ++b) {
        vx_vertex_t const& corner = sceneCubes->vertices[8 * b + 0];
        vx_vertex_t center;
        center.x = corner.x + sx * 0.5f;
        center.y = corner.y - sy * 0.5f;
        center.z = corner.z - sz * 0.5f;
        Key k = keyOfCenter(center, step);
        TASK_CHECK(refCubes.count(k) == 1);
        static float const offsets[24] = {
            -0.5f,  0.5f,  0.5f,
            -0.5f, -0.5f,  0.5f,
             0.5f, -0.5f,  0.5f,
             0.5f,  0.5f,  0.5f,
            -0.5f,  0.5f, -0.5f,
            -0.5f, -0.5f, -0.5f,
             0.5f, -0.5f, -0.5f,
             0.5f,  0.5f, -0.5f,
        };
        for (int j = 0; j < 8; ++j) {
            vx_vertex_t const& v = sceneCubes->vertices[8 * b + j];
            TASK_CHECK(v.x == center.x + offsets[3 * j + 0] * sx);
            TASK_CHECK(v.y == center.y + offsets[3 * j + 1] * sy);
            TASK_CHECK(v.z == center.z + offsets[3 * j + 2] * sz);
        }
        if (scene.colored()) {
            TASK_CHECK(colorEquals(refCubes.at(k), sceneCubes->colors[8 * b]));
            for (int j = 0; j < 8; ++j) {
                TASK_CHECK(colorEquals(sceneCubes->colors[8 * b],
                                       sceneCubes->colors[8 * b + j]));
            }
        }
    }
    std::map<Key, int> sceneCubeKeys;
    for (std::size_t b = 0; b < voxels; ++b) {
        vx_vertex_t const& corner = sceneCubes->vertices[8 * b + 0];
        vx_vertex_t center;
        center.x = corner.x + sx * 0.5f;
        center.y = corner.y - sy * 0.5f;
        center.z = corner.z - sz * 0.5f;
        TASK_CHECK(sceneCubeKeys.insert(std::make_pair(keyOfCenter(center, step), 1)).second);
        TASK_CHECK(refCubes.count(keyOfCenter(center, step)) == 1);
    }
    vx_mesh_free(cubes);
    vx_mesh_free(sceneCubes);

    return 0;
}

int checkSortedUnique(std::vector<unsigned int> const& v)
{
    TASK_CHECK(std::is_sorted(v.begin(), v.end()));
    for (std::size_t i = 1; i < v.size(); ++i) {
        TASK_CHECK(v[i - 1] != v[i]);
    }
    return 0;
}

// Two triangles fully overlapping a single unit cell at the origin.
vx_mesh_t* redBlueMesh(bool colored)
{
    vx_mesh_t* m = makeMesh(6, 6, colored);
    for (unsigned t = 0; t < 2; ++t) {
        float const xy[3][2] = {{ .05f, .05f }, { .20f, .05f }, { .05f, .20f }};
        for (int j = 0; j < 3; ++j) {
            unsigned i = 3 * t + j;
            m->indices[i] = i;
            float r = colored ? (t == 0 ? 1.0f : 0.0f) : 0.0f;
            float b = colored ? (t == 0 ? 0.0f : 1.0f) : 0.0f;
            setVertex(m, i, xy[j][0], xy[j][1], 0.1f, r, 0.0f, b);
        }
    }
    return m;
}

int analyticSingleCellWinner(VoxelScene& scene)
{
    std::vector<VoxelRecord> snap = scene.snapshot();
    TASK_CHECK(snap.size() == 1);
    TASK_CHECK((snap[0].key == Key{{ 0, 0, 0 }}));
    TASK_CHECK(snap[0].winnerTriangle == 0);
    TASK_CHECK(snap[0].contributorCount == 2);
    if (scene.colored()) {
        TASK_CHECK(snap[0].color.r == 1.0f);
        TASK_CHECK(snap[0].color.g == 0.0f);
        TASK_CHECK(snap[0].color.b == 0.0f);
    }
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// dependencies: batching, shared-vertex merging and work locality
// ---------------------------------------------------------------------------

static int case_shared_vertex(void)
{
    float const step[3] = { 0.25f, 0.25f, 0.25f };
    const float precision = 0.0f;
    vx_mesh_t* m = makeMesh(4, 6, true);
    m->indices[0] = 0; m->indices[1] = 1; m->indices[2] = 2;
    m->indices[3] = 0; m->indices[4] = 2; m->indices[5] = 3;
    setVertex(m, 0, .05f, .05f, .1f, 1.f, 0.f, 0.f);
    setVertex(m, 1, .40f, .05f, .1f, 1.f, 0.f, 0.f);
    setVertex(m, 2, .05f, .40f, .1f, 0.f, 1.f, 0.f);
    setVertex(m, 3, .40f, .40f, .1f, 0.f, 0.f, 1.f);
    MeshHolder holder(m);

    VoxelScene scene(m, step[0], step[1], step[2], precision);
    Replay replay;
    for (VoxelRecord const& r : scene.snapshot()) { replay[r.key] = r; }
    TASK_CHECK(checkReplay(replay, scene) == 0);
    TASK_CHECK(checkAgainstReference(scene, m, step[0], step[1], step[2], precision) == 0);

    // The shared vertex 0 appears three times: the last record must win, and
    // both incident triangles are rasterized exactly once each.
    std::vector<VertexUpdate> batch;
    batch.push_back(VertexUpdate{ 0, vx_vertex_t{{{ .30f, .30f, .1f }}}, vx_color_t{{{ 1.f, 1.f, 0.f }}} });
    batch.push_back(VertexUpdate{ 0, vx_vertex_t{{{ .10f, .10f, .1f }}}, vx_color_t{{{ 1.f, 1.f, 0.f }}} });
    batch.push_back(VertexUpdate{ 0, vx_vertex_t{{{ .22f, .18f, .1f }}}, vx_color_t{{{ 1.f, 1.f, 0.f }}} });
    batch.push_back(VertexUpdate{ 2, vx_vertex_t{{{ .05f, .45f, .1f }}}, vx_color_t{{{ 0.f, 1.f, 0.f }}} });
    VoxelPatch patch = scene.apply(batch);

    TASK_CHECK(checkSortedUnique(patch.work.triangleIds) == 0);
    TASK_CHECK(patch.work.triangleIds.size() == 2);
    TASK_CHECK(patch.work.triangleIds[0] == 0);
    TASK_CHECK(patch.work.triangleIds[1] == 1);
    TASK_CHECK(patch.work.colorTriangleIds.empty());

    // Mirror the final merged values into the reference mesh and prove the
    // reported test count is local to the two moved triangles.
    m->vertices[0] = vx_vertex_t{{{ .22f, .18f, .1f }}};
    m->colors[0] = vx_color_t{{{ 1.f, 1.f, 0.f }}};
    m->vertices[2] = vx_vertex_t{{{ .05f, .45f, .1f }}};
    unsigned long long expected = directTests(m, 0, step, precision)
                                + directTests(m, 1, step, precision);
    TASK_CHECK(patch.work.candidateTests == expected);
    TASK_CHECK(patch.work.candidateTests > 0);

    replayPatch(replay, patch);
    TASK_CHECK(checkReplay(replay, scene) == 0);
    TASK_CHECK(checkAgainstReference(scene, m, step[0], step[1], step[2], precision) == 0);
    return 0;
}

static int case_duplicate_last_wins_and_empty(void)
{
    float const step[3] = { 0.5f, 0.5f, 0.5f };
    const float precision = 0.0f;
    vx_mesh_t* m = redBlueMesh(true);
    MeshHolder holder(m);
    VoxelScene scene(m, step[0], step[1], step[2], precision);
    Replay replay;
    for (VoxelRecord const& r : scene.snapshot()) { replay[r.key] = r; }

    // Last of three records for vertex 0 is the only effective one; it only
    // changes the color here.
    std::vector<VertexUpdate> batch;
    batch.push_back(VertexUpdate{ 0, m->vertices[0], vx_color_t{{{ 9.f, 9.f, 9.f }}} });
    batch.push_back(VertexUpdate{ 0, m->vertices[0], vx_color_t{{{ 2.f, 2.f, 2.f }}} });
    batch.push_back(VertexUpdate{ 0, m->vertices[0], vx_color_t{{{ .7f, .1f, .2f }}} });
    VoxelPatch patch = scene.apply(batch);
    TASK_CHECK(patch.work.triangleIds.empty());
    TASK_CHECK(patch.work.candidateTests == 0);
    TASK_CHECK(patch.work.colorTriangleIds.size() == 1);
    TASK_CHECK(patch.work.colorTriangleIds[0] == 0);

    m->colors[0] = vx_color_t{{{ .7f, .1f, .2f }}};
    replayPatch(replay, patch);
    TASK_CHECK(checkReplay(replay, scene) == 0);
    TASK_CHECK(checkAgainstReference(scene, m, step[0], step[1], step[2], precision) == 0);

    // An empty batch and a no-op batch must do nothing at all.
    VoxelPatch emptyPatch = scene.apply(std::vector<VertexUpdate>());
    TASK_CHECK(emptyPatch.upsert.empty() && emptyPatch.remove.empty());
    TASK_CHECK(emptyPatch.work.triangleIds.empty());
    TASK_CHECK(emptyPatch.work.colorTriangleIds.empty());
    TASK_CHECK(emptyPatch.work.candidateTests == 0);

    std::vector<VertexUpdate> noop;
    noop.push_back(VertexUpdate{ 1, m->vertices[1], m->colors[1] });
    VoxelPatch noopPatch = scene.apply(noop);
    TASK_CHECK(noopPatch.upsert.empty() && noopPatch.remove.empty());
    TASK_CHECK(noopPatch.work.candidateTests == 0);

    // Two conflicting position records for one vertex whose final value is
    // the current one: any intermediate move must not leak into the patch.
    std::vector<VertexUpdate> hidden;
    vx_vertex_t away = m->vertices[0];
    away.x += 3.0f;
    hidden.push_back(VertexUpdate{ 0, away, m->colors[0] });
    hidden.push_back(VertexUpdate{ 0, m->vertices[0], m->colors[0] });
    VoxelPatch hiddenPatch = scene.apply(hidden);
    TASK_CHECK(hiddenPatch.upsert.empty() && hiddenPatch.remove.empty());
    TASK_CHECK(hiddenPatch.work.triangleIds.empty());
    TASK_CHECK(hiddenPatch.work.colorTriangleIds.empty());
    TASK_CHECK(hiddenPatch.work.candidateTests == 0);
    TASK_CHECK(checkReplay(replay, scene) == 0);
    return 0;
}

static int case_color_change_zero_tests(void)
{
    float const step[3] = { 1.f, 1.f, 1.f };
    const float precision = 0.0f;
    vx_mesh_t* m = redBlueMesh(true);
    MeshHolder holder(m);
    VoxelScene scene(m, step[0], step[1], step[2], precision);
    Replay replay;
    for (VoxelRecord const& r : scene.snapshot()) { replay[r.key] = r; }
    TASK_CHECK(analyticSingleCellWinner(scene) == 0);

    // Recolor every vertex of the winning red triangle, including a value
    // outside [0,1].
    std::vector<VertexUpdate> batch;
    for (int j = 0; j < 3; ++j) {
        batch.push_back(VertexUpdate{ (unsigned) j, m->vertices[j],
            vx_color_t{{{ 0.25f, -1.5f, 3.0f }}} });
    }
    VoxelPatch patch = scene.apply(batch);
    TASK_CHECK(patch.work.triangleIds.empty());
    TASK_CHECK(patch.work.candidateTests == 0);
    TASK_CHECK(patch.work.colorTriangleIds.size() == 1);
    TASK_CHECK(patch.work.colorTriangleIds[0] == 0);
    TASK_CHECK(patch.upsert.size() == 1);
    TASK_CHECK((patch.upsert[0].key == Key{{ 0, 0, 0 }}));
    TASK_CHECK(patch.upsert[0].winnerTriangle == 0);
    TASK_CHECK(patch.upsert[0].contributorCount == 2);

    for (int j = 0; j < 3; ++j) {
        m->colors[j] = vx_color_t{{{ 0.25f, -1.5f, 3.0f }}};
    }
    replayPatch(replay, patch);
    TASK_CHECK(checkReplay(replay, scene) == 0);
    TASK_CHECK(checkAgainstReference(scene, m, step[0], step[1], step[2], precision) == 0);
    return 0;
}

static int case_far_component_locality(void)
{
    float const step[3] = { 1.f, 1.f, 1.f };
    const float precision = 0.0f;
    vx_mesh_t* m = makeMesh(6, 6, false);
    float const nearXY[3][2] = {{ .05f, .05f }, { .40f, .05f }, { .05f, .40f }};
    float const farXY[3][2] = {{ 40.05f, 20.05f }, { 40.40f, 20.05f }, { 40.05f, 20.40f }};
    for (unsigned t = 0; t < 2; ++t) {
        for (int j = 0; j < 3; ++j) {
            unsigned i = 3 * t + j;
            m->indices[i] = i;
            float const (*xy)[2] = t == 0 ? &nearXY[0] : &farXY[0];
            setVertex(m, i, xy[j][0], xy[j][1], 0.1f, 0, 0, 0);
        }
    }
    MeshHolder holder(m);
    VoxelScene scene(m, step[0], step[1], step[2], precision);
    Replay replay;
    for (VoxelRecord const& r : scene.snapshot()) { replay[r.key] = r; }

    std::vector<Key> farKeysBefore;
    for (VoxelRecord const& r : scene.snapshot()) {
        if (r.key[0] >= 40) { farKeysBefore.push_back(r.key); }
    }
    TASK_CHECK(!farKeysBefore.empty());

    std::vector<VertexUpdate> batch;
    batch.push_back(VertexUpdate{ 0, vx_vertex_t{{{ .60f, .05f, .1f }}}, vx_color_t{} });
    VoxelPatch patch = scene.apply(batch);
    TASK_CHECK(patch.work.triangleIds.size() == 1);
    TASK_CHECK(patch.work.triangleIds[0] == 0);
    TASK_CHECK(patch.work.colorTriangleIds.empty());

    m->vertices[0] = vx_vertex_t{{{ .60f, .05f, .1f }}};
    TASK_CHECK(patch.work.candidateTests == directTests(m, 0, step, precision));

    for (Key const& k : farKeysBefore) {
        for (VoxelRecord const& r : patch.upsert) { TASK_CHECK(r.key != k); }
        for (Key const& k2 : patch.remove) { TASK_CHECK(k2 != k); }
    }
    replayPatch(replay, patch);
    TASK_CHECK(checkReplay(replay, scene) == 0);
    TASK_CHECK(checkAgainstReference(scene, m, step[0], step[1], step[2], precision) == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// contributors: winner migration, degeneracy, restoration and color handoff
// ---------------------------------------------------------------------------

static int case_winner_moves_away(void)
{
    float const step[3] = { 1.f, 1.f, 1.f };
    const float precision = 0.0f;
    vx_mesh_t* m = redBlueMesh(true);
    MeshHolder holder(m);
    VoxelScene scene(m, step[0], step[1], step[2], precision);
    Replay replay;
    for (VoxelRecord const& r : scene.snapshot()) { replay[r.key] = r; }
    TASK_CHECK(analyticSingleCellWinner(scene) == 0);

    std::vector<VertexUpdate> batch;
    for (int j = 0; j < 3; ++j) {
        vx_vertex_t p = m->vertices[j];
        p.x += 4.0f;
        batch.push_back(VertexUpdate{ (unsigned) j, p, m->colors[j] });
    }
    VoxelPatch patch = scene.apply(batch);
    TASK_CHECK(patch.remove.empty());
    TASK_CHECK(patch.upsert.size() == 2);
    TASK_CHECK(patch.work.triangleIds.size() == 1);
    TASK_CHECK(patch.work.triangleIds[0] == 0);
    TASK_CHECK(patch.work.candidateTests > 0);

    for (int j = 0; j < 3; ++j) { m->vertices[j].x += 4.0f; }
    replayPatch(replay, patch);
    TASK_CHECK(checkReplay(replay, scene) == 0);

    std::vector<VoxelRecord> snap = scene.snapshot();
    TASK_CHECK(snap.size() == 2);
    VoxelRecord const& origin = snap[0].key == Key{{ 0, 0, 0 }} ? snap[0] : snap[1];
    VoxelRecord const& moved  = snap[0].key == Key{{ 4, 0, 0 }} ? snap[0] : snap[1];
    TASK_CHECK((origin.key == Key{{ 0, 0, 0 }}));
    TASK_CHECK(origin.winnerTriangle == 1 && origin.contributorCount == 1);
    TASK_CHECK(origin.color.b == 1.0f && origin.color.r == 0.0f);
    TASK_CHECK((moved.key == Key{{ 4, 0, 0 }}));
    TASK_CHECK(moved.winnerTriangle == 0 && moved.contributorCount == 1);
    TASK_CHECK(moved.color.r == 1.0f);

    TASK_CHECK(checkAgainstReference(scene, m, step[0], step[1], step[2], precision) == 0);
    return 0;
}

static int case_winner_degenerates(void)
{
    float const step[3] = { 1.f, 1.f, 1.f };
    const float precision = 0.0f;
    vx_mesh_t* m = redBlueMesh(true);
    MeshHolder holder(m);
    VoxelScene scene(m, step[0], step[1], step[2], precision);
    Replay replay;
    for (VoxelRecord const& r : scene.snapshot()) { replay[r.key] = r; }

    // Collapse the red triangle onto a single far point: it becomes
    // degenerate, contributes nothing and must run zero overlap tests.
    std::vector<VertexUpdate> batch;
    for (int j = 0; j < 3; ++j) {
        batch.push_back(VertexUpdate{ (unsigned) j,
            vx_vertex_t{{{ 5.f, 5.f, .1f }}}, m->colors[j] });
    }
    VoxelPatch patch = scene.apply(batch);
    TASK_CHECK(patch.work.triangleIds.size() == 1);
    TASK_CHECK(patch.work.triangleIds[0] == 0);
    TASK_CHECK(patch.work.candidateTests == 0);
    TASK_CHECK(patch.upsert.size() == 1);
    TASK_CHECK((patch.upsert[0].key == Key{{ 0, 0, 0 }}));
    TASK_CHECK(patch.upsert[0].winnerTriangle == 1);
    TASK_CHECK(patch.upsert[0].contributorCount == 1);
    TASK_CHECK(patch.upsert[0].color.b == 1.0f);

    for (int j = 0; j < 3; ++j) {
        m->vertices[j] = vx_vertex_t{{{ 5.f, 5.f, .1f }}};
    }
    replayPatch(replay, patch);
    TASK_CHECK(checkReplay(replay, scene) == 0);
    TASK_CHECK(checkAgainstReference(scene, m, step[0], step[1], step[2], precision) == 0);
    return 0;
}

static int case_winner_restored(void)
{
    float const step[3] = { 1.f, 1.f, 1.f };
    const float precision = 0.0f;
    vx_mesh_t* m = redBlueMesh(true);
    MeshHolder holder(m);
    VoxelScene scene(m, step[0], step[1], step[2], precision);
    Replay replay;
    for (VoxelRecord const& r : scene.snapshot()) { replay[r.key] = r; }

    float const xy[3][2] = {{ .05f, .05f }, { .20f, .05f }, { .05f, .20f }};
    std::vector<VertexUpdate> away;
    for (int j = 0; j < 3; ++j) {
        away.push_back(VertexUpdate{ (unsigned) j,
            vx_vertex_t{{{ xy[j][0] + 4.f, xy[j][1], .1f }}}, m->colors[j] });
    }
    VoxelPatch first = scene.apply(away);
    for (int j = 0; j < 3; ++j) { m->vertices[j].x += 4.0f; }
    replayPatch(replay, first);
    TASK_CHECK(checkReplay(replay, scene) == 0);

    // Move the winner back: the shared cell must return to red with two
    // contributors and the moved cell must disappear.
    std::vector<VertexUpdate> back;
    for (int j = 0; j < 3; ++j) {
        back.push_back(VertexUpdate{ (unsigned) j,
            vx_vertex_t{{{ xy[j][0], xy[j][1], .1f }}}, m->colors[j] });
    }
    VoxelPatch second = scene.apply(back);
    TASK_CHECK(second.remove.size() == 1);
    TASK_CHECK((second.remove[0] == Key{{ 4, 0, 0 }}));
    TASK_CHECK(second.upsert.size() == 1);
    TASK_CHECK((second.upsert[0].key == Key{{ 0, 0, 0 }}));
    TASK_CHECK(second.upsert[0].winnerTriangle == 0);
    TASK_CHECK(second.upsert[0].contributorCount == 2);
    TASK_CHECK(second.upsert[0].color.r == 1.0f);

    for (int j = 0; j < 3; ++j) {
        m->vertices[j] = vx_vertex_t{{{ xy[j][0], xy[j][1], .1f }}};
    }
    replayPatch(replay, second);
    TASK_CHECK(checkReplay(replay, scene) == 0);
    TASK_CHECK(analyticSingleCellWinner(scene) == 0);
    TASK_CHECK(checkAgainstReference(scene, m, step[0], step[1], step[2], precision) == 0);
    return 0;
}

static int case_nonwinner_recolor_then_winner_leaves(void)
{
    float const step[3] = { 1.f, 1.f, 1.f };
    const float precision = 0.0f;
    vx_mesh_t* m = redBlueMesh(true);
    MeshHolder holder(m);
    VoxelScene scene(m, step[0], step[1], step[2], precision);
    Replay replay;
    for (VoxelRecord const& r : scene.snapshot()) { replay[r.key] = r; }

    // First recolor the non-winning blue triangle to green: the red winner
    // still owns the only cell, so nothing about it changes.
    std::vector<VertexUpdate> recolor;
    for (int j = 3; j < 6; ++j) {
        recolor.push_back(VertexUpdate{ (unsigned) j, m->vertices[j],
            vx_color_t{{{ 0.f, 1.f, 0.f }}} });
    }
    VoxelPatch first = scene.apply(recolor);
    TASK_CHECK(first.work.candidateTests == 0);
    TASK_CHECK(first.work.triangleIds.empty());
    TASK_CHECK(first.work.colorTriangleIds.size() == 1);
    TASK_CHECK(first.work.colorTriangleIds[0] == 1);
    TASK_CHECK(first.upsert.empty() && first.remove.empty());
    for (int j = 3; j < 6; ++j) {
        m->colors[j] = vx_color_t{{{ 0.f, 1.f, 0.f }}};
    }
    TASK_CHECK(checkReplay(replay, scene) == 0);
    TASK_CHECK(analyticSingleCellWinner(scene) == 0);

    // Then move the winner away: the surviving cell takes the persisted green
    // color of triangle 1 through its stored interpolation weights.
    std::vector<VertexUpdate> move;
    for (int j = 0; j < 3; ++j) {
        vx_vertex_t p = m->vertices[j];
        p.x += 4.0f;
        move.push_back(VertexUpdate{ (unsigned) j, p, m->colors[j] });
    }
    VoxelPatch second = scene.apply(move);
    for (int j = 0; j < 3; ++j) { m->vertices[j].x += 4.0f; }
    replayPatch(replay, second);
    TASK_CHECK(checkReplay(replay, scene) == 0);

    std::vector<VoxelRecord> snap = scene.snapshot();
    TASK_CHECK(snap.size() == 2);
    VoxelRecord const& origin = snap[0].key == Key{{ 0, 0, 0 }} ? snap[0] : snap[1];
    TASK_CHECK(origin.winnerTriangle == 1);
    TASK_CHECK(origin.contributorCount == 1);
    TASK_CHECK(origin.color.g == 1.0f);
    TASK_CHECK(origin.color.r == 0.0f && origin.color.b == 0.0f);

    TASK_CHECK(checkAgainstReference(scene, m, step[0], step[1], step[2], precision) == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// exports: snapshots, export parity, isolation and signed/anisotropic grids
// ---------------------------------------------------------------------------

static int case_export_parity_and_isolation(void)
{
    float const step[3] = { 0.5f, 0.5f, 0.5f };
    const float precision = 0.02f;
    vx_mesh_t* m = redBlueMesh(true);
    MeshHolder holder(m);
    VoxelScene scene(m, step[0], step[1], step[2], precision);

    std::vector<VoxelRecord> snap = scene.snapshot();
    TASK_CHECK(!snap.empty());
    TASK_CHECK(std::is_sorted(snap.begin(), snap.end(),
        [](VoxelRecord const& a, VoxelRecord const& b) { return a.key < b.key; }));
    TASK_CHECK(checkAgainstReference(scene, m, step[0], step[1], step[2], precision) == 0);

    // Mutating every exported array must not pollute the scene.
    vx_point_cloud_t* pc = scene.pointCloud();
    TASK_CHECK(pc != NULL && pc->nvertices > 0);
    vx_vertex_t savedVertex = pc->vertices[0];
    vx_color_t savedColor = pc->colors[0];
    pc->vertices[0].x += 100.0f;
    pc->colors[0].r += 100.0f;
    vx_point_cloud_free(pc);

    vx_mesh_t* cubes = scene.cubeMesh();
    TASK_CHECK(cubes != NULL && cubes->nvertices > 0);
    cubes->vertices[0].x += 100.0f;
    cubes->indices[0] = 0;
    vx_mesh_free(cubes);

    std::vector<VoxelRecord> again = scene.snapshot();
    TASK_CHECK(again.size() == snap.size());
    TASK_CHECK(again[0].key == snap[0].key);
    TASK_CHECK(colorEquals(again[0].color, snap[0].color));
    vx_point_cloud_t* pc2 = scene.pointCloud();
    TASK_CHECK(pc2->vertices[0].x == savedVertex.x);
    TASK_CHECK(pc2->colors[0].r == savedColor.r);
    vx_point_cloud_free(pc2);

    // The snapshot vector itself is independent storage.
    std::vector<VoxelRecord> copy = scene.snapshot();
    copy[0].winnerTriangle = 9999;
    copy[0].key = Key{{ -99, -99, -99 }};
    std::vector<VoxelRecord> after = scene.snapshot();
    TASK_CHECK(after[0].winnerTriangle == snap[0].winnerTriangle);
    TASK_CHECK(after[0].key == snap[0].key);

    // Uncolored scene keeps the same geometry but exposes no colors.
    vx_mesh_t* plain = redBlueMesh(false);
    MeshHolder plainHolder(plain);
    VoxelScene plainScene(plain, step[0], step[1], step[2], precision);
    TASK_CHECK(!plainScene.colored());
    TASK_CHECK(checkAgainstReference(plainScene, plain, step[0], step[1], step[2], precision) == 0);
    vx_point_cloud_t* plainPc = plainScene.pointCloud();
    TASK_CHECK(plainPc->colors == NULL);
    vx_point_cloud_free(plainPc);
    vx_mesh_t* plainCubes = plainScene.cubeMesh();
    TASK_CHECK(plainCubes->colors == NULL);
    vx_mesh_free(plainCubes);
    return 0;
}

static int case_negative_keys_anisotropic_steps(void)
{
    float const step[3] = { 0.5f, 0.25f, 1.f };
    const float precision = 0.01f; // below minStep / 4 = 0.0625
    vx_mesh_t* m = makeMesh(6, 6, true);
    float const base[3][3] = {
        { -2.30f, -1.70f, 0.40f },
        { -1.80f, -1.70f, 0.40f },
        { -2.30f, -1.20f, 0.40f },
    };
    for (unsigned t = 0; t < 2; ++t) {
        for (int j = 0; j < 3; ++j) {
            unsigned i = 3 * t + j;
            m->indices[i] = i;
            float shift = t * 0.05f;
            setVertex(m, i, base[j][0] + shift, base[j][1] + shift,
                      base[j][2] + shift, t ? 0.f : 1.f, 0.f, t ? 1.f : 0.f);
        }
    }
    MeshHolder holder(m);
    VoxelScene scene(m, step[0], step[1], step[2], precision);

    std::vector<VoxelRecord> snap = scene.snapshot();
    TASK_CHECK(!snap.empty());
    bool hasNegative = false;
    for (VoxelRecord const& r : snap) {
        if (r.key[0] < 0 || r.key[1] < 0 || r.key[2] <= 0) { hasNegative = true; }
        vx_vertex_t center;
        center.x = r.key[0] * step[0];
        center.y = r.key[1] * step[1];
        center.z = r.key[2] * step[2];
        TASK_CHECK(keyOfCenter(center, step) == r.key);
    }
    TASK_CHECK(hasNegative);
    TASK_CHECK(std::is_sorted(snap.begin(), snap.end(),
        [](VoxelRecord const& a, VoxelRecord const& b) { return a.key < b.key; }));
    TASK_CHECK(checkAgainstReference(scene, m, step[0], step[1], step[2], precision) == 0);
    Replay replay;
    for (VoxelRecord const& r : snap) { replay[r.key] = r; }

    // A deformation keeps producing signed keys and stays reference-equivalent.
    std::vector<VertexUpdate> batch;
    batch.push_back(VertexUpdate{ 0,
        vx_vertex_t{{{ -2.05f, -1.45f, 0.90f }}}, m->colors[0] });
    VoxelPatch patch = scene.apply(batch);
    m->vertices[0] = vx_vertex_t{{{ -2.05f, -1.45f, 0.90f }}};
    TASK_CHECK(patch.work.triangleIds.size() == 1);
    TASK_CHECK(patch.work.triangleIds[0] == 0);
    TASK_CHECK(patch.work.candidateTests == directTests(m, 0, step, precision));
    replayPatch(replay, patch);
    TASK_CHECK(checkReplay(replay, scene) == 0);
    TASK_CHECK(checkAgainstReference(scene, m, step[0], step[1], step[2], precision) == 0);
    return 0;
}

const struct task_case task_cases[] = {
    { "dependencies", "shared_vertex",              case_shared_vertex },
    { "dependencies", "duplicate_last_wins_empty",  case_duplicate_last_wins_and_empty },
    { "dependencies", "color_change_zero_tests",    case_color_change_zero_tests },
    { "dependencies", "far_component_locality",     case_far_component_locality },
    { "contributors", "winner_moves_away",          case_winner_moves_away },
    { "contributors", "winner_degenerates",         case_winner_degenerates },
    { "contributors", "winner_restored",            case_winner_restored },
    { "contributors", "nonwinner_recolor_handoff",  case_nonwinner_recolor_then_winner_leaves },
    { "exports",      "export_parity_isolation",    case_export_parity_and_isolation },
    { "exports",      "negative_anisotropic_grid",  case_negative_keys_anisotropic_steps },
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
