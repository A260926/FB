/////////////////////////////////////////////
//
// Optional topology-preserving mode for the
// Fast Quadric Mesh Simplification library.
//
// This header is included by Simplify.h from inside
// namespace Simplify (after the mesh structures and the
// global vertices/triangles arrays are declared). It has
// no includes of its own on purpose; everything it needs
// (<vector>, <set>, <map>, <algorithm>, <stdexcept>,
// <math.h>, <float.h>) is pulled in by Simplify.h at
// global scope.
//
// Two pieces:
//
//  - validate_manifold_input(): pre-check used by
//    TopologyMode::PreserveManifold. Throws
//    std::invalid_argument when the public
//    vertices/triangles arrays do not describe a finite,
//    non-degenerate, consistently oriented 2-manifold
//    triangle surface. Multiple connected components,
//    boundary loops and unused vertices are accepted.
//    The function is read-only: on failure the public
//    arrays are left untouched.
//
//  - class ManifoldGuard: tracks, for every vertex, the
//    living incident triangles and decides locally
//    (inside the star of the collapsed edge) whether an
//    edge collapse keeps the mesh a 2-manifold triangle
//    surface of the same topological type. Adjacency is
//    rebuilt once per iteration (right after
//    update_mesh()) and synced incrementally after every
//    accepted collapse.
//
/////////////////////////////////////////////

#ifndef FQMS_TOPOLOGY_PRESERVE_H
#define FQMS_TOPOLOGY_PRESERVE_H

// Validate that the public vertices/triangles arrays describe a
// finite, non-degenerate, consistently oriented 2-manifold triangle
// surface (boundaries, multiple components and unused vertices are
// fine). Throws std::invalid_argument otherwise. Read-only.
inline void validate_manifold_input()
{
	// 1. vertex positions must be finite
	for(size_t i=0;i<vertices.size();i++)
	{
		const vec3f &p=vertices[i].p;
		bool finite=(p.x==p.x)&&(p.y==p.y)&&(p.z==p.z)
			&&fabs(p.x)<=DBL_MAX&&fabs(p.y)<=DBL_MAX&&fabs(p.z)<=DBL_MAX;
		if(!finite)
			throw std::invalid_argument("Simplify: vertex position is not finite");
	}
	// 2. indices in range, triangles non-degenerate
	for(size_t i=0;i<triangles.size();i++)
	{
		const Triangle &t=triangles[i];
		for(int j=0;j<3;j++)
			if(t.v[j]<0||t.v[j]>=(int)vertices.size())
				throw std::invalid_argument("Simplify: triangle vertex index out of range");
		if(t.v[0]==t.v[1]||t.v[1]==t.v[2]||t.v[2]==t.v[0])
			throw std::invalid_argument("Simplify: degenerate triangle");
	}
	// 3. no duplicate triangles (same vertex set)
	{
		std::set<std::vector<int> > seen;
		for(size_t i=0;i<triangles.size();i++)
		{
			const Triangle &t=triangles[i];
			std::vector<int> key(t.v,t.v+3);
			std::sort(key.begin(),key.end());
			if(!seen.insert(key).second)
				throw std::invalid_argument("Simplify: duplicate triangle");
		}
	}
	// 4. edges are manifold and consistently oriented
	{
		struct EdgeUse { int count; int dir; };
		std::map<std::pair<int,int>,EdgeUse> edges;
		for(size_t i=0;i<triangles.size();i++)
		{
			const Triangle &t=triangles[i];
			for(int j=0;j<3;j++)
			{
				int a=t.v[j],b=t.v[(j+1)%3];
				std::pair<int,int> key=a<b?std::make_pair(a,b):std::make_pair(b,a);
				int dir=a<b?1:-1;
				EdgeUse &e=edges[key];
				if(e.count==0) { e.count=1; e.dir=dir; }
				else
				{
					e.count++;
					if(e.count>2)
						throw std::invalid_argument("Simplify: non-manifold edge");
					if(e.dir==dir)
						throw std::invalid_argument("Simplify: conflicting triangle orientation");
				}
			}
		}
	}
	// 5. the triangles around every used vertex form one connected fan
	{
		std::vector<std::vector<int> > inc(vertices.size());
		for(size_t i=0;i<triangles.size();i++)
			for(int j=0;j<3;j++)
				inc[triangles[i].v[j]].push_back((int)i);
		for(size_t u=0;u<inc.size();u++)
		{
			if(inc[u].empty()) continue; // unused vertices are allowed
			std::map<int,int> nxt,prv;
			for(size_t k=0;k<inc[u].size();k++)
			{
				const Triangle &t=triangles[inc[u][k]];
				int s=0; while(t.v[s]!=(int)u) s++;
				int a=t.v[(s+1)%3],b=t.v[(s+2)%3];
				if(nxt.count(a)||prv.count(b))
					throw std::invalid_argument("Simplify: non-manifold vertex fan");
				nxt[a]=b; prv[b]=a;
			}
			int start=-1;
			for(std::map<int,int>::iterator it=nxt.begin();it!=nxt.end();++it)
				if(!prv.count(it->first)) { start=it->first; break; }
			if(start<0) start=nxt.begin()->first; // closed fan
			int walked=0,cur=start,total=(int)nxt.size();
			while(walked<total)
			{
				std::map<int,int>::iterator it=nxt.find(cur);
				if(it==nxt.end()) break;
				walked++; cur=it->second;
			}
			if(walked!=total)
				throw std::invalid_argument("Simplify: vertex fan is not connected");
		}
	}
}

// Tracks the living triangle neighbourhood of every vertex and
// decides whether an edge collapse preserves the topological type.
class ManifoldGuard
{
	public:

	bool enabled;
	std::vector<std::vector<int> > star; // vertex -> living incident triangle ids

	ManifoldGuard() : enabled(false) {}
	explicit ManifoldGuard(bool on) : enabled(false) { if(on) { build(); enabled=true; } }

	bool active() const { return enabled; }

	// (Re)build adjacency from the living triangles. Called once
	// per iteration, right after update_mesh() renumbers things.
	void build()
	{
		star.clear();
		star.resize(vertices.size());
		for(size_t i=0;i<triangles.size();i++)
		{
			if(triangles[i].deleted) continue;
			for(int j=0;j<3;j++)
				star[triangles[i].v[j]].push_back((int)i);
		}
	}
	void rebuild() { build(); }

	// May edge i0-i1 collapse (i0 survives) without changing the
	// topological type? Only the star of the two endpoints is inspected.
	bool collapse_legal(int i0,int i1) const
	{
		const std::vector<int> &s0=star[i0];
		const std::vector<int> &s1=star[i1];
		// living triangles on the edge, and their opposite vertices
		int edge_tid[2]={-1,-1},edge_opp[2]={-1,-1},n_edge=0;
		for(size_t k=0;k<s0.size();k++)
		{
			int tid=s0[k];
			if(!has_tid(s1,tid)) continue;
			if(n_edge<2) edge_tid[n_edge]=tid;
			n_edge++;
		}
		if(n_edge<1||n_edge>2) return false; // not an edge / corrupt state
		for(int k=0;k<n_edge;k++)
		{
			const Triangle &t=triangles[edge_tid[k]];
			for(int j=0;j<3;j++) { int x=t.v[j]; if(x!=i0&&x!=i1) { edge_opp[k]=x; break; } }
		}
		// a whole component must not collapse into a point:
		// no edge-only or point-only fragments are left behind
		if((int)s0.size()==n_edge&&(int)s1.size()==n_edge) return false;
		// an interior edge between two boundary vertices is a chord:
		// collapsing it would pinch two boundary segments together
		bool boundary_edge=(n_edge==1);
		if(!boundary_edge&&boundary_vertex(i0)&&boundary_vertex(i1)) return false;
		// link condition: the common neighbours of the endpoints are
		// exactly the vertices opposite to the collapsed edge
		std::set<int> n0,n1;
		collect_neighbors(s0,i0,n0);
		collect_neighbors(s1,i1,n1);
		for(int k=0;k<n_edge;k++)
			if(!n0.count(edge_opp[k])||!n1.count(edge_opp[k])) return false;
		for(std::set<int>::iterator it=n0.begin();it!=n0.end();++it)
			if(n1.count(*it))
			{
				bool is_opp=false;
				for(int k=0;k<n_edge;k++) if(edge_opp[k]==*it) is_opp=true;
				if(!is_opp) return false; // extra common neighbour
			}
		// the collapse must not create duplicate triangles; this is
		// what stops the minimal closed shell (tetrahedron)
		std::set<std::pair<int,int> > pairs0;
		for(size_t k=0;k<s0.size();k++)
		{
			int tid=s0[k];
			if(is_edge_tid(tid,edge_tid,n_edge)) continue;
			int a,b; other_two(tid,i0,a,b);
			pairs0.insert(a<b?std::make_pair(a,b):std::make_pair(b,a));
		}
		for(size_t k=0;k<s1.size();k++)
		{
			int tid=s1[k];
			if(is_edge_tid(tid,edge_tid,n_edge)) continue;
			int a,b; other_two(tid,i1,a,b);
			std::pair<int,int> key=a<b?std::make_pair(a,b):std::make_pair(b,a);
			if(pairs0.count(key)) return false;
		}
		return true;
	}

	// Sync adjacency after an accepted collapse (i1 merged into i0).
	// triangles[] has already been updated by update_triangles().
	void on_collapse(int i0,int i1)
	{
		std::vector<int> s1=star[i1]; // copy
		for(size_t k=0;k<s1.size();k++)
		{
			int tid=s1[k];
			Triangle &t=triangles[tid];
			if(t.deleted)
			{
				for(int j=0;j<3;j++) remove_tid(star[t.v[j]],tid);
			}
			else
			{
				// triangle now references i0 instead of i1
				star[i0].push_back(tid);
			}
		}
		star[i1].clear();
	}

	private:

	static bool has_tid(const std::vector<int>&v,int tid)
	{
		for(size_t i=0;i<v.size();i++) if(v[i]==tid) return true;
		return false;
	}
	static void remove_tid(std::vector<int>&v,int tid)
	{
		for(size_t i=0;i<v.size();i++) if(v[i]==tid) { v.erase(v.begin()+(long)i); return; }
	}
	static bool is_edge_tid(int tid,const int*edge_tid,int n)
	{
		for(int i=0;i<n;i++) if(edge_tid[i]==tid) return true;
		return false;
	}
	// the two vertices of triangle tid that are not x
	static void other_two(int tid,int x,int&a,int&b)
	{
		const Triangle &t=triangles[tid];
		a=b=-1;
		for(int j=0;j<3;j++) { int y=t.v[j]; if(y==x) continue; if(a<0) a=y; else b=y; }
	}
	void collect_neighbors(const std::vector<int>&s,int x,std::set<int>&out) const
	{
		for(size_t k=0;k<s.size();k++)
		{
			const Triangle &t=triangles[s[k]];
			for(int j=0;j<3;j++) if(t.v[j]!=x) out.insert(t.v[j]);
		}
	}
	// living triangles incident to both x and y
	int shared_faces(int x,int y) const
	{
		const std::vector<int> &sx=star[x];
		int n=0;
		for(size_t k=0;k<sx.size();k++)
		{
			const Triangle &t=triangles[sx[k]];
			for(int j=0;j<3;j++) if(t.v[j]==y) { n++; break; }
		}
		return n;
	}
	bool boundary_vertex(int x) const
	{
		const std::vector<int> &sx=star[x];
		for(size_t k=0;k<sx.size();k++)
		{
			const Triangle &t=triangles[sx[k]];
			int s=0; while(t.v[s]!=x) s++;
			int a=t.v[(s+1)%3],b=t.v[(s+2)%3];
			if(shared_faces(x,a)==1) return true;
			if(shared_faces(x,b)==1) return true;
		}
		return false;
	}
};

#endif // FQMS_TOPOLOGY_PRESERVE_H
