#pragma once

#include "maths.h"
#include "mesh.h"
#include "scene.h"

#include <vector>

// Causal (finite light speed) rendering mode.
//
// In this mode a ray segment does not intersect a snapshot of the scene at
// the camera time.  A photon arriving at time t was emitted at an earlier
// time tau = t - s/c, where s is the true geometric distance between the
// real vertices of the segment.  Meshes move with per-vertex linear motion
// between positions at t=0 and endPositions at t=1; instance transforms are
// static and invertible.  Only triangle meshes, two-sided Lambert materials
// (material.color is the albedo) and constant two-sided emissive meshes are
// supported.  Rays whose light path would start before t=0 return black.

struct DVec3
{
	DVec3() : x(0.0), y(0.0), z(0.0) {}
	DVec3(double x, double y, double z) : x(x), y(y), z(z) {}
	explicit DVec3(const Vec3& v) : x(v.x), y(v.y), z(v.z) {}

	double operator[](int i) const { return (&x)[i]; }
	double& operator[](int i) { return (&x)[i]; }

	double x, y, z;
};

inline DVec3 operator-(const DVec3& a) { return DVec3(-a.x, -a.y, -a.z); }
inline DVec3 operator+(const DVec3& a, const DVec3& b) { return DVec3(a.x+b.x, a.y+b.y, a.z+b.z); }
inline DVec3 operator-(const DVec3& a, const DVec3& b) { return DVec3(a.x-b.x, a.y-b.y, a.z-b.z); }
inline DVec3 operator*(const DVec3& a, double s) { return DVec3(a.x*s, a.y*s, a.z*s); }
inline DVec3 operator*(double s, const DVec3& a) { return a*s; }
inline double Dot(const DVec3& a, const DVec3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline DVec3 Cross(const DVec3& a, const DVec3& b)
{
	return DVec3(a.y*b.z - b.y*a.z, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x);
}
inline double LengthSq(const DVec3& a) { return Dot(a, a); }
inline double Length(const DVec3& a) { return sqrt(LengthSq(a)); }
inline Vec3 ToVec3(const DVec3& a) { return Vec3(float(a.x), float(a.y), float(a.z)); }

// One moving triangle, stored in static mesh-local space with endpoint
// vertices so the shared mesh BVH can be reused by every instance.
struct CausalTri
{
	// vertices at t=0 and per-frame vertex velocities (position at t: p0 + dp*t)
	Vec3 a0, b0, c0;
	Vec3 da, db, dc;

	int primitive;   // index into Scene::primitives
	int localFace;   // original face id inside the shared mesh
};

struct CausalMeshEntry
{
	const Mesh* mesh;
	int primitive;
	Transform xform;

	int triStart;
	int triCount;

	// world-space endpoint triangle vertices, one set per face; the static
	// instance transform is baked in.  A0 + dA*t gives the vertex at time t.
	std::vector<Vec3> worldA0, worldB0, worldC0;
	std::vector<Vec3> worldDA, worldDB, worldDC;

	// cumulative time-0 triangle areas, normalized to 1, and face selection
	// probabilities (time-0 area of the face / time-0 area of the light)
	std::vector<float> areaCDF;
	std::vector<float> faceProb;
	float totalArea0;
};

struct CausalScene
{
	CausalScene() : enabled(false), valid(false), lightSpeed(0.0f) {}
	~CausalScene();

	bool enabled;
	bool valid;
	float lightSpeed;

	std::vector<CausalTri> tris;
	std::vector<CausalMeshEntry> entries;

	// sweep BVH, one item per mesh entry, covering the whole [0,1] domain
	BVH bvh;

	void Clear();
};

// Result of a production spacetime query.
struct CausalHit
{
	CausalHit() : hit(false), distance(0.0f), time(0.0f), u(0.0f), v(0.0f), w(0.0f),
	              face(-1), primitive(-1) {}

	bool hit;

	float distance;   // true geometric distance between the real vertices
	float time;       // emission time tau of the photon at the hit point
	// barycentric weights at tau, in (vertex B, vertex C, vertex A) order so
	// point = w*A + u*B + v*C; the same triple feeds CausalLightPdf directly
	float u, v, w;
	int face;         // original face id inside the mesh instance
	int primitive;    // scene primitive that owns the face

	Vec3 point;       // hit point at time tau
	Vec3 normal;      // geometric normal at time tau, oriented against the ray
};

// Production ray query: find the nearest valid retarded intersection.
// rayEpsilon offsets the ray origin to avoid self-intersection but is never
// counted as propagation length: distance/time always refer to the real
// vertices.  Returns false (and a black result) when every candidate would
// have to be emitted before t=0.
bool CausalTrace(const CausalScene& scene, const Vec3& origin, const Vec3& dir,
                 float observationTime, float rayEpsilon, CausalHit& outHit);

// Same production query but visiting every face directly, bypassing the BVH.
// Regression checks use it to prove the BVH path never skips a candidate.
bool CausalTraceBruteForce(const CausalScene& scene, const Vec3& origin, const Vec3& dir,
                           float observationTime, float rayEpsilon, CausalHit& outHit);

// A direct-light sample drawn from one emissive primitive.
struct CausalLightSample
{
	CausalLightSample() : valid(false), te(0.0f), r(0.0f), pdf(0.0f),
	                      bu(0.0f), bv(0.0f), bw(0.0f), face(-1), primitive(-1) {}

	bool valid;

	Vec3 point;      // emission point y(te)
	Vec3 normal;     // geometric normal of the face at te
	Vec3 u;          // unit vector from receiver to the emission point
	Vec3 velocity;   // barycentric point velocity v
	float te;        // retarded emission time
	float r;         // receiver -> emission point distance
	float pdf;       // solid angle density, including the face probability

	float bu, bv, bw;
	int face;
	int primitive;
};

// Draw one sample from an emissive primitive, using time-0 face areas for the
// face probability, uniform barycentric coordinates inside the face, and
// solving the retarded emission time.  Out-of-domain or singular connections
// come back valid=false and are not re-sampled.
CausalLightSample CausalSampleLight(const CausalScene& scene, int primitive,
                                    const Vec3& receiver, float observationTime,
                                    Random& rand);

// Reverse solid angle density for a connection whose emission side is a known
// face/barycentric hit.  The previous real vertex and its time are required;
// the static area formula must not be used.  Returns 0 for out-of-domain or
// singular connections.
float CausalLightPdf(const CausalScene& scene, int primitive,
                     const Vec3& receiver, float observationTime,
                     int face, float bu, float bv, float bw);

// Full causal path tracer, driven by the CPU renderer.  Balance MIS combines
// light sampling and Lambert continuation, including emission seen directly by
// the camera.
Vec3 CausalPathTrace(const Scene& scene, const Vec3& startOrigin, const Vec3& startDir,
                     float observationTime, int maxDepth, Random& rand);

// Build/rebuild the causal data attached to a scene.  Assumes the scene has
// been validated by Scene::EnableCausal.
void BuildCausalScene(Scene& scene);
