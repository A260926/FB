/****************************************************************************
* VCGLib                                                            o o     *
* Visual and Computer Graphics Library                            o     o   *
*                                                                _   O  _   *
* Copyright(C) 2004-2016                                           \/)\/    *
* Visual Computing Lab                                            /\/|      *
* ISTI - Italian National Research Council                           |      *
*                                                                    \      *
* All rights reserved.                                                      *
*                                                                           *
* This program is free software; you can redistribute it and/or modify      *
* it under the terms of the GNU General Public License as published by      *
* the Free Software Foundation; either version 2 of the License, or         *
* (at your option) any later version.                                       *
*                                                                           *
* This program is distributed in the hope that it will be useful,           *
* but WITHOUT ANY WARRANTY; without even the implied warranty of            *
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the             *
* GNU General Public License (http://www.gnu.org/licenses/gpl.txt)          *
* for more details.                                                         *
*                                                                           *
****************************************************************************/

#ifndef __VCG_VOLUME_PRESERVATION__
#define __VCG_VOLUME_PRESERVATION__

#include <vcg/math/quadric.h>
#include <vcg/complex/algorithms/update/topology.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <Eigen/SVD>
#include <Eigen/LU>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace vcg {
namespace tri {

/**
  @brief Helper implementing the exact volume-preserving placement used by
  TriEdgeCollapseQuadric when TriEdgeCollapseQuadricParameter::PreserveVolume
  is true.

  All the machinery in this file is local to a single edge collapse candidate.
  Nothing here caches state between candidates or collapses: every ring face,
  coordinate and quadric coefficient is re-read from the live mesh every time,
  so successive collapses always see the current surviving geometry.

  Numerical note. Signed volumes are sums of scalar triple products; evaluating
  them in world coordinates makes the guarantee vanish for models placed far
  from the world origin (large-number cancellation). Every computation below
  therefore works in a local frame centered on the midpoint of the collapsing
  edge, where the coordinates of the one-ring have the size of the ring itself.
*/
template <class TriMeshType>
class VolumePreservationHelper
{
public:
  typedef typename TriMeshType::VertexType  VertexType;
  typedef typename TriMeshType::FaceType    FaceType;
  typedef typename TriMeshType::CoordType   CoordType;
  typedef typename TriMeshType::ScalarType  ScalarType;
  typedef vcg::face::VFIterator<FaceType>   VFIterator;

  // Relative threshold separating zero from non-zero singular values of the
  // quadric / constraint matrices of a candidate.
  static constexpr double kRankRelEps = 1e-12;
  // Relative tolerance on residual orthogonality when checking that a linear
  // system actually has a finite solution.
  static constexpr double kConsistencyRelEps = 1e-10;

  /**
    @brief Validate the whole mesh for a volume preserving simplification.

    Throws std::invalid_argument before any candidate, vertex mark or quadric
    is touched. Required input: finite coordinates, strictly positive face
    area, closed, coherently oriented, 2-manifold triangle mesh. Several
    mutually disconnected shells are allowed, and a shell may be globally
    inverted. Self-intersecting geometry is legal and is not tested here.
  */
  static void ValidateMesh(TriMeshType &m)
  {
    vcg::tri::UpdateTopology<TriMeshType>::VertexFace(m);
    vcg::tri::UpdateBounding<TriMeshType>::Box(m);
    const double areaFloor = 1e-13 * m.bbox.Diag() * m.bbox.Diag();

    for(auto vi=m.vert.begin(); vi!=m.vert.end(); ++vi)
      if(!(*vi).IsD())
      {
        const CoordType &p = (*vi).cP();
        if(!(std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2])))
          throw std::invalid_argument("PreserveVolume: non-finite vertex coordinate");
        if(vcg::face::VFIterator<FaceType>(&*vi).End())
          throw std::invalid_argument("PreserveVolume: isolated vertex");
      }

    for(auto fi=m.face.begin(); fi!=m.face.end(); ++fi)
      if(!(*fi).IsD() && vcg::DoubleArea(*fi) <= ScalarType(areaFloor))
        throw std::invalid_argument("PreserveVolume: degenerate/zero-area face");

    // For every undirected edge collect the faces incident on it, together
    // with the net direction in which those faces traverse the edge.
    struct EdgeInfo { std::vector<FaceType*> faces; int oriented = 0; };
    std::map<std::pair<VertexType*,VertexType*>, EdgeInfo> edgeMap;
    for(auto fi=m.face.begin(); fi!=m.face.end(); ++fi)
    {
      FaceType &f = *fi;
      if(f.IsD()) continue;
      for(int j=0;j<3;++j)
      {
        VertexType *a = f.V(j);
        VertexType *b = f.V1(j);
        auto key = a<b ? std::make_pair(a,b) : std::make_pair(b,a);
        EdgeInfo &ei = edgeMap[key];
        ei.faces.push_back(&f);
        ei.oriented += (a<b) ? 1 : -1; // +1/-1: direction of this face on the edge
      }
    }

    if(edgeMap.empty())
      throw std::invalid_argument("PreserveVolume: empty mesh");

    for(auto &kv : edgeMap)
    {
      const EdgeInfo &ei = kv.second;
      if(ei.faces.size() == 1)
        throw std::invalid_argument("PreserveVolume: open boundary edge");
      if(ei.faces.size() != 2)
        throw std::invalid_argument("PreserveVolume: non manifold edge");
      // The two faces must traverse the shared edge in opposite directions,
      // otherwise the shell is not coherently oriented (or non orientable).
      if(ei.oriented != 0)
        throw std::invalid_argument("PreserveVolume: inconsistently oriented faces across edge");
    }

    // Manifold vertices: the faces incident on a vertex must form a single
    // fan cycle (rejects two surface sheets pinched at one vertex).
    for(auto vi=m.vert.begin(); vi!=m.vert.end(); ++vi)
    {
      if((*vi).IsD()) continue;
      std::vector<FaceType*> fan;
      for(VFIterator x(&*vi); !x.End(); ++x) fan.push_back(x.F());
      if(fan.size() < 3)
        throw std::invalid_argument("PreserveVolume: non manifold vertex fan");

      // Each face has exactly two edges outgoing from this vertex; link it to
      // the other face sharing each of those edges.
      std::map<FaceType*, std::vector<FaceType*> > links;
      for(FaceType *f : fan)
      {
        int k = 0;
        while(f->V(k) != &*vi) ++k;
        VertexType *w1 = f->V1(k);
        VertexType *w2 = f->V2(k);
        for(VertexType *w : {w1, w2})
        {
          auto key = (&*vi)<w ? std::make_pair(&*vi,w) : std::make_pair(w,&*vi);
          const std::vector<FaceType*> &ff = edgeMap.find(key)->second.faces;
          FaceType *other = (ff[0]==f) ? ff[1] : ff[0];
          links[f].push_back(other);
        }
        if(links[f].size() != 2 || links[f][0] == links[f][1])
          throw std::invalid_argument("PreserveVolume: non manifold vertex fan");
      }

      std::set<FaceType*> seen;
      std::vector<FaceType*> stack(1, fan.front());
      while(!stack.empty())
      {
        FaceType *cur = stack.back(); stack.pop_back();
        if(!seen.insert(cur).second) continue;
        for(FaceType *nb : links[cur]) if(!seen.count(nb)) stack.push_back(nb);
      }
      if(seen.size() != fan.size())
        throw std::invalid_argument("PreserveVolume: disconnected vertex fan (surface pinched at a vertex)");
    }
  }

  /**
    @brief Exact volume preserving constrained QEM placement.

    Given the collapse v0 -> v1 to a final position x, the one-ring directed
    volume before and after the collapse must be equal:

      V_old = 1/6 sum over the distinct ring faces of a.(b x c)
      V_new(x) = same sum on the surviving faces, with the collapse endpoint
                 replaced by x (the two shared faces disappear and are counted
                 only in V_old, every other ring face exactly once).

    A surviving face (x,p,q) in stored cyclic order contributes x.(p x q)/6,
    so V_new is affine in x. Multiplying the equality by 6 gives
      n . u = d
    with u expressed in the edge-midpoint local frame (u = x - o),
      n = sum_surviving (p x q),  d = sum_ring a.(b x c)  (local coords).

    On this plane we minimize Qd(v0)+Qd(v1). Rank deficient quadrics and
    rank deficient constrained problems keep a well defined constrained
    minimum; among several minimizers the one closest to the edge midpoint is
    returned. Returns false (no throw, no mesh mutation) when no finite point
    satisfying the equality exists.
  */
  static bool ConstrainedPlacement(VertexType *v0, VertexType *v1,
                                   const math::Quadric<double> &q0,
                                   const math::Quadric<double> &q1,
                                   CoordType &result)
  {
    const CoordType o = (v0->cP() + v1->cP()) * ScalarType(0.5);

    // ---- collect the one ring from the live mesh --------------------------
    std::vector<FaceType*> sharedFaces;   // incident on both endpoints: deleted
    std::vector<FaceType*> v0OnlyFaces;   // v0 endpoint will be rewritten to x
    std::vector<FaceType*> v1OnlyFaces;   // already contain v1, moved to x

    for(VFIterator x(v0); !x.End(); ++x)
    {
      bool hasV1 = false;
      for(int j=0;j<3;++j) if(x.F()->V(j) == v1) { hasV1 = true; break; }
      if(hasV1) sharedFaces.push_back(x.F());
      else       v0OnlyFaces.push_back(x.F());
    }
    for(VFIterator x(v1); !x.End(); ++x)
    {
      bool hasV0 = false;
      for(int j=0;j<3;++j) if(x.F()->V(j) == v0) { hasV0 = true; break; }
      if(!hasV0) v1OnlyFaces.push_back(x.F());
    }

    // ---- local frame quantities (everything relative to o) ----------------
    auto local = [&](const CoordType &p){ return p - o; };

    // d = 6 * V_old of the ring, evaluated in local coordinates.
    double d = 0.0;
    auto addOldFace = [&](const FaceType &f)
    {
      CoordType a = local(f.cP(0)), b = local(f.cP(1)), c = local(f.cP(2));
      d += a.dot(b ^ c);
    };
    for(FaceType *f : sharedFaces) addOldFace(*f);
    for(FaceType *f : v0OnlyFaces) addOldFace(*f);
    for(FaceType *f : v1OnlyFaces) addOldFace(*f);

    // n = sum over surviving faces of (p x q), so that 6 * V_new(u) = n.u.
    Eigen::Vector3d n = Eigen::Vector3d::Zero();
    auto addSurvivingFace = [&](const FaceType &f, VertexType *endpoint)
    {
      int k = 0;
      while(f.V(k) != endpoint) ++k;
      CoordType p = local(f.cP((k+1)%3));
      CoordType q = local(f.cP((k+2)%3));
      n += Eigen::Vector3d(p[0],p[1],p[2]).cross(Eigen::Vector3d(q[0],q[1],q[2]));
    };
    for(FaceType *f : v0OnlyFaces) addSurvivingFace(*f, v0);
    for(FaceType *f : v1OnlyFaces) addSurvivingFace(*f, v1);

    // ---- summed QEM quadric translated into the local frame ---------------
    // Q(o+u) = u^T A u + b.u + const, with b = 2 A o + b_world.
    Eigen::Matrix3d A;
    A << q0.a[0]+q1.a[0], q0.a[1]+q1.a[1], q0.a[2]+q1.a[2],
         q0.a[1]+q1.a[1], q0.a[3]+q1.a[3], q0.a[4]+q1.a[4],
         q0.a[2]+q1.a[2], q0.a[4]+q1.a[4], q0.a[5]+q1.a[5];
    Eigen::Vector3d bw(q0.b[0]+q1.b[0], q0.b[1]+q1.b[1], q0.b[2]+q1.b[2]);
    Eigen::Vector3d ox(o[0],o[1],o[2]);
    Eigen::Vector3d b = 2.0 * A * ox + bw;

    // Largest local coordinate in the ring: strictly positive because the
    // collapsing edge endpoints belong to the ring and differ from its
    // midpoint. All rank/consistency thresholds are scaled by this length so
    // the same relative accuracy holds at every physical model scale.
    double ringScale = 0.0;
    for(VFIterator x(v0); !x.End(); ++x)
      for(int j=0;j<3;++j)
        ringScale = std::max(ringScale, double(local(x.F()->cP(j)).Norm()));
    for(VFIterator x(v1); !x.End(); ++x)
      for(int j=0;j<3;++j)
        ringScale = std::max(ringScale, double(local(x.F()->cP(j)).Norm()));

    Eigen::Vector3d u;

    auto finiteVector = [](const Eigen::Vector3d &v)
    {
      for(int i=0;i<3;++i) if(!std::isfinite(v[i])) return false;
      return true;
    };

    if(n.norm() <= kRankRelEps * ringScale * ringScale)
    {
      // Identically zero equality: 0 . u = d. Feasible only if d vanishes as
      // well; then take the unconstrained minimum, closest to the midpoint.
      if(std::abs(d) > kConsistencyRelEps * ringScale * ringScale * ringScale)
        return false;

      // min u^T A u + b.u, i.e. 2 A u = -b, minimum-norm least squares.
      Eigen::JacobiSVD<Eigen::Matrix3d> svdA(A, Eigen::ComputeFullU | Eigen::ComputeFullV);
      Eigen::Vector3d sv = svdA.singularValues();
      double tol = kRankRelEps * sv(0);
      Eigen::Vector3d rhs = -0.5 * b;
      Eigen::Vector3d sol = Eigen::Vector3d::Zero();
      Eigen::Vector3d nullResidual = Eigen::Vector3d::Zero();
      for(int i=0;i<3;++i)
      {
        if(sv(i) > tol) sol += (svdA.matrixU().col(i).dot(rhs) / sv(i)) * svdA.matrixV().col(i);
        else            nullResidual += (svdA.matrixU().col(i).dot(rhs)) * svdA.matrixU().col(i);
      }
      double rhsScale = std::max(rhs.norm(), tol * ringScale);
      if(nullResidual.norm() > kConsistencyRelEps * std::max(rhsScale, ringScale))
        return false; // objective unbounded: no finite minimum
      u = sol;
    }
    else
    {
      // Particular point on the plane (minimum norm) and an orthonormal basis
      // Z of its direction space.
      Eigen::Vector3d u0 = (d / n.squaredNorm()) * n;
      Eigen::JacobiSVD<Eigen::Matrix<double,3,1> > svdN(n, Eigen::ComputeFullU | Eigen::ComputeFullV);
      Eigen::Matrix<double,3,2> Z;
      Z.col(0) = svdN.matrixU().col(1);
      Z.col(1) = svdN.matrixU().col(2);

      // Reduce to min_z  z^T H z + g.z (+ const), H = Z^T A Z (PSD).
      Eigen::Matrix2d H = Z.transpose() * A * Z;
      Eigen::Vector2d g = Z.transpose() * (2.0 * A * u0 + b);

      Eigen::JacobiSVD<Eigen::Matrix2d> svdH(H, Eigen::ComputeFullU | Eigen::ComputeFullV);
      Eigen::Vector2d sh = svdH.singularValues();
      double hTol = kRankRelEps * sh(0);
      Eigen::Vector2d rhs = -0.5 * g;
      Eigen::Vector2d z = Eigen::Vector2d::Zero();
      Eigen::Vector2d hNullResidual = Eigen::Vector2d::Zero();
      for(int i=0;i<2;++i)
      {
        if(sh(i) > hTol) z += (svdH.matrixU().col(i).dot(rhs) / sh(i)) * svdH.matrixV().col(i);
        else             hNullResidual += (svdH.matrixU().col(i).dot(rhs)) * svdH.matrixU().col(i);
      }
      double gScale = std::max(g.norm(), hTol * ringScale);
      if(hNullResidual.norm() > kConsistencyRelEps * std::max(gScale, ringScale))
        return false; // objective unbounded along the constraint plane

      // Tie-break: among all minimizers pick the one closest to the midpoint
      // (local origin). The pseudoinverse solution z lives in the range of H;
      // remove its component along every null direction of H by projecting
      // u0 + Z z onto the corresponding null space.
      for(int i=0;i<2;++i)
        if(sh(i) <= hTol)
        {
          Eigen::Vector2d nullDir = svdH.matrixV().col(i);
          double t = -nullDir.dot(Z.transpose() * u0 + z);
          z += t * nullDir;
        }
      u = u0 + Z * z;
    }

    if(!finiteVector(u)) return false;

    // Final gate: the returned point must be finite and satisfy the volume
    // equality to a tolerance tighter than the per-collapse volume budget.
    double volResidual = std::abs(n.dot(u) - d);
    double volScale = ringScale * ringScale * ringScale;
    if(n.norm() > kRankRelEps * ringScale * ringScale &&
       volResidual > 1e-11 * volScale)
      return false;

    Eigen::Vector3d x = ox + u;
    result = CoordType(x[0], x[1], x[2]);
    return std::isfinite(result[0]) && std::isfinite(result[1]) && std::isfinite(result[2]);
  }
};

} // namespace tri
} // namespace vcg

#endif // __VCG_VOLUME_PRESERVATION__
