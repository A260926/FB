/////////////////////////////////////////////
//
// Tests for the optional topology-preserving mode of
// src.cmd/Simplify.h (Simplify::PreserveManifold).
//
// All inputs are generated in this file and the program
// takes no arguments: it runs every scenario, prints the
// grouped PASS lines and finally FQMS_TOPOLOGY_OK when
// everything passed.
//
// The checks rebuild edge incidence, oriented vertex fans,
// boundary loops and connected components independently
// from the resulting triangles, then compare Euler
// characteristic, boundary loop count and manifoldness
// per component between input and output.
//
/////////////////////////////////////////////

#include "src.cmd/Simplify.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

// ------------------------------------------------------------------
// mesh bundle + independent topology analysis
// ------------------------------------------------------------------

struct Mesh {
	std::vector<vec3f> p;               // vertex positions
	std::vector<std::array<int,3> > f;  // triangles
};

void add_vert(Mesh&m,double x,double y,double z){ m.p.push_back(vec3f(x,y,z)); }
void add_face(Mesh&m,int a,int b,int c){ std::array<int,3> f={{a,b,c}}; m.f.push_back(f); }

struct Report {
	bool ok;
	std::string why;
	int components;
	long V,E,F;
	// (Euler characteristic, boundary loop count) per component, sorted
	std::vector<std::pair<long,int> > comps;
	Report():ok(true),components(0),V(0),E(0),F(0){}
};

struct Dsu {
	std::vector<int> p;
	void init(int n){ p.resize(n); for(int i=0;i<n;i++) p[i]=i; }
	int find(int x){ while(p[x]!=x){ p[x]=p[p[x]]; x=p[x]; } return x; }
	void unite(int a,int b){ a=find(a); b=find(b); if(a!=b) p[a]=b; }
};

// Independent manifold analysis of a triangle soup.
Report analyze(const Mesh&m){
	Report r;
	const int nv=(int)m.p.size();
	const int nf=(int)m.f.size();
	// indices in range, non-degenerate triangles
	for(int i=0;i<nf;i++){
		const std::array<int,3>&f=m.f[i];
		for(int j=0;j<3;j++)
			if(f[j]<0||f[j]>=nv){ r.ok=false; r.why="vertex index out of range"; return r; }
		if(f[0]==f[1]||f[1]==f[2]||f[2]==f[0]){ r.ok=false; r.why="degenerate triangle"; return r; }
	}
	r.F=nf;
	if(nf==0) return r;
	// no duplicate triangles
	{
		std::set<std::array<int,3> > seen;
		for(int i=0;i<nf;i++){
			std::array<int,3> k=m.f[i];
			std::sort(k.begin(),k.end());
			if(!seen.insert(k).second){ r.ok=false; r.why="duplicate triangle"; return r; }
		}
	}
	// edge incidence + orientation
	struct EInfo { int count,dir; };
	std::map<std::pair<int,int>,EInfo> em;
	std::map<std::pair<int,int>,std::vector<int> > e2f;
	for(int i=0;i<nf;i++){
		for(int j=0;j<3;j++){
			int a=m.f[i][j],b=m.f[i][(j+1)%3];
			std::pair<int,int> key=a<b?std::make_pair(a,b):std::make_pair(b,a);
			int dir=a<b?1:-1;
			EInfo&e=em[key];
			if(e.count==0){ e.count=1; e.dir=dir; }
			else{
				e.count++;
				if(e.count>2){ r.ok=false; r.why="non-manifold edge"; return r; }
				if(e.dir==dir){ r.ok=false; r.why="conflicting triangle orientation"; return r; }
			}
			e2f[key].push_back(i);
		}
	}
	r.E=(long)em.size();
	// oriented fan around every vertex
	{
		std::vector<std::vector<int> > inc(nv);
		for(int i=0;i<nf;i++) for(int j=0;j<3;j++) inc[m.f[i][j]].push_back(i);
		for(int u=0;u<nv;u++){
			if(inc[u].empty()) continue;
			std::map<int,int> nxt,prv;
			for(size_t k=0;k<inc[u].size();k++){
				const std::array<int,3>&f=m.f[inc[u][k]];
				int s=0; while(f[s]!=u) s++;
				int a=f[(s+1)%3],b=f[(s+2)%3];
				if(nxt.count(a)||prv.count(b)){ r.ok=false; r.why="vertex fan not manifold"; return r; }
				nxt[a]=b; prv[b]=a;
			}
			int start=-1;
			for(std::map<int,int>::iterator it=nxt.begin();it!=nxt.end();++it)
				if(!prv.count(it->first)){ start=it->first; break; }
			if(start<0) start=nxt.begin()->first;
			int walked=0,cur=start,total=(int)nxt.size();
			while(walked<total){
				std::map<int,int>::iterator it=nxt.find(cur);
				if(it==nxt.end()) break;
				walked++; cur=it->second;
			}
			if(walked!=total){ r.ok=false; r.why="vertex fan disconnected"; return r; }
		}
	}
	// connected components over triangles
	Dsu d; d.init(nf);
	for(std::map<std::pair<int,int>,std::vector<int> >::iterator it=e2f.begin();it!=e2f.end();++it){
		const std::vector<int>&fs=it->second;
		for(size_t k=1;k<fs.size();k++) d.unite(fs[0],fs[k]);
	}
	std::map<int,std::vector<int> > compfaces;
	for(int i=0;i<nf;i++) compfaces[d.find(i)].push_back(i);
	r.components=(int)compfaces.size();
	// per component: Euler characteristic + boundary loops
	for(std::map<int,std::vector<int> >::iterator ci=compfaces.begin();ci!=compfaces.end();++ci){
		const std::vector<int>&faces=ci->second;
		std::set<int> vs;
		std::set<std::pair<int,int> > es;
		for(size_t k=0;k<faces.size();k++){
			const std::array<int,3>&f=m.f[faces[k]];
			for(int j=0;j<3;j++){
				vs.insert(f[j]);
				int a=f[j],b=f[(j+1)%3];
				es.insert(a<b?std::make_pair(a,b):std::make_pair(b,a));
			}
		}
		long chi=(long)vs.size()-(long)es.size()+(long)faces.size();
		// boundary loops of this component
		std::map<int,std::vector<int> > bg;
		for(std::set<std::pair<int,int> >::iterator e=es.begin();e!=es.end();++e){
			if(em[*e].count==1){
				bg[e->first].push_back(e->second);
				bg[e->second].push_back(e->first);
			}
		}
		int loops=0;
		if(!bg.empty()){
			for(std::map<int,std::vector<int> >::iterator it=bg.begin();it!=bg.end();++it)
				if(it->second.size()!=2){ r.ok=false; r.why="boundary is not a union of loops"; return r; }
			std::set<int> vis;
			for(std::map<int,std::vector<int> >::iterator it=bg.begin();it!=bg.end();++it){
				int u=it->first;
				if(vis.count(u)) continue;
				loops++;
				int cur=u,prev=-1;
				do{
					vis.insert(cur);
					int n0=bg[cur][0],n1=bg[cur][1];
					int nx=(n0!=prev)?n0:n1;
					prev=cur; cur=nx;
				}while(cur!=u);
			}
		}
		r.comps.push_back(std::make_pair(chi,loops));
	}
	std::sort(r.comps.begin(),r.comps.end());
	{
		std::set<int> used;
		for(int i=0;i<nf;i++) for(int j=0;j<3;j++) used.insert(m.f[i][j]);
		r.V=(long)used.size();
	}
	return r;
}

bool same_topology(const Report&a,const Report&b){
	if(!a.ok||!b.ok) return false;
	if(a.components!=b.components) return false;
	return a.comps==b.comps;
}

// ------------------------------------------------------------------
// mesh generators (all deterministic)
// ------------------------------------------------------------------

Mesh make_tetra(){
	Mesh m;
	add_vert(m, 1, 1, 1); add_vert(m, 1,-1,-1);
	add_vert(m,-1, 1,-1); add_vert(m,-1,-1, 1);
	add_face(m,0,1,2); add_face(m,0,3,1);
	add_face(m,0,2,3); add_face(m,1,3,2);
	return m;
}

Mesh make_octa(){
	Mesh m;
	add_vert(m, 1,0,0); add_vert(m,-1,0,0);
	add_vert(m,0, 1,0); add_vert(m,0,-1,0);
	add_vert(m,0,0, 1); add_vert(m,0,0,-1);
	add_face(m,0,2,4); add_face(m,2,1,4); add_face(m,1,3,4); add_face(m,3,0,4);
	add_face(m,2,0,5); add_face(m,1,2,5); add_face(m,3,1,5); add_face(m,0,3,5);
	return m;
}

Mesh make_icosa(){
	Mesh m;
	const double t=(1.0+std::sqrt(5.0))/2.0;
	add_vert(m,-1, t,0); add_vert(m, 1, t,0); add_vert(m,-1,-t,0); add_vert(m, 1,-t,0);
	add_vert(m,0,-1, t); add_vert(m,0, 1, t); add_vert(m,0,-1,-t); add_vert(m,0, 1,-t);
	add_vert(m, t,0,-1); add_vert(m, t,0, 1); add_vert(m,-t,0,-1); add_vert(m,-t,0, 1);
	for(size_t i=0;i<m.p.size();i++) m.p[i].normalize();
	int f[20][3]={
		{0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},
		{1,5,9},{5,11,4},{11,10,2},{7,1,8},{10,7,6},
		{3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},
		{4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1}};
	for(int i=0;i<20;i++) add_face(m,f[i][0],f[i][1],f[i][2]);
	return m;
}

int sphere_midpoint(std::map<std::pair<int,int>,int>&cache,Mesh&out,int a,int b){
	std::pair<int,int> key=a<b?std::make_pair(a,b):std::make_pair(b,a);
	std::map<std::pair<int,int>,int>::iterator it=cache.find(key);
	if(it!=cache.end()) return it->second;
	vec3f p=(out.p[a]+out.p[b])*0.5;
	p.normalize();
	out.p.push_back(p);
	int id=(int)out.p.size()-1;
	cache[key]=id;
	return id;
}

Mesh subdivide_sphere(const Mesh&in){
	Mesh out;
	out.p=in.p;
	std::map<std::pair<int,int>,int> cache;
	for(size_t i=0;i<in.f.size();i++){
		int a=in.f[i][0],b=in.f[i][1],c=in.f[i][2];
		int ab=sphere_midpoint(cache,out,a,b);
		int bc=sphere_midpoint(cache,out,b,c);
		int ca=sphere_midpoint(cache,out,c,a);
		add_face(out,a,ab,ca); add_face(out,ab,b,bc);
		add_face(out,ca,bc,c); add_face(out,ab,bc,ca);
	}
	return out;
}

Mesh make_sphere(int levels){
	Mesh m=make_icosa();
	for(int i=0;i<levels;i++) m=subdivide_sphere(m);
	return m;
}

Mesh make_torus(int nu,int nv,double R,double r){
	Mesh m;
	const double pi=std::acos(-1.0);
	for(int i=0;i<nu;i++) for(int j=0;j<nv;j++){
		double u=2.0*pi*(double)i/(double)nu;
		double v=2.0*pi*(double)j/(double)nv;
		add_vert(m,(R+r*std::cos(v))*std::cos(u),
		           (R+r*std::cos(v))*std::sin(u),
		           r*std::sin(v));
	}
	for(int i=0;i<nu;i++) for(int j=0;j<nv;j++){
		int a=i*nv+j, b=((i+1)%nu)*nv+j;
		int c=((i+1)%nu)*nv+(j+1)%nv, d=i*nv+(j+1)%nv;
		add_face(m,a,b,c); add_face(m,a,c,d);
	}
	return m;
}

// flat n x n triangulated sheet on z=0; holes are cell ranges {x0,y0,x1,y1}
Mesh make_sheet(int n,const std::vector<std::array<int,4> >&holes,bool jitter){
	Mesh m;
	const int W=n+1;
	for(int j=0;j<W;j++) for(int i=0;i<W;i++){
		double x=(double)i,y=(double)j;
		if(jitter&&i>0&&i<n&&j>0&&j<n){
			x+=0.22*std::sin(i*12.9898+j*78.233);
			y+=0.22*std::cos(i*39.346+j*11.135);
		}
		add_vert(m,x,y,0.0);
	}
	for(int j=0;j<n;j++) for(int i=0;i<n;i++){
		bool hole=false;
		for(size_t h=0;h<holes.size();h++){
			const std::array<int,4>&r=holes[h];
			if(i>=r[0]&&i<r[2]&&j>=r[1]&&j<r[3]){ hole=true; break; }
		}
		if(hole) continue;
		int a=j*W+i,b=j*W+i+1,c=(j+1)*W+i+1,d=(j+1)*W+i;
		add_face(m,a,b,c); add_face(m,a,c,d);
	}
	return m;
}

Mesh make_two_shells(){
	Mesh a=make_icosa();
	Mesh b=a;
	for(size_t i=0;i<a.p.size();i++) a.p[i].x-=1.05;
	for(size_t i=0;i<b.p.size();i++) b.p[i].x+=1.05;
	Mesh m=a;
	int off=(int)a.p.size();
	for(size_t i=0;i<b.p.size();i++) m.p.push_back(b.p[i]);
	for(size_t i=0;i<b.f.size();i++)
		add_face(m,b.f[i][0]+off,b.f[i][1]+off,b.f[i][2]+off);
	return m;
}

// ------------------------------------------------------------------
// run helpers
// ------------------------------------------------------------------

void load_into_simplify(const Mesh&m){
	Simplify::vertices.clear();
	Simplify::triangles.clear();
	for(size_t i=0;i<m.p.size();i++){
		Simplify::Vertex v={};
		v.p=m.p[i];
		Simplify::vertices.push_back(v);
	}
	for(size_t i=0;i<m.f.size();i++){
		Simplify::Triangle t={};
		for(int j=0;j<3;j++) t.v[j]=m.f[i][j];
		Simplify::triangles.push_back(t);
	}
}

Mesh read_back(){
	Mesh m;
	for(size_t i=0;i<Simplify::vertices.size();i++)
		m.p.push_back(Simplify::vertices[i].p);
	for(size_t i=0;i<Simplify::triangles.size();i++){
		const Simplify::Triangle&t=Simplify::triangles[i];
		add_face(m,t.v[0],t.v[1],t.v[2]);
	}
	return m;
}

// ------------------------------------------------------------------
// case framework
// ------------------------------------------------------------------

bool fail_msg(const char*cn,const std::string&msg){
	std::printf("FAIL %s: %s\n",cn,msg.c_str());
	return false;
}

bool check(bool cond,const char*cn,const std::string&msg){
	return cond?true:fail_msg(cn,msg);
}

std::string faces_info(const Mesh&m){
	char buf[128];
	std::snprintf(buf,sizeof(buf),"faces=%ld",(long)m.f.size());
	return std::string(buf);
}

// ------------------------------------------------------------------
// closed-shell cases
// ------------------------------------------------------------------

const char*CN_TETRA="closed-shells/tetrahedron-keeps-4-faces";
const char*CN_OCTA="closed-shells/octahedron-stops-at-shell";
const char*CN_SPHERE="closed-shells/refined-sphere-reduces";
const char*CN_TORUS="closed-shells/torus-keeps-genus";
const char*CN_TWO="closed-shells/two-close-shells-stay-separate";

bool case_tetra(){
	Mesh in=make_tetra();
	Report ri=analyze(in);
	if(!check(ri.ok,CN_TETRA,"generated input invalid")) return false;
	load_into_simplify(in);
	Simplify::simplify_mesh(1,7,false,Simplify::PreserveManifold); // target far below reachable
	Mesh out=read_back();
	Report ro=analyze(out);
	if(!check(ro.ok,CN_TETRA,"output not manifold: "+ro.why)) return false;
	if(!check(out.f.size()==4,CN_TETRA,"minimal closed shell not preserved, "+faces_info(out))) return false;
	if(!check(same_topology(ri,ro),CN_TETRA,"topology changed")) return false;
	return true;
}

bool case_octa(){
	Mesh in=make_octa();
	Report ri=analyze(in);
	if(!check(ri.ok,CN_OCTA,"generated input invalid")) return false;
	load_into_simplify(in);
	Simplify::simplify_mesh(2,7,false,Simplify::PreserveManifold); // unrestricted mode goes to 2 degenerate faces
	Mesh out=read_back();
	Report ro=analyze(out);
	if(!check(ro.ok,CN_OCTA,"output not manifold: "+ro.why)) return false;
	if(!check(out.f.size()>=4,CN_OCTA,"shell collapsed below tetrahedron, "+faces_info(out))) return false;
	if(!check(out.f.size()<in.f.size(),CN_OCTA,"no reduction happened, "+faces_info(out))) return false;
	if(!check(same_topology(ri,ro),CN_OCTA,"topology changed")) return false;
	return true;
}

bool case_sphere(){
	Mesh in=make_sphere(2); // 320 faces
	Report ri=analyze(in);
	if(!check(ri.ok&&ri.components==1,CN_SPHERE,"generated input invalid")) return false;
	load_into_simplify(in);
	Simplify::simplify_mesh(40,7,false,Simplify::PreserveManifold);
	Mesh out=read_back();
	Report ro=analyze(out);
	if(!check(ro.ok,CN_SPHERE,"output not manifold: "+ro.why)) return false;
	if(!check(out.f.size()<in.f.size(),CN_SPHERE,"no reduction happened, "+faces_info(out))) return false;
	if(!check(out.f.size()<=60,CN_SPHERE,"reduction stalled too early, "+faces_info(out))) return false;
	if(!check(out.f.size()>=4,CN_SPHERE,"closed shell destroyed, "+faces_info(out))) return false;
	if(!check(same_topology(ri,ro),CN_SPHERE,"topology changed")) return false;
	return true;
}

bool case_torus(){
	Mesh in=make_torus(24,12,2.0,0.8); // 576 faces, genus 1
	Report ri=analyze(in);
	if(!check(ri.ok&&ri.components==1,CN_TORUS,"generated input invalid")) return false;
	load_into_simplify(in);
	Simplify::simplify_mesh(100,7,false,Simplify::PreserveManifold);
	Mesh out=read_back();
	Report ro=analyze(out);
	if(!check(ro.ok,CN_TORUS,"output not manifold: "+ro.why)) return false;
	if(!check(out.f.size()<in.f.size(),CN_TORUS,"no reduction happened, "+faces_info(out))) return false;
	if(!check(out.f.size()<=160,CN_TORUS,"reduction stalled too early, "+faces_info(out))) return false;
	if(!check(out.f.size()>=14,CN_TORUS,"torus collapsed below minimal triangulation, "+faces_info(out))) return false;
	if(!check(same_topology(ri,ro),CN_TORUS,"handle not preserved")) return false;
	return true;
}

bool case_two_shells(){
	Mesh in=make_two_shells(); // two icosahedra 0.1 apart, not connected
	Report ri=analyze(in);
	if(!check(ri.ok&&ri.components==2,CN_TWO,"generated input invalid")) return false;
	load_into_simplify(in);
	Simplify::simplify_mesh(8,7,false,Simplify::PreserveManifold);
	Mesh out=read_back();
	Report ro=analyze(out);
	if(!check(ro.ok,CN_TWO,"output not manifold: "+ro.why)) return false;
	if(!check(ro.components==2,CN_TWO,"shells merged or destroyed")) return false;
	if(!check(out.f.size()>=8,CN_TWO,"shells collapsed below two tetrahedra, "+faces_info(out))) return false;
	if(!check(same_topology(ri,ro),CN_TWO,"topology changed")) return false;
	return true;
}

// ------------------------------------------------------------------
// boundary cases
// ------------------------------------------------------------------

const char*CN_HOLES="boundaries/sheet-with-two-holes";
const char*CN_DISK="boundaries/disk-sheet-shrinks";
const char*CN_ONE="boundaries/single-triangle-survives";

bool case_two_hole_sheet(){
	std::vector<std::array<int,4> > holes;
	{ std::array<int,4> h={{3,3,5,5}}; holes.push_back(h); }
	{ std::array<int,4> h={{6,4,8,6}}; holes.push_back(h); } // 1-cell gap to first hole
	Mesh in=make_sheet(12,holes,false); // 272 faces, 3 boundary loops
	Report ri=analyze(in);
	if(!check(ri.ok&&ri.components==1,CN_HOLES,"generated input invalid")) return false;
	if(!check(ri.comps.size()==1&&ri.comps[0].second==3,CN_HOLES,"input loop count wrong")) return false;
	load_into_simplify(in);
	Simplify::simplify_mesh(10,7,false,Simplify::PreserveManifold); // below the reachable minimum
	Mesh out=read_back();
	Report ro=analyze(out);
	if(!check(ro.ok,CN_HOLES,"output not manifold: "+ro.why)) return false;
	if(!check(out.f.size()<in.f.size(),CN_HOLES,"no reduction happened, "+faces_info(out))) return false;
	if(!check(out.f.size()<=120,CN_HOLES,"reduction stalled too early, "+faces_info(out))) return false;
	if(!check(ro.comps.size()==1&&ro.comps[0].second==3,CN_HOLES,"boundary loops merged or destroyed")) return false;
	if(!check(same_topology(ri,ro),CN_HOLES,"topology changed")) return false;
	return true;
}

bool case_disk_sheet(){
	Mesh in=make_sheet(8,std::vector<std::array<int,4> >(),false); // 128 faces, 1 loop
	Report ri=analyze(in);
	if(!check(ri.ok&&ri.components==1,CN_DISK,"generated input invalid")) return false;
	load_into_simplify(in);
	Simplify::simplify_mesh(1,7,false,Simplify::PreserveManifold);
	Mesh out=read_back();
	Report ro=analyze(out);
	if(!check(ro.ok,CN_DISK,"output not manifold: "+ro.why)) return false;
	if(!check(out.f.size()<in.f.size(),CN_DISK,"no reduction happened, "+faces_info(out))) return false;
	if(!check(out.f.size()<=8,CN_DISK,"reduction stalled too early, "+faces_info(out))) return false;
	if(!check(out.f.size()>=1,CN_DISK,"disk destroyed")) return false;
	if(!check(same_topology(ri,ro),CN_DISK,"topology changed")) return false;
	return true;
}

bool case_single_triangle(){
	Mesh in;
	add_vert(in,0,0,0); add_vert(in,1,0,0); add_vert(in,0,1,0);
	add_face(in,0,1,2);
	Report ri=analyze(in);
	load_into_simplify(in);
	Simplify::simplify_mesh(0,7,false,Simplify::PreserveManifold);
	Mesh out=read_back();
	Report ro=analyze(out);
	if(!check(ro.ok,CN_ONE,"output not manifold: "+ro.why)) return false;
	if(!check(out.f.size()==1,CN_ONE,"last triangle destroyed, "+faces_info(out))) return false;
	if(!check(same_topology(ri,ro),CN_ONE,"topology changed")) return false;
	return true;
}

// ------------------------------------------------------------------
// lossless / reuse cases
// ------------------------------------------------------------------

const char*CN_COPLANAR="lossless-reuse/coplanar-sheet";
const char*CN_EMPTY="lossless-reuse/empty-mesh";
const char*CN_ISOLATED="lossless-reuse/isolated-vertices";
const char*CN_QUAD="lossless-reuse/quad-with-stray-vertices";
const char*CN_REUSE="lossless-reuse/no-stale-adjacency-state";

bool case_coplanar_lossless(){
	Mesh in=make_sheet(8,std::vector<std::array<int,4> >(),true); // 128 coplanar faces
	Report ri=analyze(in);
	if(!check(ri.ok,CN_COPLANAR,"generated input invalid")) return false;
	load_into_simplify(in);
	Simplify::simplify_mesh_lossless(false,Simplify::PreserveManifold);
	Mesh out=read_back();
	Report ro=analyze(out);
	if(!check(ro.ok,CN_COPLANAR,"output not manifold: "+ro.why)) return false;
	if(!check(out.f.size()<in.f.size(),CN_COPLANAR,"lossless made no progress, "+faces_info(out))) return false;
	if(!check(out.f.size()<=64,CN_COPLANAR,"lossless stalled too early, "+faces_info(out))) return false;
	if(!check(same_topology(ri,ro),CN_COPLANAR,"topology changed")) return false;
	return true;
}

bool case_empty_lossless(){
	Simplify::vertices.clear();
	Simplify::triangles.clear();
	Simplify::simplify_mesh_lossless(false,Simplify::PreserveManifold); // must not throw
	if(!check(Simplify::triangles.empty(),CN_EMPTY,"triangles appeared from nowhere")) return false;
	Simplify::simplify_mesh(0,7,false,Simplify::PreserveManifold); // must not throw either
	if(!check(Simplify::triangles.empty(),CN_EMPTY,"triangles appeared from nowhere")) return false;
	return true;
}

bool case_isolated_lossless(){
	Simplify::vertices.clear();
	Simplify::triangles.clear();
	for(int i=0;i<5;i++){
		Simplify::Vertex v={};
		v.p=vec3f((double)i,0,0);
		Simplify::vertices.push_back(v);
	}
	Simplify::simplify_mesh_lossless(false,Simplify::PreserveManifold); // unused vertices are legal input
	if(!check(Simplify::triangles.empty(),CN_ISOLATED,"triangles appeared from nowhere")) return false;
	return true;
}

bool case_quad_stray_lossless(){
	Mesh in;
	add_vert(in,0,0,0); add_vert(in,1,0,0); add_vert(in,1,1,0); add_vert(in,0,1,0);
	add_face(in,0,1,2); add_face(in,0,2,3);
	add_vert(in,5,5,5); add_vert(in,6,5,5); add_vert(in,7,5,5); // unused vertices
	Report ri=analyze(in);
	load_into_simplify(in);
	Simplify::simplify_mesh_lossless(false,Simplify::PreserveManifold);
	Mesh out=read_back();
	Report ro=analyze(out);
	if(!check(ro.ok,CN_QUAD,"output not manifold: "+ro.why)) return false;
	if(!check(out.f.size()>=1&&out.f.size()<=2,CN_QUAD,"unexpected face count, "+faces_info(out))) return false;
	if(!check(same_topology(ri,ro),CN_QUAD,"topology changed")) return false;
	return true;
}

bool case_reuse(){
	// first run: sphere, leaves state behind
	Mesh sph=make_sphere(1);
	load_into_simplify(sph);
	Simplify::simplify_mesh(20,7,false,Simplify::PreserveManifold);
	// second run: torus, must not see any stale adjacency
	Mesh tor=make_torus(16,8,2.0,0.8);
	load_into_simplify(tor);
	Simplify::simplify_mesh(60,7,false,Simplify::PreserveManifold);
	Mesh r1=read_back();
	// third run: same torus again, must give the identical result
	load_into_simplify(tor);
	Simplify::simplify_mesh(60,7,false,Simplify::PreserveManifold);
	Mesh r2=read_back();
	if(!check(r1.f.size()==r2.f.size(),CN_REUSE,"face count differs between identical runs")) return false;
	std::vector<std::array<int,3> > s1=r1.f,s2=r2.f;
	for(size_t i=0;i<s1.size();i++) std::sort(s1[i].begin(),s1[i].end());
	for(size_t i=0;i<s2.size();i++) std::sort(s2[i].begin(),s2[i].end());
	std::sort(s1.begin(),s1.end());
	std::sort(s2.begin(),s2.end());
	if(!check(s1==s2,CN_REUSE,"triangle sets differ between identical runs")) return false;
	Report ri=analyze(tor),ro=analyze(r1);
	if(!check(ro.ok,CN_REUSE,"output not manifold: "+ro.why)) return false;
	if(!check(same_topology(ri,ro),CN_REUSE,"topology changed")) return false;
	return true;
}

// ------------------------------------------------------------------
// rejected input cases
// ------------------------------------------------------------------

void poison_sentinels(){
	for(size_t i=0;i<Simplify::triangles.size();i++){
		Simplify::Triangle&t=Simplify::triangles[i];
		for(int j=0;j<4;j++) t.err[j]=100.0+(double)i*7.0+(double)j;
		t.attr=64; t.material=3; t.dirty=0; t.deleted=0;
	}
	for(size_t i=0;i<Simplify::vertices.size();i++){
		Simplify::Vertex&v=Simplify::vertices[i];
		v.border=5; v.tstart=(int)i+1; v.tcount=(int)i+2;
	}
}

// bit-exact comparison so that NaN sentinel data compares equal to itself
bool same_double(double a,double b){ return std::memcmp(&a,&b,sizeof(double))==0; }

bool mesh_data_unchanged(const std::vector<Simplify::Vertex>&sv,
                         const std::vector<Simplify::Triangle>&st){
	if(sv.size()!=Simplify::vertices.size()) return false;
	if(st.size()!=Simplify::triangles.size()) return false;
	for(size_t i=0;i<sv.size();i++){
		const Simplify::Vertex&a=sv[i];
		const Simplify::Vertex&b=Simplify::vertices[i];
		if(!same_double(a.p.x,b.p.x)||!same_double(a.p.y,b.p.y)||!same_double(a.p.z,b.p.z)) return false;
		if(a.border!=b.border||a.tstart!=b.tstart||a.tcount!=b.tcount) return false;
	}
	for(size_t i=0;i<st.size();i++){
		const Simplify::Triangle&a=st[i];
		const Simplify::Triangle&b=Simplify::triangles[i];
		for(int j=0;j<3;j++) if(a.v[j]!=b.v[j]) return false;
		for(int j=0;j<4;j++) if(!same_double(a.err[j],b.err[j])) return false;
		if(a.deleted!=b.deleted||a.dirty!=b.dirty||a.attr!=b.attr||a.material!=b.material) return false;
	}
	return true;
}

// one bad input through both public entry points
bool reject_case(const Mesh&m,const char*cn){
	load_into_simplify(m);
	poison_sentinels();
	std::vector<Simplify::Vertex> sv=Simplify::vertices;
	std::vector<Simplify::Triangle> st=Simplify::triangles;
	bool threw=false;
	try{
		Simplify::simplify_mesh(1,7,false,Simplify::PreserveManifold);
	}catch(const std::invalid_argument&){
		threw=true;
	}catch(...){
		return fail_msg(cn,"wrong exception type from simplify_mesh");
	}
	if(!check(threw,cn,"no std::invalid_argument from simplify_mesh")) return false;
	if(!check(mesh_data_unchanged(sv,st),cn,"public mesh data modified by simplify_mesh")) return false;

	load_into_simplify(m);
	poison_sentinels();
	sv=Simplify::vertices;
	st=Simplify::triangles;
	threw=false;
	try{
		Simplify::simplify_mesh_lossless(false,Simplify::PreserveManifold);
	}catch(const std::invalid_argument&){
		threw=true;
	}catch(...){
		return fail_msg(cn,"wrong exception type from simplify_mesh_lossless");
	}
	if(!check(threw,cn,"no std::invalid_argument from simplify_mesh_lossless")) return false;
	if(!check(mesh_data_unchanged(sv,st),cn,"public mesh data modified by simplify_mesh_lossless")) return false;
	return true;
}

const char*CN_R_RANGE="rejected-inputs/out-of-range-index";
const char*CN_R_NEG="rejected-inputs/negative-index";
const char*CN_R_DEGEN="rejected-inputs/degenerate-face";
const char*CN_R_DUP="rejected-inputs/duplicate-face";
const char*CN_R_RDUP="rejected-inputs/reversed-duplicate-face";
const char*CN_R_EDGE="rejected-inputs/non-manifold-edge";
const char*CN_R_FAN="rejected-inputs/disconnected-fan";
const char*CN_R_ORIENT="rejected-inputs/orientation-conflict";
const char*CN_R_NAN="rejected-inputs/non-finite-position";
const char*CN_R_STATE="rejected-inputs/state-survives-rejections";

bool case_reject_range(){
	Mesh m=make_tetra();
	m.f[0][2]=9; // vertices.size()==4
	return reject_case(m,CN_R_RANGE);
}

bool case_reject_negative(){
	Mesh m=make_tetra();
	m.f[0][1]=-1;
	return reject_case(m,CN_R_NEG);
}

bool case_reject_degenerate(){
	Mesh m=make_tetra();
	m.f[0][2]=m.f[0][1];
	return reject_case(m,CN_R_DEGEN);
}

bool case_reject_duplicate(){
	Mesh m=make_tetra();
	m.f.push_back(m.f[0]); // same orientation
	return reject_case(m,CN_R_DUP);
}

bool case_reject_reversed_duplicate(){
	Mesh m=make_tetra();
	std::array<int,3> d={{m.f[0][0],m.f[0][2],m.f[0][1]}};
	m.f.push_back(d); // same vertex set, reversed orientation
	return reject_case(m,CN_R_RDUP);
}

bool case_reject_nonmanifold_edge(){
	Mesh m;
	add_vert(m,0,0,0); add_vert(m,1,0,0); add_vert(m,0,1,0);
	add_vert(m,0,0,1); add_vert(m,1,1,1);
	add_face(m,0,1,2); add_face(m,0,1,3); add_face(m,0,1,4); // edge 0-1 used 3 times
	return reject_case(m,CN_R_EDGE);
}

bool case_reject_disconnected_fan(){
	Mesh m;
	add_vert(m,0,0,0); add_vert(m,1,0,0); add_vert(m,0,1,0);
	add_vert(m,2,0,0); add_vert(m,2,1,0);
	add_face(m,0,1,2); add_face(m,0,3,4); // two fans touching only at vertex 0
	return reject_case(m,CN_R_FAN);
}

bool case_reject_orientation(){
	Mesh m;
	add_vert(m,0,0,0); add_vert(m,1,0,0); add_vert(m,0,1,0); add_vert(m,0,0,1);
	add_face(m,0,1,2); add_face(m,0,1,3); // shared edge with same direction
	return reject_case(m,CN_R_ORIENT);
}

bool case_reject_nonfinite(){
	Mesh m=make_tetra();
	m.p[0]=vec3f(std::numeric_limits<double>::quiet_NaN(),0,0);
	return reject_case(m,CN_R_NAN);
}

bool case_state_survives(){
	// one rejection first ...
	Mesh bad;
	add_vert(bad,0,0,0); add_vert(bad,1,0,0); add_vert(bad,0,1,0); add_vert(bad,0,0,1);
	add_face(bad,0,1,2); add_face(bad,0,1,3);
	load_into_simplify(bad);
	bool threw=false;
	try{ Simplify::simplify_mesh(1,7,false,Simplify::PreserveManifold); }
	catch(const std::invalid_argument&){ threw=true; }
	if(!check(threw,CN_R_STATE,"setup rejection did not throw")) return false;
	// ... then a valid call must still work
	Mesh in=make_tetra();
	load_into_simplify(in);
	Simplify::simplify_mesh(1,7,false,Simplify::PreserveManifold);
	Mesh out=read_back();
	Report ro=analyze(out);
	if(!check(ro.ok,CN_R_STATE,"output not manifold: "+ro.why)) return false;
	if(!check(out.f.size()==4,CN_R_STATE,"tetrahedron not preserved, "+faces_info(out))) return false;
	return true;
}

// ------------------------------------------------------------------
// groups
// ------------------------------------------------------------------

typedef bool(*CaseFn)();
struct NamedCase { const char*name; CaseFn fn; };

int run_group(const char*group,const NamedCase*cases,size_t n){
	size_t passed=0;
	for(size_t i=0;i<n;i++) if(cases[i].fn()) passed++;
	if(passed==n&&n>0){
		std::printf("PASS %s cases=%zu\n",group,n);
		return 0;
	}
	std::printf("FAIL %s: %zu/%zu cases passed\n",group,passed,n);
	return 1;
}

} // namespace

int main(){
	int failed=0;
	{
		const NamedCase cases[]={
			{CN_TETRA,case_tetra},
			{CN_OCTA,case_octa},
			{CN_SPHERE,case_sphere},
			{CN_TORUS,case_torus},
			{CN_TWO,case_two_shells},
		};
		failed+=run_group("closed-shells",cases,sizeof(cases)/sizeof(cases[0]));
	}
	{
		const NamedCase cases[]={
			{CN_HOLES,case_two_hole_sheet},
			{CN_DISK,case_disk_sheet},
			{CN_ONE,case_single_triangle},
		};
		failed+=run_group("boundaries",cases,sizeof(cases)/sizeof(cases[0]));
	}
	{
		const NamedCase cases[]={
			{CN_COPLANAR,case_coplanar_lossless},
			{CN_EMPTY,case_empty_lossless},
			{CN_ISOLATED,case_isolated_lossless},
			{CN_QUAD,case_quad_stray_lossless},
			{CN_REUSE,case_reuse},
		};
		failed+=run_group("lossless-reuse",cases,sizeof(cases)/sizeof(cases[0]));
	}
	{
		const NamedCase cases[]={
			{CN_R_RANGE,case_reject_range},
			{CN_R_NEG,case_reject_negative},
			{CN_R_DEGEN,case_reject_degenerate},
			{CN_R_DUP,case_reject_duplicate},
			{CN_R_RDUP,case_reject_reversed_duplicate},
			{CN_R_EDGE,case_reject_nonmanifold_edge},
			{CN_R_FAN,case_reject_disconnected_fan},
			{CN_R_ORIENT,case_reject_orientation},
			{CN_R_NAN,case_reject_nonfinite},
			{CN_R_STATE,case_state_survives},
		};
		failed+=run_group("rejected-inputs",cases,sizeof(cases)/sizeof(cases[0]));
	}
	if(failed) return 1;
	std::printf("FQMS_TOPOLOGY_OK\n");
	return 0;
}
