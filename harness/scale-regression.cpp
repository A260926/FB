/*
 * scale-regression.cpp
 *
 * Regression tests for the translation/scale robustness of the QuickHull
 * geometric decision chain. The tests are independent of the library internals:
 * every verification quantity (support plane residuals, signed volumes, edge
 * adjacency) is recomputed in long double on coordinates localized by the
 * translation that was applied to the input.
 *
 * The file has its own main() and prints, on success:
 *
 *     PASS similarity cases=N
 *     PASS entrypoints cases=N
 *     PASS reuse-degenerate cases=N
 *     QUICKHULL_SCALE_OK
 *
 * It can be compiled standalone with no arguments:
 *     c++ -std=c++11 -O1 -I. QuickHull.cpp harness/scale-regression.cpp -o out
 */
#include "QuickHull.hpp"
#include <cmath>
#include <cstdio>
#include <vector>
#include <set>
#include <map>
#include <string>
#include <limits>

using namespace quickhull;

typedef long double ld;

static int g_failures = 0;

#define REQUIRE(cond, msg)                                                        \
	do {                                                                          \
		if (!(cond)) {                                                            \
			++g_failures;                                                         \
			std::printf("FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);        \
		}                                                                         \
	} while (0)

namespace {

template<typename T>
struct Xform {
	ld s;
	Vector3<T> t;
	const char* name;
};

// ---- The three known three-dimensional witness shapes (unit/local geometry) -

// The fixed 12 point cloud also used by harness/baseline.cpp.
std::vector<Vector3<double> > makeCloud12() {
	std::vector<Vector3<double> > p;
	p.push_back(Vector3<double>(-.63644445627244906,.68846768444896989,.77772965710215303));
	p.push_back(Vector3<double>(.61364265394764972,-.17955349395657641,-.79401228965430826));
	p.push_back(Vector3<double>(-.56099800177033965,.25951522662099591,.71984816390977391));
	p.push_back(Vector3<double>(-.91745487211373944,.48513244229633506,.14083471850494145));
	p.push_back(Vector3<double>(-.93137361301852306,-.56897355808071304,-.024861964838954753));
	p.push_back(Vector3<double>(-.45458828580918786,-.76187313462060358,-.40073183849569732));
	p.push_back(Vector3<double>(-.24429187295794086,-.99775220161164746,-.044736384275497094));
	p.push_back(Vector3<double>(.049342598096238983,-.90708559127708555,-.34982095854883044));
	p.push_back(Vector3<double>(-.19079979349929632,-.40648696518321337,.33026712813922515));
	p.push_back(Vector3<double>(.0352403759809623896,.51679849006144196,-.64696790683440997));
	p.push_back(Vector3<double>(.024984826892043403,.58737329944566097,.22723194657265736));
	p.push_back(Vector3<double>(-.87322206139455572,-.28755736232502449,.083104115053526564));
	return p;
}

// Unit cube: 8 corners.
std::vector<Vector3<double> > makeCube() {
	std::vector<Vector3<double> > p;
	for (int i = 0; i < 8; i++) {
		p.push_back(Vector3<double>(i & 1, (i >> 1) & 1, (i >> 2) & 1));
	}
	return p;
}

// Asymmetric flat wedge with real thickness: a triangular prism whose three
// axis lengths (2, 1, 0.3) are all different. Volume = (2*1/2)*0.3 = 0.3.
std::vector<Vector3<double> > makeWedge() {
	std::vector<Vector3<double> > p;
	const double h = 0.3;
	p.push_back(Vector3<double>(0,0,0));
	p.push_back(Vector3<double>(2,0,0));
	p.push_back(Vector3<double>(0,1,0));
	p.push_back(Vector3<double>(0,0,h));
	p.push_back(Vector3<double>(2,0,h));
	p.push_back(Vector3<double>(0,1,h));
	return p;
}

template<typename T>
std::vector<Vector3<T> > castCloud(const std::vector<Vector3<double> >& in) {
	std::vector<Vector3<T> > out;
	out.reserve(in.size());
	for (size_t i = 0; i < in.size(); i++) {
		out.push_back(Vector3<T>((T)in[i].x, (T)in[i].y, (T)in[i].z));
	}
	return out;
}

// Apply scale then translation in the working floating point type, so the
// checker works against the actually rounded input, never an idealized one.
template<typename T>
std::vector<Vector3<T> > applyXform(const std::vector<Vector3<T> >& base, const Xform<T>& xf) {
	std::vector<Vector3<T> > out;
	out.reserve(base.size());
	for (size_t i = 0; i < base.size(); i++) {
		out.push_back(Vector3<T>((T)((T)base[i].x * (T)xf.s) + xf.t.x,
		                         (T)((T)base[i].y * (T)xf.s) + xf.t.y,
		                         (T)((T)base[i].z * (T)xf.s) + xf.t.z));
	}
	return out;
}

// Move a world coordinate back to localized long double geometry.
template<typename T>
Vector3<ld> localize(const Vector3<T>& v, const Xform<T>& xf) {
	return Vector3<ld>(((ld)v.x - (ld)xf.t.x) / xf.s,
	                   ((ld)v.y - (ld)xf.t.y) / xf.s,
	                   ((ld)v.z - (ld)xf.t.z) / xf.s);
}

template<typename T>
int findInputIndex(const std::vector<Vector3<T> >& pc, const Vector3<T>& v) {
	for (size_t i = 0; i < pc.size(); i++) {
		if (pc[i] == v) {
			return (int)i;
		}
	}
	return -1;
}

// Signed volume of a triangle mesh given localized vertices (long double).
ld localizedVolume(const std::vector<size_t>& idx,
                   const std::vector<Vector3<ld> >& localVerts) {
	ld vol = 0;
	for (size_t i = 0; i < idx.size(); i += 3) {
		const Vector3<ld>& a = localVerts[idx[i]];
		const Vector3<ld>& b = localVerts[idx[i + 1]];
		const Vector3<ld>& c = localVerts[idx[i + 2]];
		const Vector3<ld> ab = b - a;
		const Vector3<ld> ac = c - a;
		const Vector3<ld> cr = ab.crossProduct(ac);
		vol += a.dotProduct(cr) / 6.0L;
	}
	return vol;
}

// Every undirected edge must occur exactly twice, with the two triangles
// traversing it in opposite directions (closed orientable manifold).
bool manifoldEdges(const std::vector<size_t>& idx) {
	std::map<std::pair<size_t, size_t>, int> undirected;
	std::set<std::pair<size_t, size_t> > directed;
	for (size_t i = 0; i < idx.size(); i += 3) {
		const size_t t[3] = { idx[i], idx[i + 1], idx[i + 2] };
		for (int k = 0; k < 3; k++) {
			size_t a = t[k], b = t[(k + 1) % 3];
			if (directed.count(std::make_pair(a, b))) {
				return false; // duplicated directed edge
			}
			directed.insert(std::make_pair(a, b));
			size_t lo = std::min(a, b), hi = std::max(a, b);
			undirected[std::make_pair(lo, hi)]++;
		}
	}
	for (std::map<std::pair<size_t, size_t>, int>::const_iterator it = undirected.begin();
	     it != undirected.end(); ++it) {
		if (it->second != 2) {
			return false;
		}
	}
	// Opposite direction must exist for every directed edge.
	for (std::set<std::pair<size_t, size_t> >::const_iterator it = directed.begin();
	     it != directed.end(); ++it) {
		if (!directed.count(std::make_pair(it->second, it->first))) {
			return false;
		}
	}
	return undirected.size() * 2 == directed.size();
}

template<typename T>
ld maxWorldMagnitude(const std::vector<Vector3<T> >& pc) {
	ld m = 0;
	for (size_t i = 0; i < pc.size(); i++) {
		ld ax = std::fabs((ld)pc[i].x), ay = std::fabs((ld)pc[i].y), az = std::fabs((ld)pc[i].z);
		m = std::max(m, std::max(ax, std::max(ay, az)));
	}
	return m;
}

// Rounding-derived residual tolerance, expressed in canonical (unit scale)
// length units. worldMax is the largest |world coordinate| involved.
template<typename T>
ld absoluteTolerance(const Xform<T>& xf, ld worldMax) {
	const ld eps = (ld)std::numeric_limits<T>::epsilon();
	const ld tol = 8.0L * eps * (worldMax + 1.0L) / xf.s;
	const ld floorV = (sizeof(T) >= sizeof(double)) ? 1e-12L : 1e-6L;
	return std::max(tol, floorV);
}

// Independent long double support-plane check: every input point lies on or
// behind every hull triangle plane, and the defining vertices sit on it.
template<typename T>
bool supportPlanesHold(const std::vector<size_t>& idx,
                       const std::vector<Vector3<T> >& worldHullVerts,
                       const std::vector<Vector3<T> >& worldInput,
                       const Xform<T>& xf) {
	std::vector<Vector3<ld> > lv, li;
	for (size_t i = 0; i < worldHullVerts.size(); i++) {
		lv.push_back(localize(worldHullVerts[i], xf));
	}
	for (size_t i = 0; i < worldInput.size(); i++) {
		li.push_back(localize(worldInput[i], xf));
	}
	const ld tol = absoluteTolerance(xf, maxWorldMagnitude(worldInput));
	// Defining vertices are reconstructed from the same rounded coordinates.
	const ld defTol = (sizeof(T) >= sizeof(double)) ? 1e-9L : 1e-6L;
	for (size_t f = 0; f < idx.size(); f += 3) {
		const Vector3<ld>& a = lv[idx[f]];
		const Vector3<ld>& b = lv[idx[f + 1]];
		const Vector3<ld>& c = lv[idx[f + 2]];
		Vector3<ld> n = (b - a).crossProduct(c - a);
		ld len = n.getLength();
		if (!(len > 0)) {
			return false; // zero-area triangle: a collapsed shell
		}
		n = n / len;
		// The winding convention of the index buffer is not guaranteed to match our
		// cross product handedness, so orient the normal towards the empty half space:
		// flip it if the point cloud lies predominantly on the positive side.
		ld meanResid = 0;
		for (size_t k = 0; k < li.size(); k++) {
			meanResid += n.dotProduct(li[k] - a);
		}
		if (meanResid > 0) {
			n = -n;
		}
		for (size_t k = 0; k < li.size(); k++) {
			ld d = n.dotProduct(li[k] - a);
			if (d > tol) {
				return false;
			}
		}
		// Defining vertices must be (numerically) exactly on the plane.
		const Vector3<ld> def[3] = { a, b, c };
		for (int k = 0; k < 3; k++) {
			if (std::fabs(n.dotProduct(def[k] - a)) > defTol) {
				return false;
			}
		}
	}
	return true;
}

// Hull vertex set expressed as indices into the original input cloud.
std::set<int> hullInputIndices(const std::vector<size_t>& idx) {
	std::set<int> s;
	for (size_t i = 0; i < idx.size(); i++) {
		s.insert((int)idx[i]);
	}
	return s;
}

template<typename T>
std::vector<Vector3<T> > copyVertices(const VertexDataSource<T>& vb) {
	std::vector<Vector3<T> > w;
	for (size_t i = 0; i < vb.size(); i++) {
		w.push_back(Vector3<T>(vb[i].x, vb[i].y, vb[i].z));
	}
	return w;
}

} // namespace

// ===========================================================================
// Group 1: similarity under scaling/translation (double, then float)
// ===========================================================================
template<typename T>
static int runSimilarityForType() {
	int cases = 0;

	struct ShapeDef { const char* name; std::vector<Vector3<double> > points;
	                  size_t expectFaces; size_t expectHullVerts; };
	std::vector<ShapeDef> shapes;
	ShapeDef s12; s12.name = "cloud12"; s12.points = makeCloud12(); s12.expectFaces = 18; s12.expectHullVerts = 0;
	ShapeDef scu; scu.name = "cube";    scu.points = makeCube();    scu.expectFaces = 12; scu.expectHullVerts = 8;
	ShapeDef sw;  sw.name = "wedge";    sw.points = makeWedge();    sw.expectFaces = 8;  sw.expectHullVerts = 6;
	shapes.push_back(s12); shapes.push_back(scu); shapes.push_back(sw);

	std::vector<Xform<T> > xfs;
	if (sizeof(T) >= sizeof(double)) {
		// Scales 1e-5, 1e-3, 1, 1e3 and translations in several distinct directions.
		xfs.push_back((Xform<T>){ 1e-5L, Vector3<T>((T)1e6, (T)-1e6, (T)0), "s1e-5 t(+,-,0)" });
		xfs.push_back((Xform<T>){ 1e-5L, Vector3<T>(0, 0, 0),                         "s1e-5 t0" });
		xfs.push_back((Xform<T>){ 1e-3L, Vector3<T>((T)-1e6, 0, (T)1e6),             "s1e-3 t(-,0,+)" });
		xfs.push_back((Xform<T>){ 1.0L,  Vector3<T>((T)1e6, (T)1e6, (T)1e6),         "s1 t(+,+,+)" });
		xfs.push_back((Xform<T>){ 1.0L,  Vector3<T>(0, (T)-1e3, (T)1e3),             "s1 t(0,-,+)" });
		xfs.push_back((Xform<T>){ 1e3L,  Vector3<T>((T)1e6, 0, (T)-1e6),             "s1e3 t(+,0,-)" });
		xfs.push_back((Xform<T>){ 1e3L,  Vector3<T>(0, 0, 0),                         "s1e3 t0" });
	} else {
		// Milder float parameters chosen so local detail is still representable.
		xfs.push_back((Xform<T>){ 1e-3L, Vector3<T>((T)10, (T)-10, (T)5),  "s1e-3 t(+,-,+)" });
		xfs.push_back((Xform<T>){ 1.0L,  Vector3<T>((T)1000, 0, (T)-1000), "s1 t(+,0,-)" });
		xfs.push_back((Xform<T>){ 1.0L,  Vector3<T>(0, 0, 0),               "s1 t0" });
		xfs.push_back((Xform<T>){ 1e3L,  Vector3<T>(0, 0, 0),               "s1e3 t0" });
	}

	const T eps = defaultEps<T>();

	for (size_t si = 0; si < shapes.size(); si++) {
		const std::vector<Vector3<T> > base = castCloud<T>(shapes[si].points);

		// Reference hull of the untransformed cloud, original indices.
		QuickHull<T> refQh;
		ConvexHull<T> refHull = refQh.getConvexHull(base, true, true, eps);
		const std::set<int> refSet = hullInputIndices(refHull.getIndexBuffer());
		const size_t refFaces = refHull.getIndexBuffer().size() / 3;
		REQUIRE(refFaces == shapes[si].expectFaces, "reference face count changed");
		if (shapes[si].expectHullVerts) {
			REQUIRE(refSet.size() == shapes[si].expectHullVerts, "reference hull vertex count changed");
		}
		const Xform<T> identity = { 1.0L, Vector3<T>(0, 0, 0), "identity" };
		std::vector<Vector3<ld> > refLocal;
		for (size_t i = 0; i < base.size(); i++) refLocal.push_back(localize(base[i], identity));
		const ld refVol = std::fabs(localizedVolume(refHull.getIndexBuffer(), refLocal));
		REQUIRE(refVol > 0, "reference hull must have real 3D volume");

		for (size_t xi = 0; xi < xfs.size(); xi++) {
			const Xform<T>& xf = xfs[xi];
			const std::vector<Vector3<T> > world = applyXform(base, xf);
			QuickHull<T> qh;

			// Compact vertex output.
			ConvexHull<T> hullC = qh.getConvexHull(world, true, false, eps);
			const std::vector<size_t>& idxC = hullC.getIndexBuffer();
			// Original-index output.
			ConvexHull<T> hullO = qh.getConvexHull(world, true, true, eps);
			const std::vector<size_t>& idxO = hullO.getIndexBuffer();

			REQUIRE(idxC.size() == idxO.size(), "compact/original index counts differ");
			REQUIRE(idxC.size() / 3 == refFaces, "face count changed under similarity transform");
			REQUIRE(manifoldEdges(idxC), "compact hull is not a closed orientable manifold");
			REQUIRE(manifoldEdges(idxO), "original-index hull is not a closed manifold");

			// Original indices must name exactly the same input points as the reference.
			REQUIRE(hullInputIndices(idxO) == refSet, "support vertex set changed under transform");

			// Compact hull vertices are copies of input points: map them back exactly.
			const std::vector<Vector3<T> > compactWorld = copyVertices(hullC.getVertexBuffer());
			std::set<int> compactSet;
			for (size_t i = 0; i < compactWorld.size(); i++) {
				int k = findInputIndex(world, compactWorld[i]);
				REQUIRE(k >= 0, "compact vertex is not an input point");
				compactSet.insert(k);
			}
			REQUIRE(compactSet == refSet, "compact support vertex set changed under transform");

			// Volume in localized long double coordinates.
			std::vector<Vector3<ld> > localCompact;
			for (size_t i = 0; i < compactWorld.size(); i++) {
				localCompact.push_back(localize(compactWorld[i], xf));
			}
			// Localizing (world - t)/s reconstructs the canonical unit frame, so the
			// localized volume is directly comparable to the reference volume.
			const ld gotVol = std::fabs(localizedVolume(idxC, localCompact));
			const ld relTol = std::max((ld)8 * std::numeric_limits<T>::epsilon(),
			                           absoluteTolerance(xf, maxWorldMagnitude(world)) / 2.0L);
			REQUIRE(std::fabs(gotVol - refVol) <= relTol * std::fabs(refVol),
			        "volume changed under similarity transform");

			// Independent support plane residuals for both output modes.
			REQUIRE(supportPlanesHold(idxC, compactWorld, world, xf),
			        "compact hull support plane violated");
			const std::vector<Vector3<T> > origWorld = copyVertices(hullO.getVertexBuffer());
			REQUIRE(supportPlanesHold(idxO, origWorld, world, xf),
			        "original-index hull support plane violated");

			++cases;
		}
	}
	return cases;
}

// ===========================================================================
// Group 2: every public entry point, both windings, both index modes,
//          half-edge mesh reconstruction.
// ===========================================================================
template<typename T>
static int runEntrypointsForType() {
	int cases = 0;
	const T eps = defaultEps<T>();
	const Xform<T> xf = (sizeof(T) >= sizeof(double))
		? (Xform<T>){ 1e-3L, Vector3<T>((T)1e6, (T)-1e6, (T)1e6), "far small cube" }
		: (Xform<T>){ 1e-3L, Vector3<T>((T)10, (T)-10, (T)5),    "mild far small cube" };
	const std::vector<Vector3<T> > base = castCloud<T>(makeCube());
	const std::vector<Vector3<T> > world = applyXform(base, xf);

	const ld expectVol = 1.0L; // unit cube: localized volume must come back as 1
	const ld relTol = std::max((ld)8 * std::numeric_limits<T>::epsilon(),
	                           absoluteTolerance(xf, maxWorldMagnitude(world)) / 2.0L);

	auto volumeOf = [&](const std::vector<size_t>& idx,
	                    const VertexDataSource<T>& vb) -> ld {
		std::vector<Vector3<ld> > lv;
		for (size_t i = 0; i < vb.size(); i++) {
			lv.push_back(localize(Vector3<T>(vb[i].x, vb[i].y, vb[i].z), xf));
		}
		// Localization already reconstructs the canonical frame, so no s^3 factor.
		return std::fabs(localizedVolume(idx, lv));
	};

	// (1) std::vector entry point, CCW, compact
	{
		QuickHull<T> qh;
		ConvexHull<T> h = qh.getConvexHull(world, true, false, eps);
		REQUIRE(h.getIndexBuffer().size() / 3 == 12, "vector/CCW/compact face count");
		REQUIRE(h.getVertexBuffer().size() == 8, "vector/CCW/compact vertex count");
		REQUIRE(manifoldEdges(h.getIndexBuffer()), "vector/CCW/compact manifold");
		REQUIRE(std::fabs(volumeOf(h.getIndexBuffer(), h.getVertexBuffer()) - expectVol) <= relTol,
		        "vector/CCW/compact volume");
		REQUIRE(supportPlanesHold(h.getIndexBuffer(), copyVertices(h.getVertexBuffer()), world, xf),
		        "vector/CCW/compact support planes");
		++cases;
	}

	// (2) std::vector entry point, CW vs CCW: opposite winding, same solid
	{
		QuickHull<T> qh;
		ConvexHull<T> cw = qh.getConvexHull(world, false, false, eps);
		ConvexHull<T> ccw = qh.getConvexHull(world, true, false, eps);
		REQUIRE(cw.getIndexBuffer().size() == ccw.getIndexBuffer().size(), "CW/CCW index size");
		REQUIRE(manifoldEdges(cw.getIndexBuffer()), "CW manifold");
		const auto& ci = cw.getIndexBuffer();
		const auto& ni = ccw.getIndexBuffer();
		const auto& cv = cw.getVertexBuffer();
		const auto& nv = ccw.getVertexBuffer();
		Vector3<ld> ncw = (localize(cv[ci[1]], xf) - localize(cv[ci[0]], xf))
		                  .crossProduct(localize(cv[ci[2]], xf) - localize(cv[ci[0]], xf));
		Vector3<ld> nccw = (localize(nv[ni[1]], xf) - localize(nv[ni[0]], xf))
		                   .crossProduct(localize(nv[ni[2]], xf) - localize(nv[ni[0]], xf));
		const ld dot = ncw.dotProduct(nccw) / (ncw.getLength() * nccw.getLength());
		REQUIRE(dot < -0.999999L, "CW/CCW normals are not opposite");
		++cases;
	}

	// (3) std::vector entry point, CCW, original indices: borrowed buffer keeps
	//     the original address and contents.
	{
		QuickHull<T> qh;
		ConvexHull<T> h = qh.getConvexHull(world, true, true, eps);
		REQUIRE(h.getIndexBuffer().size() / 3 == 12, "vector/original face count");
		REQUIRE(&h.getVertexBuffer()[0] == &world[0], "original indices must borrow the input buffer");
		REQUIRE(h.getVertexBuffer().size() == world.size(), "original buffer length");
		REQUIRE(manifoldEdges(h.getIndexBuffer()), "vector/original manifold");
		++cases;
	}

	// (4) Vector3 pointer entry point.
	{
		QuickHull<T> qh;
		ConvexHull<T> h = qh.getConvexHull(world.data(), world.size(), true, true, eps);
		REQUIRE(h.getIndexBuffer().size() / 3 == 12, "Vector3* face count");
		REQUIRE(&h.getVertexBuffer()[0] == &world[0], "Vector3* borrowed address");
		REQUIRE(manifoldEdges(h.getIndexBuffer()), "Vector3* manifold");
		REQUIRE(std::fabs(volumeOf(h.getIndexBuffer(), h.getVertexBuffer()) - expectVol) <= relTol,
		        "Vector3* volume");
		++cases;
	}

	// (5) Flat xyz pointer entry point.
	{
		QuickHull<T> qh;
		const T* flat = &world[0].x;
		ConvexHull<T> h = qh.getConvexHull(flat, world.size(), true, false, eps);
		REQUIRE(h.getIndexBuffer().size() / 3 == 12, "flat xyz face count");
		REQUIRE(h.getVertexBuffer().size() == 8, "flat xyz vertex count");
		REQUIRE(manifoldEdges(h.getIndexBuffer()), "flat xyz manifold");
		REQUIRE(std::fabs(volumeOf(h.getIndexBuffer(), h.getVertexBuffer()) - expectVol) <= relTol,
		        "flat xyz volume");
		++cases;
	}

	// (6) Half edge mesh, CCW: next/opp/face reconstruct the same convex solid,
	//     every edge having an opposite half edge traversed in reverse.
	{
		QuickHull<T> qh;
		HalfEdgeMesh<T, size_t> m = qh.getConvexHullAsMesh(&world[0].x, world.size(), true, eps);
		REQUIRE(m.m_faces.size() == 12, "halfedge face count");
		REQUIRE(m.m_vertices.size() == 8, "halfedge vertex count");
		REQUIRE(m.m_halfEdges.size() == 36, "halfedge count");
		bool ok = true;
		std::set<std::pair<size_t, size_t> > directed;
		for (size_t i = 0; i < m.m_halfEdges.size(); i++) {
			const typename HalfEdgeMesh<T, size_t>::HalfEdge& he = m.m_halfEdges[i];
			const typename HalfEdgeMesh<T, size_t>::HalfEdge& opp = m.m_halfEdges[he.m_opp];
			// Opposite pairing is symmetric.
			if (opp.m_opp != i) { ok = false; }
			const size_t startI = opp.m_endVertex; // edge i runs startI -> endI
			const size_t endI = he.m_endVertex;
			if (startI == endI) { ok = false; }
			// ... and the opposite edge runs endI -> startI.
			if (opp.m_endVertex != startI ||
			    m.m_halfEdges[opp.m_opp].m_endVertex != endI) {
				ok = false;
			}
			if (!directed.insert(std::make_pair(startI, endI)).second) {
				ok = false; // directed edge duplicated
			}
			// next cycle of three returns to this half edge and stays on the face.
			size_t n = he.m_next;
			n = m.m_halfEdges[n].m_next;
			n = m.m_halfEdges[n].m_next;
			if (n != i) { ok = false; }
			if (m.m_halfEdges[he.m_next].m_face != he.m_face ||
			    m.m_halfEdges[m.m_halfEdges[he.m_next].m_next].m_face != he.m_face) {
				ok = false;
			}
		}
		REQUIRE(ok, "halfedge next/opp/face structure invalid");
		REQUIRE(directed.size() == m.m_halfEdges.size(), "halfedge directed edges not unique");
		// Reconstruct triangles from the half edge structure and check the volume.
		std::vector<size_t> tri;
		for (size_t f = 0; f < m.m_faces.size(); f++) {
			size_t he = m.m_faces[f].m_halfEdgeIndex;
			tri.push_back(m.m_halfEdges[m.m_halfEdges[m.m_halfEdges[he].m_next].m_next].m_endVertex);
			tri.push_back(m.m_halfEdges[m.m_halfEdges[he].m_next].m_endVertex);
			tri.push_back(m.m_halfEdges[he].m_endVertex);
		}
		std::vector<Vector3<ld> > lv;
		for (size_t i = 0; i < m.m_vertices.size(); i++) {
			lv.push_back(localize(m.m_vertices[i], xf));
		}
		const ld v = std::fabs(localizedVolume(tri, lv));
		REQUIRE(std::fabs(v - expectVol) <= relTol, "halfedge reconstructed volume");
		++cases;
	}

	// (7) Half edge mesh, CW: same solid, reversed orientation.
	{
		QuickHull<T> qh;
		HalfEdgeMesh<T, size_t> m = qh.getConvexHullAsMesh(&world[0].x, world.size(), false, eps);
		REQUIRE(m.m_faces.size() == 12 && m.m_halfEdges.size() == 36, "CW halfedge counts");
		bool ok = true;
		for (size_t i = 0; i < m.m_halfEdges.size(); i++) {
			if (m.m_halfEdges[m.m_halfEdges[i].m_opp].m_opp != i) { ok = false; }
		}
		REQUIRE(ok, "CW halfedge opposite pairing");
		++cases;
	}

	return cases;
}

// ===========================================================================
// Group 3: object reuse, state isolation, degenerate dimensions, interior
//          and duplicate points.
// ===========================================================================
template<typename T>
static std::vector<Vector3<T> > snapshot(const std::vector<Vector3<T> >& pc) {
	return pc;
}

template<typename T>
static bool unchanged(const std::vector<Vector3<T> >& a, const std::vector<Vector3<T> >& b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (size_t i = 0; i < a.size(); i++) {
		if (a[i] != b[i]) {
			return false;
		}
	}
	return true;
}

template<typename T>
static int runReuseDegenerateForType() {
	int cases = 0;
	const T eps = defaultEps<T>();
	const bool isDouble = sizeof(T) >= sizeof(double);

	const ld bigS = 1e3L;
	const Vector3<T> bigT = isDouble ? Vector3<T>((T)1e6, 0, (T)-1e6)
	                                 : Vector3<T>((T)1000, 0, (T)-1000);
	const ld smallS = isDouble ? 1e-5L : 1e-3L;
	const Vector3<T> smallT = isDouble ? Vector3<T>((T)1e6, (T)-1e6, 0)
	                                   : Vector3<T>((T)10, (T)-10, (T)5);

	// One reused object, called in the mandated order:
	// large -> small -> coplanar -> normal 3D.
	QuickHull<T> qh;

	struct Step {
		std::vector<Vector3<T> > input;
		size_t minFaces;
		bool expectVolume;
	};
	std::vector<Step> steps;

	{
		Xform<T> xf = { bigS, bigT, "large" };
		Step st;
		st.input = applyXform(castCloud<T>(makeCube()), xf);
		st.minFaces = 12; st.expectVolume = true;
		steps.push_back(st);
	}
	{
		Xform<T> xf = { smallS, smallT, "small" };
		Step st;
		st.input = applyXform(castCloud<T>(makeCloud12()), xf);
		st.minFaces = 18; st.expectVolume = true;
		steps.push_back(st);
	}
	{
		// Genuinely coplanar quad (2D), placed far from the origin.
		Step st;
		const T o = isDouble ? (T)1e6 : (T)1000;
		st.input.push_back(Vector3<T>(o, o, o));
		st.input.push_back(Vector3<T>(o + (T)1, o, o));
		st.input.push_back(Vector3<T>(o + (T)1, o + (T)1, o));
		st.input.push_back(Vector3<T>(o, o + (T)1, o));
		// A 4-point planar quad falls back to a doubled zero-volume shell of
		// four triangles (two per side); this is the existing 2D convention.
		st.minFaces = 4; st.expectVolume = false;
		steps.push_back(st);
	}
	{
		Step st;
		st.input = castCloud<T>(makeWedge());
		st.minFaces = 8; st.expectVolume = true;
		steps.push_back(st);
	}

	for (size_t k = 0; k < steps.size(); k++) {
		const std::vector<Vector3<T> > before = snapshot(steps[k].input);

		// A fresh object gives the reference answer for this exact input.
		QuickHull<T> fresh;
		ConvexHull<T> ref = fresh.getConvexHull(steps[k].input, true, false, eps);

		ConvexHull<T> got = qh.getConvexHull(steps[k].input, true, false, eps);
		REQUIRE(got.getIndexBuffer().size() / 3 >= steps[k].minFaces, "reused object face count regression");
		REQUIRE(got.getIndexBuffer().size() == ref.getIndexBuffer().size(),
		        "reused object result differs from fresh object");
		REQUIRE(got.getVertexBuffer().size() == ref.getVertexBuffer().size(),
		        "reused object vertex count differs (planar temp leak?)");
		REQUIRE(qh.getDiagnostics().m_failedHorizonEdges == 0, "diagnostics leaked between calls");
		REQUIRE(unchanged(steps[k].input, before), "input cloud modified by the library");

		// Compact output vertices must all be positions present in the input.
		bool allFromInput = true;
		for (size_t i = 0; i < got.getVertexBuffer().size(); i++) {
			if (findInputIndex(steps[k].input, got.getVertexBuffer()[i]) < 0) {
				allFromInput = false;
			}
		}
		REQUIRE(allFromInput, "output contains a vertex not present in the input");

		if (steps[k].expectVolume) {
			REQUIRE(manifoldEdges(got.getIndexBuffer()), "reused 3D hull lost its manifold");
		}
		++cases;
	}

	// After the coplanar call the same object must still build a full 3D hull:
	// the temporary planar lift point must not leak into later calls.
	{
		ConvexHull<T> h = qh.getConvexHull(castCloud<T>(makeCube()), true, false, eps);
		REQUIRE(h.getIndexBuffer().size() / 3 == 12, "post-planar 3D hull corrupted");
		REQUIRE(h.getVertexBuffer().size() == 8, "post-planar hull has leaked temp vertices");
		++cases;
	}

	// Low dimensional inputs follow the existing fallback conventions and carry
	// zero signed volume; no output vertex is an invented position.
	const T far = isDouble ? (T)1e6 : (T)1000;
	{
		// 0D: coincident points.
		std::vector<Vector3<T> > pc;
		for (int i = 0; i < 10; i++) pc.push_back(Vector3<T>(far, -far, far));
		ConvexHull<T> h = qh.getConvexHull(pc, true, false, eps);
		REQUIRE(h.getIndexBuffer().size() == 12, "0D fallback triangle count");
		for (size_t i = 0; i < h.getVertexBuffer().size(); i++) {
			REQUIRE(findInputIndex(pc, h.getVertexBuffer()[i]) >= 0, "0D invented vertex");
		}
		++cases;
	}
	{
		// 1D: genuinely collinear points.
		std::vector<Vector3<T> > pc;
		for (int i = 0; i < 8; i++) {
			pc.push_back(Vector3<T>(far + (T)i, -far, far));
		}
		ConvexHull<T> h = qh.getConvexHull(pc, true, false, eps);
		REQUIRE(h.getIndexBuffer().size() == 12, "1D fallback triangle count");
		for (size_t i = 0; i < h.getVertexBuffer().size(); i++) {
			REQUIRE(findInputIndex(pc, h.getVertexBuffer()[i]) >= 0, "1D invented vertex");
		}
		++cases;
	}
	{
		// 2D: coplanar polygon, zero volume.
		std::vector<Vector3<T> > pc;
		pc.push_back(Vector3<T>(far, far, far));
		pc.push_back(Vector3<T>(far + (T)2, far, far));
		pc.push_back(Vector3<T>(far + (T)2, far + (T)1, far));
		pc.push_back(Vector3<T>(far, far + (T)1, far));
		ConvexHull<T> h = qh.getConvexHull(pc, true, false, eps);
		std::vector<Vector3<ld> > lv;
		for (size_t i = 0; i < h.getVertexBuffer().size(); i++) {
			lv.push_back(Vector3<ld>((ld)h.getVertexBuffer()[i].x,
			                         (ld)h.getVertexBuffer()[i].y,
			                         (ld)h.getVertexBuffer()[i].z));
		}
		REQUIRE(std::fabs(localizedVolume(h.getIndexBuffer(), lv)) == 0, "2D hull must have zero volume");
		for (size_t i = 0; i < h.getVertexBuffer().size(); i++) {
			REQUIRE(findInputIndex(pc, h.getVertexBuffer()[i]) >= 0, "2D invented vertex");
		}
		++cases;
	}

	// Interior points and duplicated support points must not change the boundary.
	{
		std::vector<Vector3<T> > pc = castCloud<T>(makeCube());
		QuickHull<T> q1;
		ConvexHull<T> base = q1.getConvexHull(pc, true, true, eps);
		const std::set<int> baseSet = hullInputIndices(base.getIndexBuffer());

		pc.push_back(Vector3<T>((T)0.5, (T)0.5, (T)0.5));   // strict interior
		pc.push_back(Vector3<T>((T)0.25, (T)0.75, (T)0.4)); // strict interior
		const size_t before = pc.size();
		for (size_t i = 0; i < 8; i++) {                    // duplicate every corner
			pc.push_back(pc[i]);
		}
		REQUIRE(pc.size() == before + 8, "test setup duplicate count");
		ConvexHull<T> h = qh.getConvexHull(pc, true, true, eps);
		REQUIRE(h.getIndexBuffer().size() == base.getIndexBuffer().size(),
		        "interior/duplicate points changed the face count");
		const std::set<int> gotSet = hullInputIndices(h.getIndexBuffer());
		REQUIRE(gotSet == baseSet, "interior/duplicate points changed the support boundary");
		++cases;
	}

	return cases;
}

int main() {
	const int simD = runSimilarityForType<double>();
	const int simF = runSimilarityForType<float>();
	const int epD = runEntrypointsForType<double>();
	const int epF = runEntrypointsForType<float>();
	const int rdD = runReuseDegenerateForType<double>();
	const int rdF = runReuseDegenerateForType<float>();

	const int similarityCases = simD + simF;
	const int entrypointCases = epD + epF;
	const int reuseCases = rdD + rdF;

	if (g_failures != 0) {
		std::printf("SCALE REGRESSION FAILURES: %d\n", g_failures);
		return 1;
	}
	if (similarityCases <= 0 || entrypointCases <= 0 || reuseCases <= 0) {
		std::printf("FAIL: no cases executed\n");
		return 1;
	}
	std::printf("PASS similarity cases=%d\n", similarityCases);
	std::printf("PASS entrypoints cases=%d\n", entrypointCases);
	std::printf("PASS reuse-degenerate cases=%d\n", reuseCases);
	std::printf("QUICKHULL_SCALE_OK\n");
	return 0;
}
