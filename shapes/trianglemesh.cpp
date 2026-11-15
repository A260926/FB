
/*
    pbrt source code Copyright(c) 1998-2010 Matt Pharr and Greg Humphreys.

    This file is part of pbrt.

    pbrt is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.  Note that the text contents of
    the book "Physically Based Rendering" are *not* licensed under
    the GNU GPL.

    pbrt is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

 */

// trianglemesh.cpp*
#include "shape.h"
#include "paramset.h"
#include <cmath>
// Deforming triangle mesh helpers
static bool FinitePoint(const Point &p) {
	return std::isfinite(p.x) && std::isfinite(p.y) &&
	       std::isfinite(p.z);
}
// TriangleMesh Declarations
class TriangleMesh : public Shape {
public:
	// TriangleMesh Public Methods
	TriangleMesh(const Transform &o2w, bool ro,
	             int ntris, int nverts, const int *vptr,
				 const Point *P, const Point *P1,
				 float motionStart, float motionEnd,
				 const float *uv);
	~TriangleMesh();
	BBox ObjectBound() const;
	BBox WorldBound() const;
	bool CanIntersect() const { return false; }
	void Refine(vector<Reference<Shape> > &refined) const;
	// Interpolate a single world-space vertex at shutter _time_.
	Point WorldVertex(float time, int index) const {
		if (!p1) return p[index];
		float alpha = Clamp((time - motionStart) /
		                    (motionEnd - motionStart), 0.f, 1.f);
		return (1.f - alpha) * p[index] + alpha * p1[index];
	}
	friend class Triangle;
	template <class T> friend class VertexTexture;
protected:
	// TriangleMesh Data
	int ntris, nverts;
	int *vertexIndex;
	Point *p;
	Point *p1;
	float motionStart, motionEnd;
	float *uvs;
};
class Triangle : public Shape {
public:
	// Triangle Public Methods
	Triangle(const Transform &o2w, bool ro,
	         TriangleMesh *m, int n)
			: Shape(o2w, ro) {
		mesh = m;
		v = &mesh->vertexIndex[3*n];
		// Update created triangles stats
		static StatsCounter trisMade("Geometry",
		                             "Triangles created");
		++trisMade;
	}
	BBox ObjectBound() const;
	BBox WorldBound() const;
	bool Intersect(const Ray &ray, float *tHit,
	               DifferentialGeometry *dg) const;
	bool IntersectP(const Ray &ray) const;
	void GetUVs(float uv[3][2]) const;
	float Area() const;
	float Area(float time) const;
	Point Sample(float u1, float u2, Normal *Ns) const;
	Point Sample(float u1, float u2, Normal *Ns,
		float time) const;
	// Evaluate the three world-space vertices at _time_
	void GetTimeVertices(float time, Point p1[3]) const {
		p1[0] = mesh->WorldVertex(time, v[0]);
		p1[1] = mesh->WorldVertex(time, v[1]);
		p1[2] = mesh->WorldVertex(time, v[2]);
	}
	virtual void GetShadingGeometry(const Transform &obj2world,
			const DifferentialGeometry &dg,
			DifferentialGeometry *dgShading) const {
		// Deforming meshes never carry _N_/_S_ arrays, so the
		// geometric differential geometry is used unchanged.
		*dgShading = dg;
	}
private:
	// Triangle Data
	Reference<TriangleMesh> mesh;
	int *v;
};
// TriangleMesh Method Definitions
TriangleMesh::TriangleMesh(const Transform &o2w, bool ro,
		int nt, int nv, const int *vi, const Point *P,
		const Point *P1, float mstart, float mend,
		const float *uv)
	: Shape(o2w, ro) {
	ntris = nt;
	nverts = nv;
	motionStart = mstart;
	motionEnd = mend;
	vertexIndex = new int[3 * ntris];
	memcpy(vertexIndex, vi, 3 * ntris * sizeof(int));
	// Copy _uv_ data, if present
	if (uv) {
		uvs = new float[2*nverts];
		memcpy(uvs, uv, 2*nverts*sizeof(float));
	}
	else uvs = NULL;
	p = new Point[nverts];
	if (P1) {
		p1 = new Point[nverts];
	}
	else
		p1 = NULL;
	// Transform mesh vertices to world space
	for (int i  = 0; i < nverts; ++i) {
		p[i] = ObjectToWorld(P[i]);
		if (p1)
			p1[i] = ObjectToWorld(P1[i]);
	}
}
TriangleMesh::~TriangleMesh() {
	delete[] vertexIndex;
	delete[] p;
	delete[] p1;
	delete[] uvs;
}
BBox TriangleMesh::ObjectBound() const {
	// The bound covers both motion endpoints, so the static grid and
	// kd-tree accelerators need no per-ray rebuilding.
	BBox bobj;
	for (int i = 0; i < nverts; i++) {
		bobj = Union(bobj, WorldToObject(p[i]));
		if (p1) bobj = Union(bobj, WorldToObject(p1[i]));
	}
	return bobj;
}
BBox TriangleMesh::WorldBound() const {
	BBox worldBounds;
	for (int i = 0; i < nverts; i++) {
		worldBounds = Union(worldBounds, p[i]);
		if (p1) worldBounds = Union(worldBounds, p1[i]);
	}
	return worldBounds;
}
void
TriangleMesh::Refine(vector<Reference<Shape> > &refined)
const {
	for (int i = 0; i < ntris; ++i)
		refined.push_back(new Triangle(ObjectToWorld,
		                               reverseOrientation,
	                                       (TriangleMesh *)this,
										   i));
}
BBox Triangle::ObjectBound() const {
	// Cover both motion endpoints so refined triangles share the
	// full-motion bound of their parent mesh.
	BBox bobj;
	bobj = Union(bobj, WorldToObject(mesh->p[v[0]]));
	bobj = Union(bobj, WorldToObject(mesh->p[v[1]]));
	bobj = Union(bobj, WorldToObject(mesh->p[v[2]]));
	if (mesh->p1) {
		bobj = Union(bobj, WorldToObject(mesh->p1[v[0]]));
		bobj = Union(bobj, WorldToObject(mesh->p1[v[1]]));
		bobj = Union(bobj, WorldToObject(mesh->p1[v[2]]));
	}
	return bobj;
}
BBox Triangle::WorldBound() const {
	BBox wb;
	wb = Union(wb, mesh->p[v[0]]);
	wb = Union(wb, mesh->p[v[1]]);
	wb = Union(wb, mesh->p[v[2]]);
	if (mesh->p1) {
		wb = Union(wb, mesh->p1[v[0]]);
		wb = Union(wb, mesh->p1[v[1]]);
		wb = Union(wb, mesh->p1[v[2]]);
	}
	return wb;
}
bool Triangle::Intersect(const Ray &ray, float *tHit,
		DifferentialGeometry *dg) const {
	// Initialize triangle intersection statistics
	static
	StatsPercentage triangleHits("Geometry",
	                             "Triangle Ray Intersections");
	// Update triangle tests count
	triangleHits.Add(0, 1);
	// Get the triangle vertices at the ray's shutter time
	Point pv[3];
	GetTimeVertices(ray.time, pv);
	const Point &p1 = pv[0];
	const Point &p2 = pv[1];
	const Point &p3 = pv[2];
	Vector e1 = p2 - p1;
	Vector e2 = p3 - p1;
	Vector s1 = Cross(ray.d, e2);
	float divisor = Dot(s1, e1);
	if (divisor == 0.)
		return false;
	float invDivisor = 1.f / divisor;
	// Compute first barycentric coordinate
	Vector d = ray.o - p1;
	float b1 = Dot(d, s1) * invDivisor;
	if (b1 < 0. || b1 > 1.)
		return false;
	// Compute second barycentric coordinate
	Vector s2 = Cross(d, e1);
	float b2 = Dot(ray.d, s2) * invDivisor;
	if (b2 < 0. || b1 + b2 > 1.)
		return false;
	// Compute _t_ to intersection point
	float t = Dot(e2, s2) * invDivisor;
	if (t < ray.mint || t > ray.maxt)
		return false;
	triangleHits.Add(1, 0); //NOBOOK
	// Fill in _DifferentialGeometry_ from triangle hit
	// Compute triangle partial derivatives
	Vector dpdu, dpdv;
	float uvs[3][2];
	GetUVs(uvs);
	// Compute deltas for triangle partial derivatives
	float du1 = uvs[0][0] - uvs[2][0];
	float du2 = uvs[1][0] - uvs[2][0];
	float dv1 = uvs[0][1] - uvs[2][1];
	float dv2 = uvs[1][1] - uvs[2][1];
	Vector dp1 = p1 - p3, dp2 = p2 - p3;
	float determinant = du1 * dv2 - dv1 * du2;
	if (determinant == 0.f) {
		// Handle zero determinant for triangle partial derivative matrix
		CoordinateSystem(Normalize(Cross(e2, e1)), &dpdu, &dpdv);
	}
	else {
		float invdet = 1.f / determinant;
		dpdu = ( dv2 * dp1 - dv1 * dp2) * invdet;
		dpdv = (-du2 * dp1 + du1 * dp2) * invdet;
	}
	// Interpolate $(u,v)$ triangle parametric coordinates
	float b0 = 1 - b1 - b2;
	float tu = b0*uvs[0][0] + b1*uvs[1][0] + b2*uvs[2][0];
	float tv = b0*uvs[0][1] + b1*uvs[1][1] + b2*uvs[2][1];
	*dg = DifferentialGeometry(ray(t), dpdu, dpdv,
	                           Vector(0,0,0), Vector(0,0,0),
							   tu, tv, this);
	*tHit = t;
	return true;
}
bool Triangle::IntersectP(const Ray &ray) const {
	// Initialize triangle intersection statistics
	static
	StatsPercentage triangleHits("Geometry",
	                             "Triangle Ray Intersections");
	// Update triangle tests count
	triangleHits.Add(0, 1);
	// Get the triangle vertices at the ray's shutter time
	Point pv[3];
	GetTimeVertices(ray.time, pv);
	const Point &p1 = pv[0];
	const Point &p2 = pv[1];
	const Point &p3 = pv[2];
	Vector e1 = p2 - p1;
	Vector e2 = p3 - p1;
	Vector s1 = Cross(ray.d, e2);
	float divisor = Dot(s1, e1);
	if (divisor == 0.)
		return false;
	float invDivisor = 1.f / divisor;
	// Compute first barycentric coordinate
	Vector d = ray.o - p1;
	float b1 = Dot(d, s1) * invDivisor;
	if (b1 < 0. || b1 > 1.)
		return false;
	// Compute second barycentric coordinate
	Vector s2 = Cross(d, e1);
	float b2 = Dot(ray.d, s2) * invDivisor;
	if (b2 < 0. || b1 + b2 > 1.)
		return false;
	// Compute _t_ to intersection point
	float t = Dot(e2, s2) * invDivisor;
	if (t < ray.mint || t > ray.maxt)
		return false;
	triangleHits.Add(1, 0); //NOBOOK
	return true;
}
void Triangle::GetUVs(float uv[3][2]) const {
	if (mesh->uvs) {
		uv[0][0] = mesh->uvs[2*v[0]];
		uv[0][1] = mesh->uvs[2*v[0]+1];
		uv[1][0] = mesh->uvs[2*v[1]];
		uv[1][1] = mesh->uvs[2*v[1]+1];
		uv[2][0] = mesh->uvs[2*v[2]];
		uv[2][1] = mesh->uvs[2*v[2]+1];
	} else {
		uv[0][0] = 0.; uv[0][1] = 0.;
		uv[1][0] = 1.; uv[1][1] = 0.;
		uv[2][0] = 1.; uv[2][1] = 1.;
	}
}
float Triangle::Area() const {
	// Get triangle vertices in _p1_, _p2_, and _p3_
	const Point &p1 = mesh->p[v[0]];
	const Point &p2 = mesh->p[v[1]];
	const Point &p3 = mesh->p[v[2]];
	return 0.5f * Cross(p2-p1, p3-p1).Length();
}
float Triangle::Area(float time) const {
	Point pv[3];
	GetTimeVertices(time, pv);
	return 0.5f * Cross(pv[1]-pv[0], pv[2]-pv[0]).Length();
}
Point Triangle::Sample(float u1, float u2,
		Normal *Ns) const {
	return Sample(u1, u2, Ns, 0.f);
}
Point Triangle::Sample(float u1, float u2, Normal *Ns,
		float time) const {
	float b1, b2;
	UniformSampleTriangle(u1, u2, &b1, &b2);
	Point pv[3];
	GetTimeVertices(time, pv);
	Point p = b1 * pv[0] + b2 * pv[1] +
	          (1.f - b1 - b2) * pv[2];
	Normal n = Normal(Cross(pv[1]-pv[0], pv[2]-pv[0]));
	if (n.LengthSquared() == 0.f)
		*Ns = Normal(0.f, 0.f, 0.f);
	else {
		*Ns = Normalize(n);
		if (reverseOrientation) *Ns *= -1.f;
	}
	return p;
}
extern "C" DLLEXPORT Shape *CreateShape(const Transform &o2w,
		bool reverseOrientation, const ParamSet &params) {
	int nvi, npi, nuvi, nsi, nni;
	const int *vi = params.FindInt("indices", &nvi);
	const Point *P = params.FindPoint("P", &npi);
	int np1i;
	const Point *P1 = params.FindPoint("P1", &np1i);
	const float *uvs = params.FindFloat("uv", &nuvi);
	if (!uvs) uvs = params.FindFloat("st", &nuvi);
	// XXX should complain if uvs aren't an array of 2...
	if (uvs) {
		if (nuvi < 2 * npi) {
			Error("Not enough of \"uv\"s for triangle mesh.  Expencted %d, "
			      "found %d.  Discarding.\n", 2*npi, nuvi);
			uvs = NULL;
		}
		else if (nuvi > 2 * npi)
			Warning("More \"uv\"s provided than will be used for triangle "
			        "mesh.  (%d expcted, %d found)\n", npi, nuvi);
	}
	if (!vi || !P) return NULL;
	const Vector *S = params.FindVector("S", &nsi);
	const Normal *N = params.FindNormal("N", &nni);
	// Deforming meshes cannot carry shading normals or tangents:
	// their values are only defined at a single instant.
	if (P1 && (N || S)) {
		Error("Deforming triangle mesh with \"P1\" cannot use "
		      "\"N\" or \"S\" vertex arrays");
		return NULL;
	}
	if (S && nsi != npi) {
		Error("Number of \"S\"s for triangle mesh must match \"P\"s");
		S = NULL;
	}
	if (N && nni != npi) {
		Error("Number of \"N\"s for triangle mesh must match \"P\"s");
		N = NULL;
	}
	if (uvs && N) {
		// if there are normals, check for bad uv's that
		// give degenerate mappings; discard them if so
		const int *vp = vi;
		for (int i = 0; i < nvi; i += 3, vp += 3) {
			float area = .5f * Cross(P[vp[0]]-P[vp[1]], P[vp[2]]-P[vp[1]]).Length();
			if (area < 1e-7) continue; // ignore degenerate tris.
			if ((uvs[2*vp[0]] == uvs[2*vp[1]] &&
				uvs[2*vp[0]+1] == uvs[2*vp[1]+1]) ||
				(uvs[2*vp[1]] == uvs[2*vp[2]] &&
				uvs[2*vp[1]+1] == uvs[2*vp[2]+1]) ||
				(uvs[2*vp[2]] == uvs[2*vp[0]] &&
				uvs[2*vp[2]+1] == uvs[2*vp[0]+1])) {
				Warning("Degenerate uv coordinates in triangle mesh.  Discarding all uvs.");
				uvs = NULL;
				break;
			}
		}
	}
	for (int i = 0; i < nvi; ++i)
		if (vi[i] >= npi) {
			Error("trianglemesh has out of-bounds vertex index %d (%d \"P\" values were given",
				vi[i], npi);
			return NULL;
		}
	float motionStart = params.FindOneFloat("motionstart", 0.f);
	float motionEnd = params.FindOneFloat("motionend", 1.f);
	if (P1) {
		// Validate the full motion specification before building
		if (np1i != npi) {
			Error("Number of \"P1\"s for triangle mesh must match \"P\"s");
			return NULL;
		}
		if (!std::isfinite(motionStart) || !std::isfinite(motionEnd) ||
		    !(motionEnd > motionStart)) {
			Error("Deforming triangle mesh requires finite, strictly "
			      "increasing motion times (motionstart=%g, motionend=%g)",
			      motionStart, motionEnd);
			return NULL;
		}
		for (int i = 0; i < nvi; ++i)
			if (vi[i] < 0) {
				Error("trianglemesh has negative vertex index %d", vi[i]);
				return NULL;
			}
		for (int i = 0; i < npi; ++i)
			if (!FinitePoint(P[i]) || !FinitePoint(P1[i])) {
				Error("Deforming triangle mesh vertex %d is not finite", i);
				return NULL;
			}
	}
	return new TriangleMesh(o2w, reverseOrientation, nvi/3, npi, vi, P,
		P1, motionStart, motionEnd, uvs);
}
