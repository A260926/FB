/* ----------------------------------------------------------------------- *
 * This file is part of GEL, http://www.imm.dtu.dk/GEL
 * Copyright (C) the authors and DTU Informatics
 * For license and list of authors, see ../../doc/intro.pdf
 * ----------------------------------------------------------------------- */

#include <cfloat>
#include <cmath>
#include <limits>
#include <queue>
#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <vector>
#include <GEL/Geometry/AABox.h>
#include <GEL/Geometry/OBox.h>
#include <GEL/Geometry/BoundingTree.h>

using namespace std;
using namespace CGLA;


namespace
{
template <class _Tp>
inline const _Tp& my_min(const _Tp& __a, const _Tp& __b)
{
    return __b < __a ? __b : __a;
}
template <class _Tp>
inline const _Tp& my_max(const _Tp& __a, const _Tp& __b)
{
    return  __a < __b ? __b : __a;
}

// Closest point on triangle (p0,p1,p2), Ericson's Voronoi region method.
// Returns the point and the barycentric weights in corner order.
CGLA::Vec3d closest_on_triangle(const CGLA::Vec3d& p,
                                const CGLA::Vec3d& a,
                                const CGLA::Vec3d& b,
                                const CGLA::Vec3d& c,
                                std::array<double,3>& weights)
{
    const CGLA::Vec3d ab = b-a;
    const CGLA::Vec3d ac = c-a;
    const CGLA::Vec3d ap = p-a;
    const double d1 = dot(ab, ap);
    const double d2 = dot(ac, ap);
    if(d1 <= 0.0 && d2 <= 0.0)
    {
        weights = {1.0, 0.0, 0.0};
        return a;
    }
    const CGLA::Vec3d bp = p-b;
    const double d3 = dot(ab, bp);
    const double d4 = dot(ac, bp);
    if(d3 >= 0.0 && d4 <= d3)
    {
        weights = {0.0, 1.0, 0.0};
        return b;
    }
    const double vc = d1*d4 - d3*d2;
    if(vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
    {
        const double v = d1/(d1-d3);
        weights = {1.0-v, v, 0.0};
        return a + v*ab;
    }
    const CGLA::Vec3d cp = p-c;
    const double d5 = dot(ab, cp);
    const double d6 = dot(ac, cp);
    if(d6 >= 0.0 && d5 <= d6)
    {
        weights = {0.0, 0.0, 1.0};
        return c;
    }
    const double vb = d5*d2 - d1*d6;
    if(vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
    {
        const double w = d2/(d2-d6);
        weights = {1.0-w, 0.0, w};
        return a + w*ac;
    }
    const double va = d3*d6 - d5*d4;
    if(va <= 0.0 && (d4-d3) >= 0.0 && (d5-d6) >= 0.0)
    {
        const double w = (d4-d3)/((d4-d3)+(d5-d6));
        weights = {0.0, 1.0-w, w};
        return b + w*(c-b);
    }
    const double denom = 1.0/(va+vb+vc);
    const double v = vb*denom;
    const double w = vc*denom;
    weights = {1.0-v-w, v, w};
    return a + ab*v + ac*w;
}

// Outward-rounded float bounds for a set of double precision vertices.
void conservative_float_box(const CGLA::Vec3d& lo, const CGLA::Vec3d& hi,
                            CGLA::Vec3f& flo, CGLA::Vec3f& fhi)
{
    for(int k=0;k<3;++k)
    {
        flo[k] = std::nextafter(static_cast<float>(lo[k]), -FLT_MAX);
        fhi[k] = std::nextafter(static_cast<float>(hi[k]),  FLT_MAX);
    }
}

}

namespace Geometry
{

template<class BoxType>
bool BoundingTree<BoxType>::intersect(const CGLA::Vec3f& p , const CGLA::Vec3f& dir,
                                      float& tmin) const 
{
    return root->intersect(p,dir,tmin);
}

template<class BoxType>
void BoundingTree<BoxType>::intersect(Ray& r) const 
{
    root->intersect(r);
}

template<class BoxType>
int BoundingTree<BoxType>::intersect_cnt(const CGLA::Vec3f& p, 
                                         const CGLA::Vec3f& dir) const
{
    return root->intersect_cnt(p,dir);
}

template<class BoxType>
void BoundingTree<BoxType>::build(std::vector<Triangle>& triangles)
{
    delete root;
    root = Node::build(triangles);
}


template<class Node>
class HE
{
    const Node* node;
    float sq_dist_min;
    float sq_dist_max;
    float sgn;
    
public:
    
    HE() {}
    
    HE(const Vec3f& p, const Node*_node): 
    node(_node), sq_dist_min(FLT_MAX), sq_dist_max(FLT_MAX), sgn(0)
    
    {
        node->sq_distance(p,sq_dist_min,sq_dist_max, sgn);
    }
    
    float get_sq_dist_min() const {return sq_dist_min;}
    float get_sq_dist_max() const {return sq_dist_max;}
    
    float get_dist() const {return sgn * sqrt(sq_dist_min);}
    
    const Node* get_node() const 
    {
        return node;
    }
    
    bool operator<(const HE<Node>& r) const
    {
        return r.sq_dist_min< sq_dist_min;
    }
    
};



template<class BoxType>
float BoundingTree<BoxType>::compute_signed_distance(const CGLA::Vec3f& p, float minmax) const
{
    int N=100;
    vector<HE<Node> > Q(N);
    Q[0] = HE<Node>(p,root);
    
    HE<Node> *Q_beg = &Q[0];
    int Q_end = 1;
    int pushes = 1;
    while(const IntNode* n = dynamic_cast<const IntNode*>(Q[0].get_node()))
    {
        float q0_max= Q[0].get_sq_dist_max();
        //float q0_min= Q[0].get_sq_dist_min();
        pop_heap(Q_beg, Q_beg + Q_end);
        --Q_end;
        
        
        HE<Node> hl(p,n->get_left());
        if(hl.get_sq_dist_min() < (minmax + DIST_THRESH))
        {
            if(hl.get_sq_dist_max() < minmax)
                minmax = hl.get_sq_dist_max();
            
            Q[Q_end++] = hl;
            push_heap(Q_beg, Q_beg + Q_end);
            if(Q_end == N) 
            {
                Q.resize(N=2*N);
                Q_beg = &Q[0];
            }
            ++pushes;
        }
        
        HE<Node> hr(p,n->get_right());
        if(hr.get_sq_dist_min() < (minmax + DIST_THRESH))
        {
            if(hr.get_sq_dist_max() < minmax)
                minmax = hr.get_sq_dist_max();
            
            Q[Q_end++] = hr;
            push_heap(Q_beg, Q_beg + Q_end);
            if(Q_end == N)
            {
                Q.resize(N=2*N);
                Q_beg = &Q[0];
            }
            ++pushes;
        }
        
        //  			if((hr.get_sq_dist_min() > (q0_max + DIST_THRESH)) &&
        // 				 (hl.get_sq_dist_min() > (q0_max + DIST_THRESH)) )
        // 				{
        // 					cout.precision(4);
        // 					cout << q0_min << " " << q0_max << endl;
        // 					cout << hl.get_sq_dist_min() << endl;
        // 					cout << hr.get_sq_dist_min() << endl;
        // 					cout << typeid(*n).name() << endl;
        // 					if(const LeafNode* ll =dynamic_cast<const LeafNode*>(hl.get_node()))
        // 						{
        // 							ll->get_tri().print();
        // 							cout << sqr_length(p-ll->get_tri().get_v0()) << endl;
        // 							cout << sqr_length(p-ll->get_tri().get_v1()) << endl;
        // 							cout << sqr_length(p-ll->get_tri().get_v2()) << endl;
        // 							float d=FLT_MAX, s;
        // 							ll->get_tri().signed_distance(p,d,s);
        // 							cout << "Dist " << d << endl;
        // 						}
        // 					if(const LeafNode* lr =dynamic_cast<const LeafNode*>(hr.get_node()))
        // 						{
        // 							lr->get_tri().print();
        // 							cout << sqr_length(p-lr->get_tri().get_v0()) << endl;
        // 							cout << sqr_length(p-lr->get_tri().get_v1()) << endl;
        // 							cout << sqr_length(p-lr->get_tri().get_v2()) << endl;
        // 							float d=FLT_MAX, s;
        // 							lr->get_tri().signed_distance(p,d,s);
        // 							cout << "Dist " << d << endl;
        // 						}
        // 					cout << "P=" << p<< endl;
        // 				}
        
        assert((hr.get_sq_dist_min() < (q0_max + DIST_THRESH)) ||
               (hl.get_sq_dist_min() < (q0_max + DIST_THRESH)) );
        assert(Q_end > 0);
        assert(Q_end <=N);
    }
    return Q[0].get_dist();
}

template<class BoxType>
double BoundingTree<BoxType>::SurfaceNode::min_sq_dist(const CGLA::Vec3d& p) const
{
    double d2 = 0.0;
    for(int k=0;k<3;++k)
    {
        const double lo = static_cast<double>(box_min[k]);
        const double hi = static_cast<double>(box_max[k]);
        if(p[k] < lo)
        {
            const double d = p[k]-lo;
            d2 += d*d;
        }
        else if(p[k] > hi)
        {
            const double d = p[k]-hi;
            d2 += d*d;
        }
    }
    return d2;
}

template<class BoxType>
typename BoundingTree<BoxType>::SurfaceNode*
BoundingTree<BoxType>::build_surface_recursive(const std::vector<SurfaceTriangle>& tris,
                                               std::vector<std::size_t>& work,
                                               std::size_t begin, std::size_t end)
{
    SurfaceNode* node = new SurfaceNode();
    const std::size_t n = end-begin;
    if(n == 1)
    {
        const SurfaceTriangle& t = tris[work[begin]];
        node->leaf = work[begin];
        CGLA::Vec3d lo = v_min(t.v0, v_min(t.v1, t.v2));
        CGLA::Vec3d hi = v_max(t.v0, v_max(t.v1, t.v2));
        conservative_float_box(lo, hi, node->box_min, node->box_max);
        return node;
    }

    CGLA::Vec3d lo( FLT_MAX), hi(-FLT_MAX);
    for(std::size_t i=begin;i<end;++i)
    {
        const SurfaceTriangle& t = tris[work[i]];
        lo = v_min(lo, v_min(t.v0, v_min(t.v1, t.v2)));
        hi = v_max(hi, v_max(t.v0, v_max(t.v1, t.v2)));
    }

    const CGLA::Vec3d diff = hi-lo;
    int axis = 0;
    if(diff[1] > diff[axis]) axis = 1;
    if(diff[2] > diff[axis]) axis = 2;
    const double mid = 0.5*(lo[axis]+hi[axis]);

    std::size_t split = begin;
    for(std::size_t i=begin;i<end;++i)
    {
        const SurfaceTriangle& t = tris[work[i]];
        const double c = (t.v0[axis]+t.v1[axis]+t.v2[axis])/3.0;
        if(c < mid)
        {
            std::swap(work[i], work[split]);
            ++split;
        }
    }
    if(split == begin || split == end)
        split = begin + n/2; // guaranteed non-empty halves

    node->left  = build_surface_recursive(tris, work, begin, split);
    node->right = build_surface_recursive(tris, work, split, end);
    conservative_float_box(lo, hi, node->box_min, node->box_max);
    return node;
}

template<class BoxType>
void BoundingTree<BoxType>::build_surface(std::vector<SurfaceTriangle> triangles)
{
    if(triangles.empty())
        throw std::invalid_argument("BoundingTree::build_surface: empty triangle list");

    std::unordered_set<uint64_t> seen;
    seen.reserve(triangles.size()*2);
    for(const SurfaceTriangle& t: triangles)
    {
        if(t.source_id == 0)
            throw std::invalid_argument("BoundingTree::build_surface: source id must be non-zero");
        if(!seen.insert(t.source_id).second)
            throw std::invalid_argument("BoundingTree::build_surface: duplicate source id");
        const CGLA::Vec3d* verts[3] = {&t.v0, &t.v1, &t.v2};
        for(const CGLA::Vec3d* v: verts)
            for(int k=0;k<3;++k)
                if(!std::isfinite((*v)[k]))
                    throw std::invalid_argument("BoundingTree::build_surface: non-finite vertex");
        const double area2 = sqr_length(cross(t.v1-t.v0, t.v2-t.v0));
        if(!(area2 > 0.0))
            throw std::invalid_argument("BoundingTree::build_surface: degenerate triangle");
    }

    // Build entirely against local storage; on failure the previous
    // valid tree and its geometry are untouched.
    std::vector<SurfaceTriangle> new_triangles = std::move(triangles);
    std::vector<std::size_t> work(new_triangles.size());
    for(std::size_t i=0;i<work.size();++i) work[i] = i;
    SurfaceNode* new_root = build_surface_recursive(new_triangles, work, 0, work.size());

    delete surface_root;
    surface_root = new_root;
    surface_triangles = std::move(new_triangles);
}

template<class BoxType>
bool BoundingTree<BoxType>::closest_point(const CGLA::Vec3d& p,
                                          ClosestPointResult& witness) const
{
    for(int k=0;k<3;++k)
        if(!std::isfinite(p[k]))
            throw std::invalid_argument("BoundingTree::closest_point: non-finite query point");
    if(surface_root == nullptr)
        return false;

    std::size_t node_visits = 0;
    std::size_t triangle_tests = 0;
    double best_sd = std::numeric_limits<double>::infinity();
    uint64_t best_id = 0;
    CGLA::Vec3d best_point(0.0);
    std::array<double,3> best_bary{0.0, 0.0, 0.0};

    // Branch-and-bound depth first traversal of the actual hierarchy.
    std::vector<const SurfaceNode*> stack;
    stack.push_back(surface_root);
    while(!stack.empty())
    {
        const SurfaceNode* node = stack.back();
        stack.pop_back();
        ++node_visits;
        if(node->min_sq_dist(p) > best_sd)
            continue;

        if(node->leaf != static_cast<std::size_t>(-1))
        {
            ++triangle_tests;
            const SurfaceTriangle& t = surface_triangles[node->leaf];
            std::array<double,3> bary;
            const CGLA::Vec3d q = closest_on_triangle(p, t.v0, t.v1, t.v2, bary);
            const double sd = sqr_length(p-q);
            if(sd < best_sd || (sd == best_sd && t.source_id < best_id))
            {
                best_sd = sd;
                best_id = t.source_id;
                best_point = q;
                best_bary = bary;
            }
            continue;
        }
        // Visit the tighter child first to shrink the bound early.
        const double dl = node->left->min_sq_dist(p);
        const double dr = node->right->min_sq_dist(p);
        if(dl < dr)
        {
            if(dr <= best_sd) stack.push_back(node->right);
            if(dl <= best_sd) stack.push_back(node->left);
        }
        else
        {
            if(dl <= best_sd) stack.push_back(node->left);
            if(dr <= best_sd) stack.push_back(node->right);
        }
    }

    witness.source_id = best_id;
    witness.point = best_point;
    witness.barycentric = best_bary;
    witness.sq_dist = best_sd;
    witness.node_visits = node_visits;
    witness.triangle_tests = triangle_tests;
    return true;
}

template class BoundingTree<AABox>;
template class BoundingTree<OBox>;

}
