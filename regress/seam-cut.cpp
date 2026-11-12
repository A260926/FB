// Regression driver for OpenMesh::Utils::cut_seams().
//
// Exercises the named scenarios boundary-chain, sphere-loop, torus-loop,
// empty and reject-branch, checking element counts, boundary loops, face
// components, per-vertex fans, per-corner UVs, vertex normals and the
// result -> source correspondence maps.  Prints one PASS line per scenario
// and OPENMESH_SEAM_TARGET_OK at the very end.

#include <OpenMesh/Core/Mesh/TriMesh_ArrayKernelT.hh>
#include <OpenMesh/Tools/Utils/SeamCutT.hh>
#include <OpenMesh/Tools/Utils/MeshCheckerT.hh>

#include <cstdlib>
#include <iostream>
#include <set>
#include <vector>

typedef OpenMesh::TriMesh_ArrayKernelT<> Mesh;
using OpenMesh::Utils::SeamCutMap;
using OpenMesh::Utils::cut_seams;

static void fail(int line, const char* expr)
{
  std::cerr << "FAIL line " << line << ": " << expr << std::endl;
  std::exit(1);
}
#define CHECK(cond) do { if (!(cond)) fail(__LINE__, #cond); } while (0)

//== mesh factories ===========================================================

// 3x3 vertex grid, 8 triangles, per-corner UVs and vertex normals requested.
static void make_grid(Mesh& m, std::vector<Mesh::VertexHandle>& v, bool isolated)
{
  m.request_vertex_normals();
  m.request_halfedge_texcoords2D();
  v.clear();
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 3; ++x)
    {
      v.push_back(m.add_vertex(Mesh::Point(x, y, 0)));
      m.set_normal(v.back(), Mesh::Normal(x + 1, y + 1, 1));
    }
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 2; ++x)
    {
      int i = y * 3 + x;
      CHECK(m.add_face(v[i], v[i + 1], v[i + 4]).is_valid());
      CHECK(m.add_face(v[i], v[i + 4], v[i + 3]).is_valid());
    }
  for (auto f = m.faces_begin(); f != m.faces_end(); ++f)
    for (auto h = m.fh_iter(*f); h.is_valid(); ++h)
    {
      auto p = m.point(m.to_vertex_handle(*h));
      m.set_texcoord2D(*h, Mesh::TexCoord2D(p[0] + 10 * f->idx(), p[1]));
    }
  if (isolated)
    m.add_vertex(Mesh::Point(9, 9, 9));
}

// closed octahedron: 6 vertices, 12 edges, 8 faces
static void make_octahedron(Mesh& m, std::vector<Mesh::VertexHandle>& v)
{
  m.request_vertex_normals();
  m.request_halfedge_texcoords2D();
  v.clear();
  v.push_back(m.add_vertex(Mesh::Point( 1, 0, 0))); // 0
  v.push_back(m.add_vertex(Mesh::Point(-1, 0, 0))); // 1
  v.push_back(m.add_vertex(Mesh::Point( 0, 1, 0))); // 2
  v.push_back(m.add_vertex(Mesh::Point( 0,-1, 0))); // 3
  v.push_back(m.add_vertex(Mesh::Point( 0, 0, 1))); // 4
  v.push_back(m.add_vertex(Mesh::Point( 0, 0,-1))); // 5
  const int f[8][3] = {{0,2,4},{2,1,4},{1,3,4},{3,0,4},
                       {2,0,5},{1,2,5},{3,1,5},{0,3,5}};
  for (int i = 0; i < 8; ++i)
    CHECK(m.add_face(v[f[i][0]], v[f[i][1]], v[f[i][2]]).is_valid());
  for (size_t i = 0; i < v.size(); ++i)
    m.set_normal(v[i], Mesh::Normal(m.point(v[i])[0], m.point(v[i])[1], m.point(v[i])[2]));
  for (auto f = m.faces_begin(); f != m.faces_end(); ++f)
  {
    int k = 0;
    for (auto h = m.fh_iter(*f); h.is_valid(); ++h, ++k)
      m.set_texcoord2D(*h, Mesh::TexCoord2D(f->idx(), k));
  }
}

// 4x4 periodic grid: 16 vertices, 48 edges, 32 faces, genus 1
static void make_torus(Mesh& m, std::vector<std::vector<Mesh::VertexHandle> >& v)
{
  m.request_vertex_normals();
  m.request_halfedge_texcoords2D();
  v.assign(4, std::vector<Mesh::VertexHandle>(4));
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
    {
      v[i][j] = m.add_vertex(Mesh::Point(i, j, (i + j) % 2));
      m.set_normal(v[i][j], Mesh::Normal(i, j, 1));
    }
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
    {
      const int i1 = (i + 1) % 4, j1 = (j + 1) % 4;
      CHECK(m.add_face(v[i][j], v[i1][j], v[i1][j1]).is_valid());
      CHECK(m.add_face(v[i][j], v[i1][j1], v[i][j1]).is_valid());
    }
  for (auto f = m.faces_begin(); f != m.faces_end(); ++f)
  {
    int k = 0;
    for (auto h = m.fh_iter(*f); h.is_valid(); ++h, ++k)
      m.set_texcoord2D(*h, Mesh::TexCoord2D(2 * f->idx(), 3 * k));
  }
}

//== independent connectivity checks ==========================================

// walk every boundary loop with next/prev navigation; returns loop count
static int count_boundary_loops(const Mesh& m)
{
  std::vector<char> vis(m.n_halfedges(), 0);
  int loops = 0;
  for (unsigned int i = 0; i < m.n_halfedges(); ++i)
  {
    Mesh::HalfedgeHandle h(i);
    if (m.face_handle(h).is_valid() || vis[i])
      continue;
    ++loops;
    size_t len = 0;
    Mesh::HalfedgeHandle cur = h;
    do
    {
      CHECK(!vis[cur.idx()]);
      CHECK(!m.face_handle(cur).is_valid());
      vis[cur.idx()] = 1;
      cur = m.next_halfedge_handle(cur);
      CHECK(cur.is_valid());
      ++len;
      CHECK(len <= m.n_halfedges());
    } while (cur != h);
    // the same loop must be walkable backwards with prev
    size_t back = 0;
    cur = h;
    do
    {
      cur = m.prev_halfedge_handle(cur);
      CHECK(cur.is_valid());
      ++back;
      CHECK(back <= m.n_halfedges());
    } while (cur != h);
    CHECK(back == len);
  }
  for (unsigned int i = 0; i < m.n_halfedges(); ++i)
  {
    Mesh::HalfedgeHandle h(i);
    if (!m.face_handle(h).is_valid())
      CHECK(vis[i]); // every boundary halfedge is part of exactly one loop
  }
  return loops;
}

// face components via adjacency across edges
static int count_components(const Mesh& m)
{
  std::vector<char> vis(m.n_faces(), 0);
  int comp = 0;
  for (unsigned int fi = 0; fi < m.n_faces(); ++fi)
  {
    if (vis[fi])
      continue;
    ++comp;
    std::vector<unsigned int> stack(1, fi);
    vis[fi] = 1;
    while (!stack.empty())
    {
      const unsigned int f = stack.back();
      stack.pop_back();
      Mesh::HalfedgeHandle h = m.halfedge_handle(Mesh::FaceHandle(f));
      for (int k = 0; k < 3; ++k)
      {
        Mesh::FaceHandle of = m.face_handle(m.opposite_halfedge_handle(h));
        if (of.is_valid() && !vis[of.idx()])
        {
          vis[of.idx()] = 1;
          stack.push_back(of.idx());
        }
        h = m.next_halfedge_handle(h);
      }
    }
  }
  return comp;
}

// every vertex must be incident to a single connected fan of faces
static void check_single_fans(const Mesh& m)
{
  for (auto v = m.vertices_begin(); v != m.vertices_end(); ++v)
  {
    std::vector<int> faces;
    for (auto f = m.cvf_iter(*v); f.is_valid(); ++f)
      faces.push_back(f->idx());
    if (faces.empty())
      continue; // isolated vertex
    // adjacency between incident faces via edges incident to the vertex
    std::vector<std::vector<int> > edge_faces(2 * m.n_edges());
    for (size_t i = 0; i < faces.size(); ++i)
    {
      Mesh::HalfedgeHandle h = m.halfedge_handle(Mesh::FaceHandle(faces[i]));
      for (int k = 0; k < 3; ++k)
      {
        if (m.to_vertex_handle(h) == *v || m.from_vertex_handle(h) == *v)
          edge_faces[m.edge_handle(h).idx()].push_back(faces[i]);
        h = m.next_halfedge_handle(h);
      }
    }
    std::vector<char> in_fan(m.n_faces(), 0), reach(m.n_faces(), 0);
    for (size_t i = 0; i < faces.size(); ++i)
      in_fan[faces[i]] = 1;
    std::vector<int> stack(1, faces[0]);
    reach[faces[0]] = 1;
    size_t n_reach = 0;
    while (!stack.empty())
    {
      const int f = stack.back();
      stack.pop_back();
      ++n_reach;
      Mesh::HalfedgeHandle h = m.halfedge_handle(Mesh::FaceHandle(f));
      for (int k = 0; k < 3; ++k)
      {
        if (m.to_vertex_handle(h) == *v || m.from_vertex_handle(h) == *v)
        {
          const std::vector<int>& ef = edge_faces[m.edge_handle(h).idx()];
          for (size_t i = 0; i < ef.size(); ++i)
            if (in_fan[ef[i]] && !reach[ef[i]])
            {
              reach[ef[i]] = 1;
              stack.push_back(ef[i]);
            }
        }
        h = m.next_halfedge_handle(h);
      }
    }
    CHECK(n_reach == faces.size()); // fan around the vertex is connected
  }
}

//== map / attribute verification =============================================

static void check_maps_and_attributes(const Mesh& src, const Mesh& res,
                                      const SeamCutMap& map,
                                      const std::set<int>& seams)
{
  CHECK(map.vertex_source.size() == res.n_vertices());
  CHECK(map.face_source.size() == res.n_faces());
  CHECK(map.halfedge_source.size() == res.n_halfedges());

  // each source face exactly once, corner sources and winding preserved
  std::vector<int> face_count(src.n_faces(), 0);
  for (unsigned int fi = 0; fi < res.n_faces(); ++fi)
  {
    const int sf = map.face_source[fi];
    CHECK(sf >= 0 && sf < (int)src.n_faces());
    ++face_count[sf];
    Mesh::HalfedgeHandle rh = res.halfedge_handle(Mesh::FaceHandle(fi));
    for (int k = 0; k < 3; ++k)
    {
      const int sh = map.halfedge_source[rh.idx()];
      CHECK(sh >= 0 && sh < (int)src.n_halfedges());
      const Mesh::HalfedgeHandle shh(sh);
      CHECK(src.face_handle(shh).is_valid());
      CHECK(src.face_handle(shh).idx() == sf);
      CHECK(map.vertex_source[res.to_vertex_handle(rh).idx()] ==
            src.to_vertex_handle(shh).idx());
      CHECK(map.vertex_source[res.from_vertex_handle(rh).idx()] ==
            src.from_vertex_handle(shh).idx());
      rh = res.next_halfedge_handle(rh);
    }
  }
  for (size_t i = 0; i < face_count.size(); ++i)
    CHECK(face_count[i] == 1);

  // vertex sources valid, positions and normals carried over
  for (unsigned int vi = 0; vi < res.n_vertices(); ++vi)
  {
    const int sv = map.vertex_source[vi];
    CHECK(sv >= 0 && sv < (int)src.n_vertices());
    CHECK(res.point(Mesh::VertexHandle(vi)) ==
          src.point(Mesh::VertexHandle(sv)));
    if (src.has_vertex_normals())
    {
      CHECK(res.has_vertex_normals());
      CHECK(res.normal(Mesh::VertexHandle(vi)) ==
            src.normal(Mesh::VertexHandle(sv)));
    }
  }

  // per-corner UVs carried over halfedge by halfedge (both sides of uncut
  // edges keep their distinct values; new boundary halfedges carry the
  // value of the same-direction source halfedge)
  if (src.has_halfedge_texcoords2D())
  {
    CHECK(res.has_halfedge_texcoords2D());
    for (unsigned int hi = 0; hi < res.n_halfedges(); ++hi)
    {
      const int sh = map.halfedge_source[hi];
      CHECK(sh >= 0 && sh < (int)src.n_halfedges());
      CHECK(res.texcoord2D(Mesh::HalfedgeHandle(hi)) ==
            src.texcoord2D(Mesh::HalfedgeHandle(sh)));
    }
  }

  // edge-level check, independently from the source edges: a cut edge shows
  // up as two boundary edges, any other edge exactly once with its boundary
  // status unchanged
  std::vector<int> edge_count(src.n_edges(), 0);
  for (unsigned int ei = 0; ei < res.n_edges(); ++ei)
  {
    const Mesh::EdgeHandle re(ei);
    const Mesh::HalfedgeHandle rh = res.halfedge_handle(re, 0);
    const int se = src.edge_handle(Mesh::HalfedgeHandle(map.halfedge_source[rh.idx()])).idx();
    ++edge_count[se];
    if (seams.count(se))
      CHECK(res.is_boundary(re));
    else
      CHECK(res.is_boundary(re) == src.is_boundary(Mesh::EdgeHandle(se)));
  }
  for (unsigned int ei = 0; ei < src.n_edges(); ++ei)
    CHECK(edge_count[ei] == (seams.count((int)ei) ? 2 : 1));

  // boundary halfedges map to the same-direction source halfedge
  for (unsigned int hi = 0; hi < res.n_halfedges(); ++hi)
  {
    const Mesh::HalfedgeHandle rh(hi);
    if (res.face_handle(rh).is_valid())
      continue;
    const Mesh::HalfedgeHandle ro = res.opposite_halfedge_handle(rh);
    const Mesh::HalfedgeHandle sh(map.halfedge_source[rh.idx()]);
    const Mesh::HalfedgeHandle so(map.halfedge_source[ro.idx()]);
    CHECK(sh == src.opposite_halfedge_handle(so));
  }
}

static void check_wellformed(const Mesh& m)
{
  CHECK(OpenMesh::Utils::MeshCheckerT<Mesh>(m).check());
  check_single_fans(m);
}

//== scenarios ================================================================

static void scenario_boundary_chain()
{
  Mesh m;
  std::vector<Mesh::VertexHandle> v;
  make_grid(m, v, false);

  std::vector<Mesh::EdgeHandle> seams;
  seams.push_back(m.edge_handle(m.find_halfedge(v[4], v[7])));
  seams.push_back(m.edge_handle(m.find_halfedge(v[1], v[4])));
  seams.push_back(m.edge_handle(m.find_halfedge(v[7], v[4]))); // duplicate, reversed
  seams.push_back(m.edge_handle(m.find_halfedge(v[4], v[1]))); // duplicate, reversed

  std::set<int> seamset;
  seamset.insert(m.edge_handle(m.find_halfedge(v[1], v[4])).idx());
  seamset.insert(m.edge_handle(m.find_halfedge(v[4], v[7])).idx());

  Mesh r;
  SeamCutMap map;
  cut_seams(m, seams, r, map);

  CHECK(r.n_vertices() == 12);
  CHECK(r.n_edges() == 18);
  CHECK(r.n_faces() == 8);
  CHECK(count_boundary_loops(r) == 2);
  CHECK(count_components(r) == 2);
  check_wellformed(r);
  check_maps_and_attributes(m, r, map, seamset);

  // an uncut interior edge keeps the two distinct UVs of its two sides
  const Mesh::HalfedgeHandle h45 = m.find_halfedge(v[4], v[5]);
  CHECK(h45.is_valid() && !m.is_boundary(m.edge_handle(h45)));
  // locate the result edge originating from source edge (v4,v5)
  bool found = false;
  for (unsigned int ei = 0; ei < r.n_edges() && !found; ++ei)
  {
    Mesh::HalfedgeHandle h0 = r.halfedge_handle(Mesh::EdgeHandle(ei), 0);
    Mesh::HalfedgeHandle h1 = r.halfedge_handle(Mesh::EdgeHandle(ei), 1);
    if (map.halfedge_source[h0.idx()] == h45.idx() ||
        map.halfedge_source[h1.idx()] == h45.idx())
    {
      found = true;
      CHECK(!r.is_boundary(Mesh::EdgeHandle(ei)));
      CHECK(r.texcoord2D(h0) != r.texcoord2D(h1));
      CHECK(r.texcoord2D(h0) == m.texcoord2D(Mesh::HalfedgeHandle(map.halfedge_source[h0.idx()])));
      CHECK(r.texcoord2D(h1) == m.texcoord2D(Mesh::HalfedgeHandle(map.halfedge_source[h1.idx()])));
    }
  }
  CHECK(found);

  std::cout << "PASS boundary-chain" << std::endl;
}

static void scenario_sphere_loop()
{
  Mesh m;
  std::vector<Mesh::VertexHandle> v;
  make_octahedron(m, v);

  // equator loop 0-2-1-3-0, all edges interior (closed mesh)
  std::vector<Mesh::EdgeHandle> seams;
  std::set<int> seamset;
  const int loop[4][2] = {{0,2},{2,1},{1,3},{3,0}};
  for (int i = 0; i < 4; ++i)
  {
    Mesh::HalfedgeHandle h = m.find_halfedge(v[loop[i][0]], v[loop[i][1]]);
    CHECK(h.is_valid() && !m.is_boundary(m.edge_handle(h)));
    seams.push_back(m.edge_handle(h));
    seamset.insert(m.edge_handle(h).idx());
  }

  Mesh r;
  SeamCutMap map;
  cut_seams(m, seams, r, map);

  CHECK(r.n_vertices() == 10);
  CHECK(r.n_edges() == 16);
  CHECK(r.n_faces() == 8);
  CHECK(count_boundary_loops(r) == 2);
  CHECK(count_components(r) == 2);
  check_wellformed(r);
  check_maps_and_attributes(m, r, map, seamset);

  std::cout << "PASS sphere-loop" << std::endl;
}

static void scenario_torus_loop()
{
  Mesh m;
  std::vector<std::vector<Mesh::VertexHandle> > v;
  make_torus(m, v);
  CHECK(m.n_vertices() == 16 && m.n_edges() == 48 && m.n_faces() == 32);

  // non-separating meridian loop around the i=0 column
  std::vector<Mesh::EdgeHandle> seams;
  std::set<int> seamset;
  for (int k = 0; k < 4; ++k)
  {
    Mesh::HalfedgeHandle h = m.find_halfedge(v[0][k], v[0][(k + 1) % 4]);
    CHECK(h.is_valid() && !m.is_boundary(m.edge_handle(h)));
    seams.push_back(m.edge_handle(h));
    seamset.insert(m.edge_handle(h).idx());
  }

  Mesh r;
  SeamCutMap map;
  cut_seams(m, seams, r, map);

  CHECK(r.n_vertices() == 20);
  CHECK(r.n_edges() == 52);
  CHECK(r.n_faces() == 32);
  CHECK(count_components(r) == 1); // non-separating cut: still one piece
  CHECK(count_boundary_loops(r) == 2);
  check_wellformed(r);
  check_maps_and_attributes(m, r, map, seamset);

  std::cout << "PASS torus-loop" << std::endl;
}

static void scenario_empty()
{
  Mesh m;
  std::vector<Mesh::VertexHandle> v;
  make_grid(m, v, true); // includes one isolated vertex
  CHECK(m.n_vertices() == 10);

  Mesh r;
  SeamCutMap map;
  cut_seams(m, std::vector<Mesh::EdgeHandle>(), r, map);

  CHECK(r.n_vertices() == 10);
  CHECK(r.n_edges() == 16);
  CHECK(r.n_faces() == 8);
  CHECK(count_boundary_loops(r) == 1);
  CHECK(count_components(r) == 1);
  check_wellformed(r);
  check_maps_and_attributes(m, r, map, std::set<int>());

  // the isolated vertex is preserved exactly once
  int isolated = 0;
  for (auto it = r.vertices_begin(); it != r.vertices_end(); ++it)
    if (!r.cvf_iter(*it).is_valid())
      ++isolated;
  CHECK(isolated == 1);

  std::cout << "PASS empty" << std::endl;
}

static void scenario_reject_branch()
{
  Mesh m;
  std::vector<Mesh::VertexHandle> v;
  make_grid(m, v, false);

  // sentinel state that must survive every failed call
  Mesh sentinel;
  Mesh::VertexHandle s0 = sentinel.add_vertex(Mesh::Point(0, 0, 0));
  Mesh::VertexHandle s1 = sentinel.add_vertex(Mesh::Point(1, 0, 0));
  Mesh::VertexHandle s2 = sentinel.add_vertex(Mesh::Point(0, 1, 0));
  CHECK(sentinel.add_face(s0, s1, s2).is_valid());
  const SeamCutMap sentinel_map = { std::vector<int>(1, -7),
                                    std::vector<int>(1, -8),
                                    std::vector<int>(1, -9) };

  std::vector<std::vector<Mesh::EdgeHandle> > bad;
  // branch: seam degree 3 at the interior vertex v4
  bad.push_back(std::vector<Mesh::EdgeHandle>{
    m.edge_handle(m.find_halfedge(v[1], v[4])),
    m.edge_handle(m.find_halfedge(v[4], v[7])),
    m.edge_handle(m.find_halfedge(v[4], v[5])) });
  // selected boundary edge
  bad.push_back(std::vector<Mesh::EdgeHandle>{
    m.edge_handle(m.find_halfedge(v[0], v[1])) });
  // chain with a dangling interior end
  bad.push_back(std::vector<Mesh::EdgeHandle>{
    m.edge_handle(m.find_halfedge(v[1], v[4])) });
  // out-of-range handle
  bad.push_back(std::vector<Mesh::EdgeHandle>{ Mesh::EdgeHandle(999) });
  // invalid handle
  bad.push_back(std::vector<Mesh::EdgeHandle>{ Mesh::EdgeHandle() });

  for (size_t i = 0; i < bad.size(); ++i)
  {
    Mesh r;
    r.request_vertex_normals();
    Mesh::VertexHandle t0 = r.add_vertex(Mesh::Point(0, 0, 0));
    Mesh::VertexHandle t1 = r.add_vertex(Mesh::Point(1, 0, 0));
    Mesh::VertexHandle t2 = r.add_vertex(Mesh::Point(0, 1, 0));
    CHECK(r.add_face(t0, t1, t2).is_valid());
    SeamCutMap map = sentinel_map;
    bool thrown = false;
    try
    {
      cut_seams(m, bad[i], r, map);
    }
    catch (const std::invalid_argument&)
    {
      thrown = true;
    }
    CHECK(thrown);
    // result and map untouched
    CHECK(r.n_vertices() == 3 && r.n_faces() == 1);
    CHECK(map.vertex_source == sentinel_map.vertex_source);
    CHECK(map.face_source == sentinel_map.face_source);
    CHECK(map.halfedge_source == sentinel_map.halfedge_source);
  }

  // source and result must not alias
  {
    SeamCutMap map = sentinel_map;
    bool thrown = false;
    try
    {
      cut_seams(m, std::vector<Mesh::EdgeHandle>(), m, map);
    }
    catch (const std::invalid_argument&)
    {
      thrown = true;
    }
    CHECK(thrown);
    CHECK(map.vertex_source == sentinel_map.vertex_source);
  }

  std::cout << "PASS reject-branch" << std::endl;
}

int main()
{
  scenario_boundary_chain();
  scenario_sphere_loop();
  scenario_torus_loop();
  scenario_empty();
  scenario_reject_branch();
  std::cout << "OPENMESH_SEAM_TARGET_OK" << std::endl;
  return 0;
}
