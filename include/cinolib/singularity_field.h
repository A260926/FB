/********************************************************************************
*  This file is part of CinoLib                                                 *
*  Copyright(C) 2016: Marco Livesu                                              *
*                                                                               *
*  The MIT License                                                              *
*                                                                               *
*  Permission is hereby granted, free of charge, to any person obtaining a      *
*  copy of this software and associated documentation files (the "Software"),   *
*  to deal in the Software without restriction, including without limitation    *
*  the rights to use, copy, modify, merge, publish, distribute, sublicense,     *
*  and/or sell copies of the Software, and to permit persons to whom the        *
*  Software is furnished to do so, subject to the following conditions:         *
*                                                                               *
*  The above copyright notice and this permission notice shall be included in   *
*  all copies or substantial portions of the Software.                          *
*                                                                               *
*  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR   *
*  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,     *
*  FITNESS FOR A PARTICULAR PURPOSE AND NON INFRINGEMENT. IN NO EVENT SHALL THE *
*  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER       *
*  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING      *
*  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS *
*  IN THE SOFTWARE.                                                             *
*                                                                               *
*  Author(s):                                                                   *
*                                                                               *
*     Marco Livesu (marco.livesu@gmail.com)                                     *
*     http://pers.ge.imati.cnr.it/livesu/                                       *
*                                                                               *
*     Italian National Research Council (CNR)                                   *
*     Institute for Applied Mathematics and Information Technologies (IMATI)    *
*     Via de Marini, 6                                                          *
*     16149 Genoa,                                                              *
*     Italy                                                                     *
*********************************************************************************/
#ifndef CINO_SINGULARITY_FIELD_H
#define CINO_SINGULARITY_FIELD_H

#include <cinolib/meshes/trimesh.h>
#include <cinolib/vector_field.h>
#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>
#include <vector>
#include <utility>

namespace cinolib
{

/* Four-direction (cross) field with prescribed singularities and integer
 * periods on a closed, oriented, connected triangle mesh of genus 0 or 1.
 *
 * The construction follows the trivial-connections approach:
 *
 *  - each face is equipped with a right handed orthonormal tangent frame
 *    (u,v), with v = face_normal x u;
 *  - adjacent frames are related by the Levi-Civita connection: unfolding
 *    the two faces of an edge onto a common plane defines a directed
 *    transport angle omega per dual edge (principal value in (-pi,pi],
 *    negated when the edge is traversed backwards);
 *  - a real correction beta per directed dual edge (antisymmetric under
 *    reversal) is computed as the UNIQUE minimizer of sum(beta^2/2) subject
 *    to the global constraints
 *        sum_{positive ring of v} beta = q[v]*pi/2 - K[v]   (per vertex)
 *        sum_{basis cycle j}    beta = periods[j]*pi/2 - sum_{cycle j} omega
 *    where K[v] = 2*pi - (sum of face angles at v) is the angle defect,
 *    used through its real-valued lift (never reduced modulo 2*pi), and the
 *    basis cycles are the tree/cotree generators of cinolib::homotopy_basis
 *    (root=0, globally_shortest=false, detach_loops=false), ordered by
 *    increasing id of the generator edge. One redundant vertex equation is
 *    dropped before the sparse solve and independently checked a posteriori;
 *  - the corrected connection omega+beta is integrated from an anchor
 *    direction on face 0, producing one unit tangent representative per
 *    face, published as a cinolib::VectorField, plus an integer matching
 *    (quarter turns) per directed dual edge such that
 *        theta_g - theta_f - omega_fg - beta_fg = quarter_turns_fg * pi/2
 *    holds exactly (up to 1e-8).
 *
 * Geometry is preprocessed once at construction; changing singularities,
 * periods or anchor reuses the same sparse factorization. The object owns an
 * independent snapshot of the input mesh: later changes to (or destruction
 * of) the source mesh do not affect it.
 *
 * Input errors (invalid mesh at construction, or invalid q/periods/anchor at
 * solve time) throw std::invalid_argument. Numerical failures make solve()
 * return false. In both cases the last successful field, matching and
 * diagnostics are preserved. Querying solve-dependent data before the first
 * successful solve throws std::logic_error.
 */
class SingularityField
{
    public:

    // right handed orthonormal tangent frame of a face: u x v = face normal
    struct Frame
    {
        vec3d u; // first axis
        vec3d v; // second axis (face normal x u)
    };

    // non contractible basis cycle of the dual graph, closed by construction
    struct Cycle
    {
        uint              generator_edge = 0; // the only edge of the cycle that is neither in the tree nor in the cotree
        std::vector<uint> edges;              // primal edges crossed by the cycle, in order; edges[0] == generator_edge
        std::vector<int>  signs;              // +1 if the cycle crosses edges[i] from its lower to its higher face id, -1 otherwise
    };

    struct Diagnostics
    {
        double max_constraint_residual = 0.0; // max |A*beta - b| over ALL constraints, dropped vertex equation included
        double dropped_vertex_residual = 0.0; // residual of the redundant vertex equation excluded from the solve
        double max_matching_error      = 0.0; // max |dtheta - omega - beta - quarter_turns*pi/2| over undirected dual edges
        double energy                  = 0.0; // sum of beta^2/2 over undirected dual edges
        uint   sparse_factorizations   = 0;   // how many times the sparse factorization was (re)built
        uint   matrix_rows             = 0;   // rows of the sparse normal-equation system
        uint   matrix_nonzeros         = 0;   // nonzeros of the sparse normal-equation system
    };

    //::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

    explicit SingularityField(const Trimesh<> & m); // throws std::invalid_argument on invalid input

    //::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

    /* q[v] is the singularity index at vertex v, in units of 1/4 (the index
     * is q[v]/4); sum(q) must equal 4*Euler_characteristic. periods has one
     * integer per basis cycle (2*genus entries): the integer number of
     * quarter turns accumulated by the corrected connection along each
     * handle. anchor is a nonzero, finite, tangent direction on face 0.
     * Returns false on numerical failure, preserving the previous state. */
    bool solve(const std::vector<int> & q,
               const std::vector<int> & periods,
               const vec3d            & anchor);

    //::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

    // geometry and topology, available right after construction
    uint num_verts()            const;
    uint num_edges()            const;
    uint num_faces()            const;
    uint genus()                const;
    int  euler_characteristic() const;

    const std::vector<Frame>  & frames()      const; // per face tangent frames
    const vec3d               & face_normal(const uint fid) const;
    const std::vector<double> & connections() const; // Levi-Civita transport angle per undirected dual edge (lower->higher face id)
    const std::vector<Cycle>  & cycles()      const; // basis cycles and their signed edge sequences
    std::pair<uint,uint>        edge_faces(const uint eid) const; // the two faces of an edge, lower id first

    //::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

    // solve-dependent quantities: throw std::logic_error before the first successful solve
    const std::vector<double> & corrections()  const; // beta per undirected dual edge (lower->higher face id)
    const std::vector<int>    & quarter_turns() const; // integer matching per undirected dual edge (lower->higher face id)
    const VectorField         & field()        const; // one unit tangent representative per face
    const Diagnostics         & diagnostics()  const;

    //::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

    // directed access: applies the antisymmetry of connection/correction/matching
    // (from,to must be the two faces of edge eid, in any order)
    double connection  (const uint eid, const uint from, const uint to) const;
    double correction  (const uint eid, const uint from, const uint to) const;
    int    quarter_turn(const uint eid, const uint from, const uint to) const;

    //::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

    private:

    Trimesh<> mesh_; // independent snapshot of the input

    uint nv_ = 0, ne_ = 0, nf_ = 0, genus_ = 0;
    int  chi_ = 0;

    std::vector<Frame>             frames_;     // per face
    std::vector<vec3d>             normals_;    // per face unit normal (by winding)
    std::vector<std::pair<uint,uint>> edge_faces_; // per edge: the two incident faces, lower id first
    std::vector<double>            omega_;      // per edge: Levi-Civita transport angle, lower->higher face id
    std::vector<double>            angle_defect_; // per vertex: K = 2*pi - sum of face angles
    std::vector<Cycle>             cycles_;     // 2*genus basis cycles
    std::vector<double>            cycle_omega_;// per cycle: signed sum of omega along the cycle
    std::vector<std::vector<std::pair<uint,uint>>> face_nbrs_; // per face: (edge, neighbor face) for BFS integration

    // constraint system A*beta = b (rows: nv_-1 vertex equations + 2*genus cycle equations)
    uint                                             dropped_vertex_ = 0;
    Eigen::SparseMatrix<double>                      A_;
    Eigen::SparseMatrix<double>                      S_; // A * A^T
    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> solver_;
    bool                                             factored_ = false;
    uint                                             factorizations_ = 0;

    // last successful solve
    bool              solved_ = false;
    std::vector<double> beta_;       // per edge (lower->higher face id)
    std::vector<int>    qt_;         // per edge (lower->higher face id)
    VectorField         field_;      // per face representative
    Diagnostics         diag_;

    //::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

    void   validate_mesh(const Trimesh<> & m) const;
    void   build_frames();
    void   build_cycles();
    void   build_connection();
    void   build_system();
    int    vertex_edge_sign(const uint vid, const uint eid) const; // incidence of a dual edge in a positive vertex ring
    bool   face_winds_edge(const uint fid, const uint v_from, const uint v_to) const;
    void   require_solved() const;
};

}

#endif // CINO_SINGULARITY_FIELD_H
