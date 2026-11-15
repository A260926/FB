//-----------------------------------------------------------------------------
// Headless regression for the tessellation result chain: shared SCurve trim
// validation, uv polygon and surface grid triangulation, SShell aggregation,
// Group display caches and ExportMeshTo.
//
// Every check goes through the production entries in src/srf, groupmesh.cpp
// and export.cpp; this file only constructs fixtures and inspects results.
//-----------------------------------------------------------------------------
#include "solvespace.h"
#include "check.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace SolveSpace;

namespace {

//-----------------------------------------------------------------------------
// Fixtures
//-----------------------------------------------------------------------------

// A unit square plane at z = id, bounded by four exact line curves, exactly
// like the production extrude end caps. With broken == true one side is
// omitted, so the trim edges cannot assemble into a closed loop.
void AddPlaneSurface(SShell &sh, uint32_t id, bool broken = false) {
    SSurface s = SSurface::FromPlane({0, 0, double(id)},
                                     {1, 0, 0}, {0, 1, 0});
    s.h = {id};
    s.face = id;
    s.color = RGBi(70, 80, 90);

    Vector ps[4] = {
        {0, 0, double(id)}, {1, 0, double(id)},
        {1, 1, double(id)}, {0, 1, double(id)}
    };
    int edges = broken ? 3 : 4;
    for(int i = 0; i < edges; ++i) {
        SCurve c = {};
        c.h = {id * 10 + unsigned(i) + 1};
        c.isExact = true;
        c.exact = SBezier::From(ps[i], ps[(i + 1) % 4]);
        SCurvePt a = {0, ps[i], true};
        SCurvePt b = {0, ps[(i + 1) % 4], true};
        c.pts.Add(&a);
        c.pts.Add(&b);
        c.surfA = s.h;
        sh.curve.Add(&c);

        STrimBy t = STrimBy::EntireCurve(&sh, c.h, false);
        s.trim.Add(&t);
    }
    sh.surface.Add(&s);
}

// Unit square plane with a square hole, built from shared curves: every
// boundary side and every hole side is its own SCurve referenced by trims.
void AddPlaneWithHole(SShell &sh, uint32_t id) {
    SSurface s = SSurface::FromPlane({0, 0, double(id)},
                                     {1, 0, 0}, {0, 1, 0});
    s.h = {id};
    s.face = id;
    s.color = RGBi(11, 22, 33);

    Vector outer[4] = {{0, 0, double(id)}, {1, 0, double(id)},
                       {1, 1, double(id)}, {0, 1, double(id)}};
    Vector hole[4] = {{0.25, 0.25, double(id)}, {0.75, 0.25, double(id)},
                      {0.75, 0.75, double(id)}, {0.25, 0.75, double(id)}};

    auto addLoop = [&](Vector pts[4], uint32_t base, bool holeWinding) {
        for(int i = 0; i < 4; ++i) {
            Vector p0 = pts[i];
            Vector p1 = pts[(i + 1) % 4];
            SCurve c = {};
            c.h = {base + unsigned(i) + 1};
            c.isExact = true;
            // The hole loop is stored clockwise so the polygon assembler
            // sees a proper inner contour; the trim records that direction.
            if(holeWinding) {
                c.exact = SBezier::From(p1, p0);
                SCurvePt a = {0, p1, true};
                SCurvePt b = {0, p0, true};
                c.pts.Add(&a);
                c.pts.Add(&b);
            } else {
                c.exact = SBezier::From(p0, p1);
                SCurvePt a = {0, p0, true};
                SCurvePt b = {0, p1, true};
                c.pts.Add(&a);
                c.pts.Add(&b);
            }
            c.surfA = s.h;
            sh.curve.Add(&c);
            STrimBy t = STrimBy::EntireCurve(&sh, c.h, holeWinding);
            s.trim.Add(&t);
        }
    };
    addLoop(outer, id * 100, false);
    addLoop(hole, id * 100 + 50, true);

    sh.surface.Add(&s);
}

// Two unit square planes joined by a shared, reversed trim edge. The shared
// SCurve is referenced once forwards and once backwards, and the backwards
// trim endpoints must be found in the reversed traversal.
void AddTwoPlanesSharingEdge(SShell &sh) {
    AddPlaneSurface(sh, 1);

    SSurface s = SSurface::FromPlane({1, 0, 0}, {1, 0, 0}, {0, 1, 0});
    s.h = {2};
    s.face = 2;
    s.color = RGBi(1, 2, 3);

    Vector ps[4] = {{1, 0, 0}, {2, 0, 0}, {2, 1, 0}, {1, 1, 0}};
    // Only the three non-shared sides get their own curves; the fourth side
    // is the reversed shared curve of surface 1.
    for(int i = 0; i < 3; ++i) {
        SCurve c = {};
        c.h = {200 + unsigned(i) + 1};
        c.isExact = true;
        c.exact = SBezier::From(ps[i], ps[(i + 1) % 4]);
        SCurvePt a = {0, ps[i], true};
        SCurvePt b = {0, ps[(i + 1) % 4], true};
        c.pts.Add(&a);
        c.pts.Add(&b);
        c.surfA = s.h;
        sh.curve.Add(&c);
        STrimBy t = STrimBy::EntireCurve(&sh, c.h, false);
        s.trim.Add(&t);
    }

    // Reuse the existing shared curve (handle 12): it runs from (1,0,0) to
    // (1,1,0), while this surface traverses it backwards.
    STrimBy shared = STrimBy::EntireCurve(&sh, {12}, true);
    s.trim.Add(&shared);
    sh.surface.Add(&s);
}

// A degree (2,2) Bezier patch like the baseline curvature fixture, but with
// every control point and weight finite and positive.
SSurface CurvedPatch() {
    SSurface s = {};
    s.degm = 2;
    s.degn = 2;
    for(int i = 0; i < 3; ++i) {
        for(int j = 0; j < 3; ++j) {
            s.ctrl[i][j] = {i / 2.0, j / 2.0,
                            (i == 2 && j == 2) ? 4.0 : 0.0};
            s.weight[i][j] = 1.0;
        }
    }
    return s;
}

// The same patch trimmed by its four parameter-space boundary lines.
SShell CurvedShell(uint32_t id) {
    SShell sh = {};
    SSurface s = CurvedPatch();
    s.h = {id};
    s.face = id;
    s.color = RGBi(9, 8, 7);

    Vector corners[4];
    for(int k = 0; k < 4; ++k) {
        double u = (k == 1 || k == 2) ? 1.0 : 0.0;
        double v = (k >= 2) ? 1.0 : 0.0;
        corners[k] = s.PointAt(u, v);
    }
    for(int i = 0; i < 4; ++i) {
        SCurve c = {};
        c.h = {id * 10 + unsigned(i) + 1};
        c.isExact = true;
        c.exact = SBezier::From(corners[i], corners[(i + 1) % 4]);
        SCurvePt a = {0, corners[i], true};
        SCurvePt b = {0, corners[(i + 1) % 4], true};
        c.pts.Add(&a);
        c.pts.Add(&b);
        c.surfA = s.h;
        sh.curve.Add(&c);
        STrimBy t = STrimBy::EntireCurve(&sh, c.h, false);
        s.trim.Add(&t);
    }
    sh.surface.Add(&s);
    return sh;
}

// Same curvature, but the trim curves reference handles that are absent
// from the shell's curve list.
SShell CurvedShellWithMissingCurve(uint32_t id) {
    SShell sh = CurvedShell(id);
    sh.curve.Clear();
    return sh;
}

// Trim whose endpoint is not a point of the referenced curve.
SShell BadEndpointShell(uint32_t id) {
    SShell sh = {};
    SSurface s = SSurface::FromPlane({0, 0, 0}, {1, 0, 0}, {0, 1, 0});
    s.h = {id};
    s.face = id;

    Vector ps[4] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    for(int i = 0; i < 4; ++i) {
        SCurve c = {};
        c.h = {id * 10 + unsigned(i) + 1};
        c.isExact = true;
        c.exact = SBezier::From(ps[i], ps[(i + 1) % 4]);
        SCurvePt a = {0, ps[i], true};
        SCurvePt b = {0, ps[(i + 1) % 4], true};
        c.pts.Add(&a);
        c.pts.Add(&b);
        sh.curve.Add(&c);
        STrimBy t = STrimBy::EntireCurve(&sh, c.h, false);
        s.trim.Add(&t);
    }
    s.trim[0].start = {5.0, 5.0, 0.0};
    sh.surface.Add(&s);
    return sh;
}

// A curve sample containing a non-finite point.
SShell NonFiniteCurveShell(uint32_t id) {
    SShell sh = {};
    SSurface s = SSurface::FromPlane({0, 0, 0}, {1, 0, 0}, {0, 1, 0});
    s.h = {id};
    s.face = id;

    SCurve c = {};
    c.h = {id * 10 + 1};
    c.isExact = true;
    c.exact = SBezier::From(Vector{0, 0, 0}, Vector{1, 0, 0});
    SCurvePt a = {0, {0, 0, 0}, true};
    SCurvePt b = {0, {std::nan(""), 0, 0}, true};
    c.pts.Add(&a);
    c.pts.Add(&b);
    sh.curve.Add(&c);
    STrimBy t = STrimBy::EntireCurve(&sh, c.h, false);
    s.trim.Add(&t);
    sh.surface.Add(&s);
    return sh;
}

double MeshArea(const SMesh &m) {
    double area = 0.0;
    for(const STriangle &t : m.l) area += t.Area();
    return area;
}

void ConfigureUi() {
    SS.chordTolCalculated = .001;
    SS.maxSegments = 128;
    SS.exportScale = 1;
    SS.exportChordTol = .001;
    SS.exportMaxSegments = 128;
    SS.exportMode = false;
}

//-----------------------------------------------------------------------------
// geometry group
//-----------------------------------------------------------------------------

int PlaneHoleCoverage() {
    ConfigureUi();
    SShell sh = {};
    AddPlaneWithHole(sh, 7);
    SMesh m = {};
    TessellationResult r = sh.TriangulateInto(&m);

    TASK_CHECK(r.Ok());
    TASK_CHECK(r.surface.v == 0 && r.curve.v == 0);
    TASK_CHECK(m.l.n >= 4);
    // Outer area 1 minus the 0.5-wide square hole (area 0.25) = 0.75.
    TASK_CHECK(std::fabs(MeshArea(m) - 0.75) < 1e-9);
    // The face/color metadata propagates to every triangle.
    for(const STriangle &t : m.l) {
        TASK_CHECK(t.meta.face == 7);
    }

    sh.Clear();
    m.Clear();
    return 0;
}

int CurvedAndSharedEdge() {
    ConfigureUi();
    SShell curved = CurvedShell(3);
    SMesh cm = {};
    TessellationResult r = curved.TriangulateInto(&cm);
    TASK_CHECK(r.Ok());
    TASK_CHECK(cm.l.n > 2);
    // A properly budgeted curved patch must refine well past the coarse
    // two-triangle grid.
    TASK_CHECK(cm.l.n >= 8);

    SShell joined = {};
    AddTwoPlanesSharingEdge(joined);
    SMesh jm = {};
    r = joined.TriangulateInto(&jm);
    TASK_CHECK(r.Ok());
    double area = MeshArea(jm);
    TASK_CHECK(std::fabs(area - 2.0) < 1e-9);

    // The reversed shared edge keeps the same stations and the same
    // geometric split: collect the distinct x coordinates along x=1.
    int stations = 0;
    for(const STriangle &t : jm.l) {
        for(const Vector &p : {t.a, t.b, t.c}) {
            if(std::fabs(p.x - 1.0) < 1e-9 &&
               std::fabs(p.y) < 1e-9) { stations |= 1; }
            if(std::fabs(p.x - 1.0) < 1e-9 &&
               std::fabs(p.y - 1.0) < 1e-9) { stations |= 2; }
        }
    }
    TASK_CHECK(stations == 3);

    curved.Clear();
    cm.Clear();
    joined.Clear();
    jm.Clear();
    return 0;
}

int GridLimitAndSuccess() {
    ConfigureUi();
    SSurface patch = CurvedPatch();
    patch.h = {42};

    // With a single segment the interval can never be refined to the chord
    // tolerance: LimitExceeded, and no silent acceptance of a coarse grid.
    SS.maxSegments = 1;
    SS.chordTolCalculated = .001;
    List<double> grid = {};
    double zero = 0.0;
    grid.Add(&zero);
    TessellationResult r =
        patch.MakeTriangulationGridInto(&grid, 0, 1, false, 0);
    TASK_CHECK(r.code == TessellationResult::Code::LimitExceeded);
    TASK_CHECK(r.surface.v == 42);
    TASK_CHECK(r.curve.v == 0);

    // The failure must not be hidden by depth or interval heuristics: the
    // terminal interval here is the full interval, still geometrically bad.
    TASK_CHECK(grid.n == 1);

    // A real budget reaches the geometric criterion on the same surface.
    SS.maxSegments = 128;
    List<double> fine = {};
    fine.Add(&zero);
    r = patch.MakeTriangulationGridInto(&fine, 0, 1, false, 0);
    TASK_CHECK(r.Ok());
    TASK_CHECK(fine.n >= 5);

    double worst = 0.0;
    for(int k = 1; k < fine.n; ++k) {
        double a = fine[k - 1], b = fine[k];
        for(int i = 0; i < 4; ++i) {
            double u = i / 3.0;
            Vector p = patch.PointAt(u, a);
            Vector q = patch.PointAt(u, b);
            for(int j = 1; j < 3; ++j) {
                Vector x = patch.PointAt(u, a + (b - a) * j / 3.0);
                worst = std::max(worst, x.DistanceToLine(p, q.Minus(p)));
            }
        }
    }
    TASK_CHECK(worst <= SS.chordTolCalculated);

    grid.Clear();
    fine.Clear();
    ConfigureUi();
    return 0;
}

int InvalidToleranceAndGeometry() {
    ConfigureUi();
    SSurface patch = CurvedPatch();
    patch.h = {43};
    double zero = 0.0;

    SS.chordTolCalculated = 0.0;
    List<double> grid = {};
    grid.Add(&zero);
    TessellationResult r =
        patch.MakeTriangulationGridInto(&grid, 0, 1, false, 0);
    TASK_CHECK(r.code == TessellationResult::Code::InvalidGeometry);

    SS.chordTolCalculated = .001;
    SS.maxSegments = 0;
    List<double> grid2 = {};
    grid2.Add(&zero);
    r = patch.MakeTriangulationGridInto(&grid2, 0, 1, false, 0);
    TASK_CHECK(r.code == TessellationResult::Code::InvalidGeometry);

    // A surface with an unsupported degree and a non-positive weight is
    // rejected before any evaluation.
    SSurface bad = CurvedPatch();
    bad.h = {44};
    bad.degm = 4;
    SMesh m = {};
    SShell holder = {};
    bad.weight[0][0] = -1.0;
    holder.surface.Add(&bad);
    r = holder.TriangulateInto(&m);
    TASK_CHECK(r.code == TessellationResult::Code::InvalidGeometry);
    TASK_CHECK(r.surface.v == 44);
    TASK_CHECK(m.l.n == 0);

    grid.Clear();
    grid2.Clear();
    holder.Clear();
    m.Clear();
    ConfigureUi();
    return 0;
}

int TrimFailureClassification() {
    ConfigureUi();
    SShell missing = CurvedShellWithMissingCurve(5);
    SMesh mm = {};
    TessellationResult r = missing.TriangulateInto(&mm);
    TASK_CHECK(r.code == TessellationResult::Code::InvalidTrim);
    TASK_CHECK(r.surface.v == 5);
    TASK_CHECK(r.curve.v != 0);
    TASK_CHECK(mm.l.n == 0);
    missing.Clear();
    mm.Clear();

    SShell endpoint = BadEndpointShell(6);
    r = endpoint.TriangulateInto(&mm);
    TASK_CHECK(r.code == TessellationResult::Code::InvalidTrim);
    TASK_CHECK(r.surface.v == 6);
    TASK_CHECK(r.curve.v != 0);
    TASK_CHECK(mm.l.n == 0);
    endpoint.Clear();
    mm.Clear();

    SShell nonfinite = NonFiniteCurveShell(8);
    r = nonfinite.TriangulateInto(&mm);
    TASK_CHECK(r.code == TessellationResult::Code::InvalidTrim);
    TASK_CHECK(r.surface.v == 8);
    TASK_CHECK(r.curve.v != 0);
    TASK_CHECK(mm.l.n == 0);
    nonfinite.Clear();
    mm.Clear();

    // An open trim loop is a polygon assembly failure, not a trim failure.
    SShell open = {};
    AddPlaneSurface(open, 9, true);
    r = open.TriangulateInto(&mm);
    TASK_CHECK(r.code == TessellationResult::Code::PolygonFailure);
    TASK_CHECK(r.surface.v == 9);
    TASK_CHECK(mm.l.n == 0);
    open.Clear();
    mm.Clear();
    return 0;
}

//-----------------------------------------------------------------------------
// propagation group
//-----------------------------------------------------------------------------

int ShellNoPartialAppend() {
    ConfigureUi();
    // A good surface first, then a broken one: nothing from the good surface
    // may survive in the target mesh.
    SShell sh = {};
    AddPlaneSurface(sh, 1);
    AddPlaneSurface(sh, 2, true);

    SMesh pre = {};
    STriangle sentinel = {};
    sentinel.a = {10, 10, 10};
    sentinel.b = {11, 10, 10};
    sentinel.c = {10, 11, 10};
    sentinel.meta = {999, RGBi(1, 2, 3)};
    pre.AddTriangle(&sentinel);

    TessellationResult r = sh.TriangulateInto(&pre);
    TASK_CHECK(!r.Ok());
    TASK_CHECK(r.code == TessellationResult::Code::PolygonFailure);
    // The first failure is the lower-handled broken surface (handle 2).
    TASK_CHECK(r.surface.v == 2);

    // Exact pre-call state: one triangle, same order and metadata.
    TASK_CHECK(pre.l.n == 1);
    TASK_CHECK(pre.l[0].meta.face == 999);
    TASK_CHECK(pre.l[0].a.x == 10 && pre.l[0].b.x == 11);
    TASK_CHECK(pre.l[0].c.y == 11);

    // The source shell itself was not rewritten: trim/curve data intact.
    TASK_CHECK(sh.surface.n == 2);
    TASK_CHECK(sh.curve.n == 7);
    TASK_CHECK(sh.surface[1].trim.n == 3);

    sh.Clear();
    pre.Clear();
    return 0;
}

int ShellDeterministicFirstFailure() {
    ConfigureUi();
    // Two broken surfaces: the reported one must always be the lowest
    // handle, independent of scheduling.
    SShell sh = {};
    AddPlaneSurface(sh, 4, true);
    AddPlaneSurface(sh, 5, true);
    SMesh m = {};
    for(int attempt = 0; attempt < 5; ++attempt) {
        TessellationResult r = sh.TriangulateInto(&m);
        TASK_CHECK(r.code == TessellationResult::Code::PolygonFailure);
        TASK_CHECK(r.surface.v == 4);
        TASK_CHECK(m.l.n == 0);
    }
    sh.Clear();
    m.Clear();
    return 0;
}

int SharedCurveNotMutated() {
    ConfigureUi();
    SShell sh = {};
    AddTwoPlanesSharingEdge(sh);

    // Snapshot the shared curve (handle 12) and a control point of the
    // surfaces before the run.
    SCurve before = *sh.curve.FindById({12});
    Vector ctrlBefore[2][2];
    for(int i = 0; i < 2; ++i)
        for(int j = 0; j < 2; ++j)
            ctrlBefore[i][j] = sh.surface[0].ctrl[i][j];

    SMesh m = {};
    TessellationResult r = sh.TriangulateInto(&m);
    TASK_CHECK(r.Ok());

    const SCurve &after = *sh.curve.FindById({12});
    TASK_CHECK(after.pts.n == before.pts.n);
    TASK_CHECK(after.pts[0].p.Equals(before.pts[0].p));
    TASK_CHECK(after.pts[1].p.Equals(before.pts[1].p));
    TASK_CHECK(after.exact.ctrl[0].Equals(before.exact.ctrl[0]));
    for(int i = 0; i < 2; ++i)
        for(int j = 0; j < 2; ++j)
            TASK_CHECK(sh.surface[0].ctrl[i][j].Equals(ctrlBefore[i][j]));

    sh.Clear();
    m.Clear();
    return 0;
}

int AppendSemanticsOnSuccess() {
    ConfigureUi();
    SShell sh = {};
    AddPlaneSurface(sh, 1);

    SMesh m = {};
    STriangle kept = {};
    kept.a = {20, 20, 20};
    kept.b = {21, 20, 20};
    kept.c = {20, 21, 20};
    m.AddTriangle(&kept);

    TessellationResult r = sh.TriangulateInto(&m);
    TASK_CHECK(r.Ok());
    // Existing triangles stay first, new ones are appended.
    TASK_CHECK(m.l.n == 3);
    TASK_CHECK(m.l[0].a.x == 20);
    TASK_CHECK(std::fabs(MeshArea(m) - 1.0 - kept.Area()) < 1e-9);

    sh.Clear();
    m.Clear();
    return 0;
}

//-----------------------------------------------------------------------------
// cache_export group
//-----------------------------------------------------------------------------

// Builds the group used by the cache/export tests and registers it in SK.
// Returns the handle; pointers into SK.group invalidate when later groups
// are added, so tests always re-fetch the group by handle.
hGroup RegisterGroup(uint32_t id) {
    Group g = {};
    g.h = {id};
    g.type = Group::Type::EXTRUDE;
    g.displayDirty = true;
    SK.group.Add(&g);
    hGroup gh = {id};
    SK.groupOrder.Add(&gh);
    return gh;
}

int GroupCacheSuccessFailureRepair() {
    ConfigureUi();
    hGroup gh = RegisterGroup(100);

    // Seed a previous complete display cache.
    SShell goodShell = {};
    AddPlaneSurface(goodShell, 1);
    SMesh oldMesh = {};
    TASK_CHECK(goodShell.TriangulateInto(&oldMesh).Ok());
    Group *g = SK.GetGroup(gh);
    STriangle sentinel = oldMesh.l[0];
    sentinel.a.x += 10;
    sentinel.b.x += 10;
    sentinel.c.x += 10;
    sentinel.meta.face = 999;
    g->displayMesh.MakeFromCopyOf(&oldMesh);
    g->displayMesh.AddTriangle(&sentinel);
    int oldCount = g->displayMesh.l.n;
    TASK_CHECK(oldCount == 3);

    // Stage 1: a good running shell regenerates successfully and clears
    // the dirty flag.
    g->runningShell.MakeFromCopyOf(&goodShell);
    // A real mesh group carries its own shell, which selects the branch
    // that triangulates instead of reusing the running mesh group.
    g->thisShell.MakeFromCopyOf(&goodShell);
    g->displayDirty = true;
    TessellationResult r = g->GenerateDisplayItems();
    TASK_CHECK(r.Ok());
    TASK_CHECK(!g->displayDirty);
    TASK_CHECK(g->DisplayResultAvailable());
    TASK_CHECK(g->GetLastDisplayResult().Ok());
    TASK_CHECK(g->displayMesh.l.n == 2);
    int goodCount = g->displayMesh.l.n;

    // Stage 2: break the running shell. The old good cache survives, the
    // group stays dirty and carries the source diagnosis.
    SShell badShell = {};
    AddPlaneSurface(badShell, 1);
    AddPlaneSurface(badShell, 2, true);
    g->runningShell.Clear();
    g->runningShell.MakeFromCopyOf(&badShell);
    g->thisShell.Clear();
    g->thisShell.MakeFromCopyOf(&badShell);
    g->displayDirty = true;
    r = g->GenerateDisplayItems();
    TASK_CHECK(!r.Ok());
    TASK_CHECK(r.code == TessellationResult::Code::PolygonFailure);
    TASK_CHECK(r.surface.v == 2);
    TASK_CHECK(g->displayDirty);
    TASK_CHECK(g->displayMesh.l.n == goodCount);
    TASK_CHECK(!g->GetLastDisplayResult().Ok());
    TASK_CHECK(!g->GetLastDisplayResult().detail.empty());

    // Stage 3: repair the trim; the retry succeeds and replaces the cache.
    g->runningShell.Clear();
    g->runningShell.MakeFromCopyOf(&goodShell);
    g->thisShell.Clear();
    g->thisShell.MakeFromCopyOf(&goodShell);
    g->displayDirty = true;
    r = g->GenerateDisplayItems();
    TASK_CHECK(r.Ok());
    TASK_CHECK(!g->displayDirty);
    TASK_CHECK(g->displayMesh.l.n == 2);
    TASK_CHECK(g->GetLastDisplayResult().Ok());

    goodShell.Clear();
    oldMesh.Clear();
    badShell.Clear();
    return 0;
}

int InheritedGroupFailure() {
    ConfigureUi();
    hGroup ph = RegisterGroup(200);
    hGroup ch = RegisterGroup(201);

    SShell badShell = {};
    AddPlaneSurface(badShell, 1);
    AddPlaneSurface(badShell, 2, true);
    Group *parent = SK.GetGroup(ph);
    parent->runningShell.MakeFromCopyOf(&badShell);
    parent->thisShell.MakeFromCopyOf(&badShell);
    parent->displayDirty = true;

    // The child contributes no geometry of its own, so it displays the
    // running mesh group's items and must inherit the failure.
    Group *child = SK.GetGroup(ch);
    child->displayDirty = true;
    TessellationResult r = child->GenerateDisplayItems();
    TASK_CHECK(!r.Ok());
    TASK_CHECK(r.code == TessellationResult::Code::PolygonFailure);
    TASK_CHECK(child->displayDirty);
    TASK_CHECK(child->displayMesh.l.n == 0);
    TASK_CHECK(!child->GetLastDisplayResult().Ok());

    // Repairing the parent lets the inherited regeneration succeed.
    SShell goodShell = {};
    AddPlaneSurface(goodShell, 1);
    parent = SK.GetGroup(ph);
    parent->runningShell.Clear();
    parent->runningShell.MakeFromCopyOf(&goodShell);
    parent->thisShell.Clear();
    parent->thisShell.MakeFromCopyOf(&goodShell);
    parent->displayDirty = true;
    child = SK.GetGroup(ch);
    child->displayDirty = true;
    r = child->GenerateDisplayItems();
    TASK_CHECK(r.Ok());
    TASK_CHECK(!child->displayDirty);
    TASK_CHECK(child->displayMesh.l.n == 2);

    badShell.Clear();
    goodShell.Clear();
    return 0;
}

int RunningMeshMixIsAtomic() {
    ConfigureUi();
    hGroup gh = RegisterGroup(300);

    // A broken shell mixed with a non-empty running mesh: neither the
    // shell triangles nor the running-mesh triangles may be published.
    SShell badShell = {};
    AddPlaneSurface(badShell, 1);
    AddPlaneSurface(badShell, 2, true);
    Group *g = SK.GetGroup(gh);
    g->runningShell.MakeFromCopyOf(&badShell);
    g->thisShell.MakeFromCopyOf(&badShell);

    STriangle meshTri = {};
    meshTri.a = {0, 0, 0};
    meshTri.b = {1, 0, 0};
    meshTri.c = {0, 1, 0};
    g->runningMesh.AddTriangle(&meshTri);
    g->displayDirty = true;

    // Seed an old complete cache that must survive the failed attempt.
    STriangle oldTri = meshTri;
    oldTri.a = {30, 30, 30};
    oldTri.b = {31, 30, 30};
    oldTri.c = {30, 31, 30};
    g->displayMesh.AddTriangle(&oldTri);

    TessellationResult r = g->GenerateDisplayItems();
    TASK_CHECK(!r.Ok());
    TASK_CHECK(g->displayDirty);
    TASK_CHECK(g->displayMesh.l.n == 1);
    TASK_CHECK(g->displayMesh.l[0].a.x == 30);

    badShell.Clear();
    return 0;
}

int ExportFailureKeepsFileAndMode() {
    ConfigureUi();
    hGroup gh = RegisterGroup(400);
    SS.GW.activeGroup = gh;

    SShell badShell = {};
    AddPlaneSurface(badShell, 1);
    AddPlaneSurface(badShell, 2, true);
    Group *g = SK.GetGroup(gh);
    g->runningShell.MakeFromCopyOf(&badShell);
    g->thisShell.MakeFromCopyOf(&badShell);
    g->displayDirty = true;

    const char *tmpdir = std::getenv("TMPDIR");
    std::string dir = tmpdir ? tmpdir : "/tmp";
    if(dir.empty() || dir.back() != '/') dir += '/';
    std::string path = dir + "solvespace_rejected.stl";
    FILE *old = std::fopen(path.c_str(), "wb");
    TASK_CHECK(old != nullptr);
    std::fputs("KEEP_ME", old);
    std::fclose(old);

    SS.exportMode = false;
    SS.ExportMeshTo(Platform::Path::From(path));

    FILE *check = std::fopen(path.c_str(), "rb");
    TASK_CHECK(check != nullptr);
    std::fseek(check, 0, SEEK_END);
    long bytes = std::ftell(check);
    std::fclose(check);
    TASK_CHECK(bytes == 7);
    TASK_CHECK(SS.exportMode == false);
    TASK_CHECK(g->displayDirty);

    // A repaired shell exports a real, non-empty STL and restores mode too.
    SShell goodShell = {};
    AddPlaneSurface(goodShell, 1);
    g->runningShell.Clear();
    g->runningShell.MakeFromCopyOf(&goodShell);
    g->thisShell.Clear();
    g->thisShell.MakeFromCopyOf(&goodShell);
    g->displayDirty = true;

    std::string outPath = dir + "solvespace_production.stl";
    std::remove(outPath.c_str());
    SS.exportMode = false;
    SS.ExportMeshTo(Platform::Path::From(outPath));

    FILE *stl = std::fopen(outPath.c_str(), "rb");
    TASK_CHECK(stl != nullptr);
    std::fseek(stl, 0, SEEK_END);
    long stlBytes = std::ftell(stl);
    std::fclose(stl);
    // 80-byte header plus the 4-byte triangle count, at minimum.
    TASK_CHECK(stlBytes > 84);
    TASK_CHECK(SS.exportMode == false);
    TASK_CHECK(!g->displayDirty);

    badShell.Clear();
    goodShell.Clear();
    return 0;
}

} // namespace

extern const struct task_case task_cases[];
const struct task_case task_cases[] = {
    {"geometry", "plane_with_hole_coverage",      PlaneHoleCoverage},
    {"geometry", "curved_patch_and_shared_edge",  CurvedAndSharedEdge},
    {"geometry", "grid_limit_exceeded_and_fine",  GridLimitAndSuccess},
    {"geometry", "invalid_tolerance_and_surface", InvalidToleranceAndGeometry},
    {"geometry", "trim_failure_classification",   TrimFailureClassification},

    {"propagation", "shell_no_partial_append",       ShellNoPartialAppend},
    {"propagation", "shell_deterministic_failure",   ShellDeterministicFirstFailure},
    {"propagation", "shared_curve_not_mutated",      SharedCurveNotMutated},
    {"propagation", "append_semantics_on_success",   AppendSemanticsOnSuccess},

    {"cache_export", "group_success_failure_repair", GroupCacheSuccessFailureRepair},
    {"cache_export", "inherited_group_failure",      InheritedGroupFailure},
    {"cache_export", "running_mesh_mix_atomic",      RunningMeshMixIsAtomic},
    {"cache_export", "export_file_and_mode",         ExportFailureKeepsFileAndMode},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
