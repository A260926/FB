// Regression tests for the capacity-constrained power diagram solver
// (src/capacity.hh / src/capacity.cc). Built and run by
// checks/check-capacity.sh. Prints CAPACITY_OK cases=N on success.

#include "capacity.hh"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace voro;

static int cases_done=0;

static void fail(const char *msg) {
	printf("CAPACITY_FAIL %s\n",msg);
	exit(1);
}

static void expect(bool cond,const char *msg) {
	if(!cond) fail(msg);
}

static bool close(double a,double b,double tol) {return std::fabs(a-b)<=tol;}

static bool relclose(double a,double b,double tol) {
	return std::fabs(a-b)<=tol*std::fabs(b);
}

static void case_done(const char *name) {
	cases_done++;
	printf("ok %d - %s\n",cases_done,name);
}

// ------------------------------------------------------------------
// Shared fixtures
// ------------------------------------------------------------------

// A deterministic asymmetric 3D configuration: six seeds in an
// asymmetric box, and a publicly specified weight vector (with mixed
// signs) used to generate target volumes.
static const double AX=0.1,BX=2.3,AY=-0.4,BY=1.6,AZ=0.7,BZ=3.1;
static const double BOXVOL=(BX-AX)*(BY-AY)*(BZ-AZ);

static void asym_setup(std::vector<int> &ids,std::vector<double> &pos,
		std::vector<double> &wstar) {
	int ii[6]={0,1,2,3,4,5};
	ids.assign(ii,ii+6);
	double pp[18]={
		0.42,0.13,1.02,
		1.87,0.44,0.91,
		0.71,1.31,2.43,
		1.53,-0.21,2.71,
		0.29,0.87,1.88,
		2.02,1.19,1.37};
	pos.assign(pp,pp+18);
	double ww[6]={0.35,-0.42,0.78,0.05,-0.15,0.61};
	wstar.assign(ww,ww+6);
}

static std::vector<double> normalized(std::vector<double> w) {
	double m=w[0];
	for(size_t i=1;i<w.size();i++) if(w[i]<m) m=w[i];
	for(size_t i=0;i<w.size();i++) w[i]-=m;
	return w;
}

static double min_weight(const std::vector<double> &w) {
	double m=w[0];
	for(size_t i=1;i<w.size();i++) if(w[i]<m) m=w[i];
	return m;
}

static const capacity_cell* find_cell(const capacity_geometry &g,int id) {
	for(size_t i=0;i<g.cells.size();i++) if(g.cells[i].id==id) return &g.cells[i];
	return 0;
}

static bool has_neighbor(const capacity_cell &c,int id) {
	for(size_t i=0;i<c.face_neighbors.size();i++) if(c.face_neighbors[i]==id) return true;
	return false;
}

static int count_particle_neighbor(const capacity_cell &c,int id) {
	int k=0;
	for(size_t i=0;i<c.face_neighbors.size();i++) if(c.face_neighbors[i]==id) k++;
	return k;
}

// Computes the volume and centroid of a cell from its exported vertices
// and ordered faces, by summing signed tetrahedra. Voro++ orders face
// vertices clockwise as seen from outside, so the signed volume is
// negative; the centroid formula is sign-independent.
static void face_derived(const capacity_cell &c,double &vol,double cen[3]) {
	double sv=0,sc[3]={0,0,0};
	for(size_t f=0;f<c.faces.size();f++) {
		const std::vector<int> &fv=c.faces[f];
		for(size_t k=1;k+1<fv.size();k++) {
			const double *a=&c.vertices[3*fv[0]];
			const double *b=&c.vertices[3*fv[k]];
			const double *d=&c.vertices[3*fv[k+1]];
			double tv=(a[0]*(b[1]*d[2]-b[2]*d[1])
					-a[1]*(b[0]*d[2]-b[2]*d[0])
					+a[2]*(b[0]*d[1]-b[1]*d[0]))/6.0;
			sv+=tv;
			for(int q=0;q<3;q++) sc[q]+=tv*(a[q]+b[q]+d[q])/4.0;
		}
	}
	vol=-sv;
	for(int q=0;q<3;q++) cen[q]=sc[q]/sv;
}

// ------------------------------------------------------------------
// 1. Two seeds on the x axis: the interface is a plane that can be
//    checked directly.
// ------------------------------------------------------------------
static void two_seeds_plane() {
	std::vector<int> ids;
	std::vector<double> pos,tgt;
	int ii[2]={0,1};ids.assign(ii,ii+2);
	double pp[6]={0.5,0.5,0.5, 1.5,0.5,0.5};pos.assign(pp,pp+6);
	double tt[2]={0.75,1.25};tgt.assign(tt,tt+2);

	capacity_result res;
	capacity_status st=capacity_solve(0,2,0,1,0,1,ids,pos,tgt,res);
	expect(st==capacity_converged,"two_seeds: not converged");
	expect(res.weights.size()==2,"two_seeds: weight count");
	// Plane at x=0.75 requires w0-w1=-0.5; normalized: {0,0.5}.
	expect(close(res.weights[0],0,1e-9),"two_seeds: weight 0");
	expect(close(res.weights[1],0.5,1e-9),"two_seeds: weight 1");
	expect(min_weight(res.weights)==0,"two_seeds: normalization");
	expect(close(res.geometry.cells[0].volume,0.75,1e-9),"two_seeds: volume 0");
	expect(close(res.geometry.cells[1].volume,1.25,1e-9),"two_seeds: volume 1");

	// Cell 0 is the box [0,0.75]x[0,1]x[0,1]: 8 vertices, 4 of them on
	// the cutting plane x=0.75, centroid (0.375,0.5,0.5).
	const capacity_cell &c0=res.geometry.cells[0];
	expect(c0.vertices.size()==24,"two_seeds: vertex count");
	int on_plane=0;
	for(size_t i=0;i<c0.vertices.size();i+=3) {
		double x=c0.vertices[i];
		expect(x>-1e-9&&x<0.75+1e-9,"two_seeds: vertex range");
		if(std::fabs(x-0.75)<1e-9) on_plane++;
	}
	expect(on_plane==4,"two_seeds: interface vertex count");
	expect(close(c0.centroid[0],0.375,1e-9),"two_seeds: centroid x");
	expect(close(c0.centroid[1],0.5,1e-9),"two_seeds: centroid y");
	expect(close(c0.centroid[2],0.5,1e-9),"two_seeds: centroid z");

	// Exactly one face borders the other seed; the other five faces are
	// box walls with negative IDs.
	expect(c0.faces.size()==6,"two_seeds: face count");
	expect(count_particle_neighbor(c0,1)==1,"two_seeds: interface face count");
	int walls=0;
	for(size_t i=0;i<c0.face_neighbors.size();i++) if(c0.face_neighbors[i]<0) walls++;
	expect(walls==5,"two_seeds: wall face count");

	// The power relation |x-p0|^2-w0 == |x-p1|^2-w1 must hold on the
	// interface vertices.
	for(size_t i=0;i<c0.vertices.size();i+=3) {
		double x=c0.vertices[i],y=c0.vertices[i+1],z=c0.vertices[i+2];
		if(std::fabs(x-0.75)>1e-9) continue;
		double d0=(x-0.5)*(x-0.5)+(y-0.5)*(y-0.5)+(z-0.5)*(z-0.5)-res.weights[0];
		double d1=(x-1.5)*(x-1.5)+(y-0.5)*(y-0.5)+(z-0.5)*(z-0.5)-res.weights[1];
		expect(close(d0,d1,1e-9),"two_seeds: power relation");
	}

	// Face area is 1 and the seeds are 1 apart, so the derivative
	// matrix is [[0.5,-0.5],[-0.5,0.5]].
	expect(res.geometry.derivative.size()==4,"two_seeds: derivative size");
	expect(close(res.geometry.derivative[0],0.5,1e-9),"two_seeds: d00");
	expect(close(res.geometry.derivative[1],-0.5,1e-9),"two_seeds: d01");
	expect(close(res.geometry.derivative[2],-0.5,1e-9),"two_seeds: d10");
	expect(close(res.geometry.derivative[3],0.5,1e-9),"two_seeds: d11");

	// The geometry-only entry point at the same weights must agree.
	capacity_geometry geo;
	capacity_status est=capacity_evaluate(0,2,0,1,0,1,ids,pos,res.weights,geo);
	expect(est==capacity_converged&&geo.ok,"two_seeds: evaluate status");
	expect(close(geo.cells[0].volume,0.75,1e-9),"two_seeds: evaluate volume 0");
	expect(close(geo.cells[1].volume,1.25,1e-9),"two_seeds: evaluate volume 1");
	case_done("two_seeds_plane");
}

// ------------------------------------------------------------------
// 2. Three seeds in a row with unequal slab volumes.
// ------------------------------------------------------------------
static void three_slabs() {
	std::vector<int> ids;
	std::vector<double> pos,tgt;
	int ii[3]={0,1,2};ids.assign(ii,ii+3);
	double pp[9]={0.5,0.5,0.5, 1.5,0.5,0.5, 2.5,0.5,0.5};pos.assign(pp,pp+9);
	double tt[3]={0.25,2.0,0.75};tgt.assign(tt,tt+3);

	capacity_result res;
	capacity_status st=capacity_solve(0,3,0,1,0,1,ids,pos,tgt,res);
	expect(st==capacity_converged,"slabs: not converged");
	// Planes at x=0.25 and x=2.25 require weights {0,1.5,1.0}.
	expect(close(res.weights[0],0,1e-6),"slabs: weight 0");
	expect(close(res.weights[1],1.5,1e-6),"slabs: weight 1");
	expect(close(res.weights[2],1.0,1e-6),"slabs: weight 2");
	expect(relclose(res.geometry.cells[0].volume,0.25,1e-6),"slabs: volume 0");
	expect(relclose(res.geometry.cells[1].volume,2.0,1e-6),"slabs: volume 1");
	expect(relclose(res.geometry.cells[2].volume,0.75,1e-6),"slabs: volume 2");

	// Check the two interface planes through the exported vertices.
	const capacity_cell &c0=res.geometry.cells[0];
	int p01=0;
	for(size_t i=0;i<c0.vertices.size();i+=3) {
		double x=c0.vertices[i];
		expect(x>-1e-9&&x<0.25+1e-9,"slabs: cell0 vertex range");
		if(std::fabs(x-0.25)<1e-9) p01++;
	}
	expect(p01==4,"slabs: interface 01 vertices");
	const capacity_cell &c2=res.geometry.cells[2];
	int p12=0;
	for(size_t i=0;i<c2.vertices.size();i+=3) {
		double x=c2.vertices[i];
		expect(x>2.25-1e-9&&x<3+1e-9,"slabs: cell2 vertex range");
		if(std::fabs(x-2.25)<1e-9) p12++;
	}
	expect(p12==4,"slabs: interface 12 vertices");
	// Slab cells never touch: 0 and 2 are not neighbors.
	expect(!has_neighbor(c0,2),"slabs: 0-2 not adjacent");
	expect(has_neighbor(c0,1)&&has_neighbor(c2,1),"slabs: adjacency");
	case_done("three_slabs");
}

// ------------------------------------------------------------------
// 3. Asymmetric 3D set: targets generated from publicly specified
//    weights, recovered from an equal-weight start.
// ------------------------------------------------------------------
static void asymmetric_roundtrip() {
	std::vector<int> ids;
	std::vector<double> pos,wstar;
	asym_setup(ids,pos,wstar);

	capacity_geometry gen;
	capacity_status est=capacity_evaluate(AX,BX,AY,BY,AZ,BZ,ids,pos,wstar,gen);
	expect(est==capacity_converged&&gen.ok,"roundtrip: generate");
	std::vector<double> tgt;
	double vsum=0;
	for(size_t i=0;i<gen.cells.size();i++) {tgt.push_back(gen.cells[i].volume);vsum+=gen.cells[i].volume;}
	expect(relclose(vsum,BOXVOL,1e-12),"roundtrip: partition of unity");

	capacity_result res;
	capacity_status st=capacity_solve(AX,BX,AY,BY,AZ,BZ,ids,pos,tgt,res);
	expect(st==capacity_converged,"roundtrip: not converged");
	std::vector<double> expect_w=normalized(wstar);
	for(size_t i=0;i<6;i++) {
		expect(close(res.weights[i],expect_w[i],1e-5),"roundtrip: weight recovery");
		expect(relclose(res.geometry.cells[i].volume,tgt[i],1e-6),"roundtrip: volume match");
	}
	expect(min_weight(res.weights)==0,"roundtrip: normalization");
	case_done("asymmetric_roundtrip");
}

// ------------------------------------------------------------------
// 4. A target that changes the adjacency structure: with equal weights
//    the Delaunay diagonal connects seeds 1-2; the target flips it to
//    0-3. The solve must rebuild adjacency from scratch.
// ------------------------------------------------------------------
static void adjacency_change() {
	std::vector<int> ids;
	std::vector<double> pos;
	int ii[4]={0,1,2,3};ids.assign(ii,ii+4);
	double pp[12]={1.0,1.0,0.5, 3.0,1.0,0.5, 1.0,3.0,0.5, 2.9,3.1,0.5};
	pos.assign(pp,pp+12);

	std::vector<double> w0(4,0.0);
	capacity_geometry g0;
	capacity_status est=capacity_evaluate(0,4,0,4,0,1,ids,pos,w0,g0);
	expect(est==capacity_converged&&g0.ok,"adjacency: equal-weight evaluate");
	const capacity_cell *e0=find_cell(g0,0),*e1=find_cell(g0,1);
	expect(e0&&e1,"adjacency: find cells");
	expect(has_neighbor(*e0,1)&&has_neighbor(*e0,2)&&!has_neighbor(*e0,3),
			"adjacency: initial diagonal 1-2 (a)");
	expect(has_neighbor(*e1,2),"adjacency: initial diagonal 1-2 (b)");

	// Targets from a weight vector with a large weight on seed 3.
	std::vector<double> ws;
	double ww[4]={0,0,0,3.0};ws.assign(ww,ww+4);
	capacity_geometry g1;
	est=capacity_evaluate(0,4,0,4,0,1,ids,pos,ws,g1);
	expect(est==capacity_converged&&g1.ok,"adjacency: target evaluate");
	std::vector<double> tgt;
	for(size_t i=0;i<g1.cells.size();i++) tgt.push_back(g1.cells[i].volume);

	capacity_result res;
	capacity_status st=capacity_solve(0,4,0,4,0,1,ids,pos,tgt,res);
	expect(st==capacity_converged,"adjacency: not converged");
	const capacity_cell *s0=find_cell(res.geometry,0),*s1=find_cell(res.geometry,1);
	expect(s0&&s1,"adjacency: find solved cells");
	expect(has_neighbor(*s0,3),"adjacency: flipped diagonal 0-3");
	expect(!has_neighbor(*s1,2),"adjacency: 1-2 no longer adjacent");
	for(size_t i=0;i<4;i++)
		expect(relclose(res.geometry.cells[i].volume,tgt[i],1e-6),"adjacency: volume match");
	case_done("adjacency_change");
}

// ------------------------------------------------------------------
// 5. Equal-weight start versus a valid unequal-weight start.
// ------------------------------------------------------------------
static void equal_vs_unequal_start() {
	std::vector<int> ids;
	std::vector<double> pos,wstar;
	asym_setup(ids,pos,wstar);
	capacity_geometry gen;
	capacity_evaluate(AX,BX,AY,BY,AZ,BZ,ids,pos,wstar,gen);
	std::vector<double> tgt;
	for(size_t i=0;i<gen.cells.size();i++) tgt.push_back(gen.cells[i].volume);

	// Unequal start with mixed signs; must be a valid (non-empty) state.
	std::vector<double> w0;
	double ww[6]={0.1,-0.3,0.9,0.2,-0.1,0.4};w0.assign(ww,ww+6);
	capacity_geometry chk;
	capacity_status est=capacity_evaluate(AX,BX,AY,BY,AZ,BZ,ids,pos,w0,chk);
	expect(est==capacity_converged&&chk.ok,"start: unequal start valid");

	capacity_result ra,rb;
	capacity_status sa=capacity_solve(AX,BX,AY,BY,AZ,BZ,ids,pos,tgt,ra);
	capacity_status sb=capacity_solve(AX,BX,AY,BY,AZ,BZ,ids,pos,tgt,rb,
			capacity_options(),w0);
	expect(sa==capacity_converged,"start: equal start converged");
	expect(sb==capacity_converged,"start: unequal start converged");
	for(size_t i=0;i<6;i++) {
		expect(close(ra.weights[i],rb.weights[i],1e-6),"start: weights agree");
		expect(relclose(ra.geometry.cells[i].volume,rb.geometry.cells[i].volume,3e-6),
				"start: volumes agree");
	}
	case_done("equal_vs_unequal_start");
}

// ------------------------------------------------------------------
// 6. Adding a constant to all weights changes nothing.
// ------------------------------------------------------------------
static void shift_invariance() {
	std::vector<int> ids;
	std::vector<double> pos,wstar;
	asym_setup(ids,pos,wstar);
	std::vector<double> wshift=wstar;
	for(size_t i=0;i<wshift.size();i++) wshift[i]+=7.5;

	capacity_geometry ga,gb;
	capacity_evaluate(AX,BX,AY,BY,AZ,BZ,ids,pos,wstar,ga);
	capacity_evaluate(AX,BX,AY,BY,AZ,BZ,ids,pos,wshift,gb);
	expect(ga.ok&&gb.ok,"shift: evaluate ok");
	for(size_t i=0;i<6;i++) {
		expect(relclose(ga.cells[i].volume,gb.cells[i].volume,1e-9),"shift: volumes");
		for(int q=0;q<3;q++)
			expect(close(ga.cells[i].centroid[q],gb.cells[i].centroid[q],1e-9),
					"shift: centroids");
	}
	for(size_t i=0;i<36;i++)
		expect(close(ga.derivative[i],gb.derivative[i],1e-9),"shift: derivative");

	// Solves started from a weight vector and its shifted copy agree.
	capacity_geometry gen;
	capacity_evaluate(AX,BX,AY,BY,AZ,BZ,ids,pos,wstar,gen);
	std::vector<double> tgt;
	for(size_t i=0;i<gen.cells.size();i++) tgt.push_back(gen.cells[i].volume);
	std::vector<double> w0;
	double ww[6]={0.1,-0.3,0.9,0.2,-0.1,0.4};w0.assign(ww,ww+6);
	std::vector<double> w0s=w0;
	for(size_t i=0;i<w0s.size();i++) w0s[i]+=3.25;
	capacity_result ra,rb;
	capacity_solve(AX,BX,AY,BY,AZ,BZ,ids,pos,tgt,ra,capacity_options(),w0);
	capacity_solve(AX,BX,AY,BY,AZ,BZ,ids,pos,tgt,rb,capacity_options(),w0s);
	expect(ra.status==capacity_converged&&rb.status==capacity_converged,"shift: solves");
	for(size_t i=0;i<6;i++)
		expect(close(ra.weights[i],rb.weights[i],1e-6),"shift: solved weights");
	case_done("shift_invariance");
}

// ------------------------------------------------------------------
// 7. Permuting the input order must give the same answer per ID, and
//    the output ordering must follow the input IDs.
// ------------------------------------------------------------------
static void permutation() {
	std::vector<int> ids0;
	std::vector<double> pos,wstar;
	asym_setup(ids0,pos,wstar);
	// Attach non-trivial IDs.
	std::vector<int> ids;
	int ii[6]={10,20,30,40,50,60};ids.assign(ii,ii+6);
	capacity_geometry gen;
	capacity_evaluate(AX,BX,AY,BY,AZ,BZ,ids,pos,wstar,gen);
	std::vector<double> tgt;
	for(size_t i=0;i<gen.cells.size();i++) tgt.push_back(gen.cells[i].volume);

	capacity_result ra;
	capacity_status sa=capacity_solve(AX,BX,AY,BY,AZ,BZ,ids,pos,tgt,ra);
	expect(sa==capacity_converged,"perm: base solve");

	// Reversed order of all four input arrays.
	std::vector<int> rids(6);
	std::vector<double> rpos(18),rtgt(6);
	for(size_t i=0;i<6;i++) {
		rids[i]=ids[5-i];
		rtgt[i]=tgt[5-i];
		for(int q=0;q<3;q++) rpos[3*i+q]=pos[3*(5-i)+q];
	}
	capacity_result rb;
	capacity_status sb=capacity_solve(AX,BX,AY,BY,AZ,BZ,rids,rpos,rtgt,rb);
	expect(sb==capacity_converged,"perm: permuted solve");
	// Output ordering follows the input ordering.
	for(size_t i=0;i<6;i++) {
		expect(ra.geometry.cells[i].id==ids[i],"perm: base order");
		expect(rb.geometry.cells[i].id==rids[i],"perm: permuted order");
	}
	// Same weights and volumes per ID.
	for(size_t i=0;i<6;i++) {
		int id=ids[i];
		expect(find_cell(ra.geometry,id)!=0&&find_cell(rb.geometry,id)!=0,
				"perm: find cell");
		size_t ja=0,jb=0;
		for(size_t k=0;k<6;k++) {
			if(ra.geometry.cells[k].id==id) ja=k;
			if(rb.geometry.cells[k].id==id) jb=k;
		}
		expect(close(ra.weights[ja],rb.weights[jb],1e-5),"perm: weights per ID");
		expect(relclose(ra.geometry.cells[ja].volume,rb.geometry.cells[jb].volume,1e-6),
				"perm: volumes per ID");
	}
	case_done("permutation");
}

// ------------------------------------------------------------------
// 8. Translation and uniform scaling: lengths scaled by s scale the
//    weights by s^2 and the volumes by s^3; translation leaves the
//    weights unchanged.
// ------------------------------------------------------------------
static void scale_translate() {
	std::vector<int> ids;
	std::vector<double> pos,wstar;
	asym_setup(ids,pos,wstar);
	capacity_geometry gen;
	capacity_evaluate(AX,BX,AY,BY,AZ,BZ,ids,pos,wstar,gen);
	std::vector<double> tgt;
	for(size_t i=0;i<gen.cells.size();i++) tgt.push_back(gen.cells[i].volume);
	capacity_result base;
	capacity_status sb=capacity_solve(AX,BX,AY,BY,AZ,BZ,ids,pos,tgt,base);
	expect(sb==capacity_converged,"scale: base solve");

	const double s=2.5;
	double t[3]={0.3,-1.2,2.0};
	std::vector<double> spos(18),stgt(6);
	for(size_t i=0;i<6;i++) {
		for(int q=0;q<3;q++) spos[3*i+q]=s*pos[3*i+q]+t[q];
		stgt[i]=s*s*s*tgt[i];
	}
	capacity_result sc;
	capacity_status ss=capacity_solve(s*AX+t[0],s*BX+t[0],s*AY+t[1],s*BY+t[1],
			s*AZ+t[2],s*BZ+t[2],ids,spos,stgt,sc);
	expect(ss==capacity_converged,"scale: scaled solve");
	for(size_t i=0;i<6;i++) {
		expect(close(sc.weights[i],s*s*base.weights[i],1e-4),"scale: weights scale as s^2");
		expect(relclose(sc.geometry.cells[i].volume,s*s*s*base.geometry.cells[i].volume,1e-5),
				"scale: volumes scale as s^3");
		for(int q=0;q<3;q++)
			expect(close(sc.geometry.cells[i].centroid[q],
						s*base.geometry.cells[i].centroid[q]+t[q],1e-4),
					"scale: centroids scale+translate");
	}

	// Pure translation: weights unchanged.
	std::vector<double> tpos(18);
	for(size_t i=0;i<6;i++) for(int q=0;q<3;q++) tpos[3*i+q]=pos[3*i+q]+t[q];
	capacity_result tr;
	capacity_status ts=capacity_solve(AX+t[0],BX+t[0],AY+t[1],BY+t[1],AZ+t[2],BZ+t[2],
			ids,tpos,tgt,tr);
	expect(ts==capacity_converged,"scale: translated solve");
	for(size_t i=0;i<6;i++) {
		expect(close(tr.weights[i],base.weights[i],1e-6),"scale: translation invariant weights");
		for(int q=0;q<3;q++)
			expect(close(tr.geometry.cells[i].centroid[q],
						base.geometry.cells[i].centroid[q]+t[q],1e-6),
					"scale: translated centroids");
	}
	case_done("scale_translate");
}

// ------------------------------------------------------------------
// 9. The derivative matrix: central differences, row/column sums,
//    symmetry, sign pattern, and no column for the box walls.
// ------------------------------------------------------------------
static void derivative_checks() {
	std::vector<int> ids;
	std::vector<double> pos,wstar;
	asym_setup(ids,pos,wstar);
	capacity_geometry g;
	capacity_evaluate(AX,BX,AY,BY,AZ,BZ,ids,pos,wstar,g);
	expect(g.ok,"deriv: evaluate");
	const int n=6;
	expect(g.derivative.size()==(size_t) n*n,"deriv: matrix size");

	// Central finite differences of the volumes.
	double h=1e-6;
	for(int j=0;j<n;j++) {
		std::vector<double> wp=wstar,wm=wstar;
		wp[j]+=h;wm[j]-=h;
		capacity_geometry gp,gm;
		capacity_evaluate(AX,BX,AY,BY,AZ,BZ,ids,pos,wp,gp);
		capacity_evaluate(AX,BX,AY,BY,AZ,BZ,ids,pos,wm,gm);
		expect(gp.ok&&gm.ok,"deriv: perturbed evaluates");
		for(int i=0;i<n;i++) {
			double fd=(gp.cells[i].volume-gm.cells[i].volume)/(2*h);
			expect(close(fd,g.derivative[i*n+j],1e-6),"deriv: central difference");
		}
	}
	// Rows sum to zero (uniform weight shift leaves geometry unchanged)
	// and columns sum to zero (total volume is conserved).
	for(int i=0;i<n;i++) {
		double rs=0;
		for(int j=0;j<n;j++) rs+=g.derivative[i*n+j];
		expect(std::fabs(rs)<1e-12,"deriv: row sum zero");
	}
	for(int j=0;j<n;j++) {
		double cs=0;
		for(int i=0;i<n;i++) cs+=g.derivative[i*n+j];
		expect(std::fabs(cs)<1e-10,"deriv: column sum zero");
	}
	// Symmetry and sign pattern: positive diagonal, non-positive
	// off-diagonal entries.
	for(int i=0;i<n;i++) {
		expect(g.derivative[i*n+i]>0,"deriv: positive diagonal");
		for(int j=0;j<n;j++) {
			if(i==j) continue;
			expect(g.derivative[i*n+j]<=0,"deriv: off-diagonal sign");
			expect(close(g.derivative[i*n+j],g.derivative[j*n+i],1e-10),"deriv: symmetry");
		}
	}
	case_done("derivative_checks");
}

// ------------------------------------------------------------------
// 10. Cross-check reported volumes and centroids against values
//     derived from the exported vertices and ordered faces.
// ------------------------------------------------------------------
static void centroid_face_crosscheck() {
	std::vector<int> ids;
	std::vector<double> pos,wstar;
	asym_setup(ids,pos,wstar);
	capacity_geometry g;
	capacity_evaluate(AX,BX,AY,BY,AZ,BZ,ids,pos,wstar,g);
	expect(g.ok,"cross: evaluate");
	double vsum=0;
	for(size_t i=0;i<g.cells.size();i++) {
		const capacity_cell &c=g.cells[i];
		expect(c.faces.size()==c.face_neighbors.size(),"cross: face/neighbor count");
		for(size_t f=0;f<c.faces.size();f++) {
			expect(c.faces[f].size()>=3,"cross: face order");
			for(size_t k=0;k<c.faces[f].size();k++)
				expect(c.faces[f][k]>=0&&
						(size_t) c.faces[f][k]<c.vertices.size()/3,"cross: vertex index range");
		}
		double vol,cen[3];
		face_derived(c,vol,cen);
		expect(relclose(vol,c.volume,1e-9),"cross: face-derived volume");
		for(int q=0;q<3;q++)
			expect(close(cen[q],c.centroid[q],1e-9),"cross: face-derived centroid");
		vsum+=c.volume;
	}
	expect(relclose(vsum,BOXVOL,1e-12),"cross: volumes tile the box");
	case_done("centroid_face_crosscheck");
}

// ------------------------------------------------------------------
// 11. Input validation: every malformed input must be rejected with
//     capacity_invalid_input before anything is computed, and the
//     caller's arrays must be left untouched.
// ------------------------------------------------------------------
static void rejection() {
	std::vector<int> ids;
	std::vector<double> pos,wstar;
	asym_setup(ids,pos,wstar);
	capacity_geometry gen;
	capacity_evaluate(AX,BX,AY,BY,AZ,BZ,ids,pos,wstar,gen);
	std::vector<double> tgt;
	for(size_t i=0;i<gen.cells.size();i++) tgt.push_back(gen.cells[i].volume);

	std::vector<int> ids0=ids;
	std::vector<double> pos0=pos,tgt0=tgt;
	capacity_result res;
	capacity_geometry geo;

	// Target sum mismatch (relative error ~1e-6 >> 1e-10).
	{
		std::vector<double> bad=tgt;bad[0]*=1+1e-6;
		expect(capacity_solve(AX,BX,AY,BY,AZ,BZ,ids,pos,bad,res)==capacity_invalid_input,
				"reject: target sum");
		expect(res.status==capacity_invalid_input,"reject: target sum status");
	}
	// Duplicate IDs.
	{
		std::vector<int> bad=ids;bad[1]=ids[0];
		expect(capacity_solve(AX,BX,AY,BY,AZ,BZ,bad,pos,tgt,res)==capacity_invalid_input,
				"reject: duplicate id");
	}
	// Duplicate positions.
	{
		std::vector<double> bad=pos;
		for(int q=0;q<3;q++) bad[3+q]=pos[q];
		expect(capacity_solve(AX,BX,AY,BY,AZ,BZ,ids,bad,tgt,res)==capacity_invalid_input,
				"reject: duplicate position");
	}
	// Non-finite inputs.
	{
		std::vector<double> bad=pos;bad[4]=NAN;
		expect(capacity_solve(AX,BX,AY,BY,AZ,BZ,ids,bad,tgt,res)==capacity_invalid_input,
				"reject: NaN position");
		std::vector<double> badt=tgt;badt[2]=INFINITY;
		expect(capacity_solve(AX,BX,AY,BY,AZ,BZ,ids,pos,badt,res)==capacity_invalid_input,
				"reject: inf target");
		std::vector<double> badw(6,0.0);badw[3]=NAN;
		expect(capacity_evaluate(AX,BX,AY,BY,AZ,BZ,ids,pos,badw,geo)==capacity_invalid_input,
				"reject: NaN weight");
		expect(capacity_solve(AX,BX,AY,BY,AZ,BZ,ids,pos,tgt,res,capacity_options(),badw)
				==capacity_invalid_input,"reject: NaN initial weight");
		expect(capacity_solve(AX,BX,AY,BY,AZ,NAN,ids,pos,tgt,res)==capacity_invalid_input,
				"reject: NaN box");
	}
	// Non-positive box dimensions.
	expect(capacity_solve(1,1,AY,BY,AZ,BZ,ids,pos,tgt,res)==capacity_invalid_input,
			"reject: degenerate box");
	expect(capacity_solve(BX,AX,AY,BY,AZ,BZ,ids,pos,tgt,res)==capacity_invalid_input,
			"reject: inverted box");
	// Seed count out of range.
	{
		std::vector<int> one_id(1,0);
		std::vector<double> one_pos(3,0.5),one_tgt(1,1.0);
		expect(capacity_solve(0,1,0,1,0,1,one_id,one_pos,one_tgt,res)==capacity_invalid_input,
				"reject: single seed");
		std::vector<int> many_ids(65);
		std::vector<double> many_pos(65*3),many_tgt(65);
		for(int i=0;i<65;i++) {
			many_ids[i]=i;
			many_pos[3*i]=0.1+(i%5)*0.2;
			many_pos[3*i+1]=0.1+((i/5)%5)*0.2;
			many_pos[3*i+2]=0.1+(i/25)*0.2;
			many_tgt[i]=(1.0*1.0*1.0)/65;
		}
		expect(capacity_solve(0,1,0,1,0,1,many_ids,many_pos,many_tgt,res)==capacity_invalid_input,
				"reject: too many seeds");
	}
	// Negative ID.
	{
		std::vector<int> bad=ids;bad[0]=-3;
		expect(capacity_solve(AX,BX,AY,BY,AZ,BZ,bad,pos,tgt,res)==capacity_invalid_input,
				"reject: negative id");
	}
	// Non-positive target.
	{
		std::vector<double> bad=tgt;bad[1]=0;
		expect(capacity_solve(AX,BX,AY,BY,AZ,BZ,ids,pos,bad,res)==capacity_invalid_input,
				"reject: zero target");
		bad[1]=-0.5;
		expect(capacity_solve(AX,BX,AY,BY,AZ,BZ,ids,pos,bad,res)==capacity_invalid_input,
				"reject: negative target");
	}
	// Seed outside or on the boundary of the box.
	{
		std::vector<double> bad=pos;bad[0]=AX;
		expect(capacity_solve(AX,BX,AY,BY,AZ,BZ,ids,bad,tgt,res)==capacity_invalid_input,
				"reject: seed on wall");
		bad[0]=AX-0.01;
		expect(capacity_solve(AX,BX,AY,BY,AZ,BZ,ids,bad,tgt,res)==capacity_invalid_input,
				"reject: seed outside");
	}
	// Malformed options and mismatched array sizes.
	{
		expect(capacity_solve(AX,BX,AY,BY,AZ,BZ,ids,pos,tgt,res,capacity_options(0,10))
				==capacity_invalid_input,"reject: zero tolerance");
		expect(capacity_solve(AX,BX,AY,BY,AZ,BZ,ids,pos,tgt,res,capacity_options(1e-6,-1))
				==capacity_invalid_input,"reject: negative max_iterations");
		std::vector<double> shortw(3,0.0);
		expect(capacity_solve(AX,BX,AY,BY,AZ,BZ,ids,pos,tgt,res,capacity_options(),shortw)
				==capacity_invalid_input,"reject: wrong initial weight count");
		std::vector<double> shortt(4,1.0);
		expect(capacity_solve(AX,BX,AY,BY,AZ,BZ,ids,pos,shortt,res)==capacity_invalid_input,
				"reject: wrong target count");
	}
	// The caller's arrays must be untouched by all of the above, and a
	// rejected call must not have computed any geometry.
	expect(ids==ids0&&pos==pos0&&tgt==tgt0,"reject: inputs modified");
	expect(res.total_geometry_rebuilds==0,"reject: no geometry built");
	case_done("rejection");
}

// ------------------------------------------------------------------
// 12. An initial state with an empty cell is reported, not solved.
// ------------------------------------------------------------------
static void empty_initial() {
	std::vector<int> ids;
	std::vector<double> pos,tgt;
	int ii[2]={0,1};ids.assign(ii,ii+2);
	double pp[6]={0.5,0.5,0.5, 1.5,0.5,0.5};pos.assign(pp,pp+6);
	double tt[2]={0.75,1.25};tgt.assign(tt,tt+2);
	// With w1 much smaller than w0, cell 1 vanishes entirely.
	std::vector<double> w0;
	double ww[2]={0,-100};w0.assign(ww,ww+2);

	capacity_geometry geo;
	capacity_status est=capacity_evaluate(0,2,0,1,0,1,ids,pos,w0,geo);
	expect(est==capacity_empty_initial,"empty: evaluate status");
	expect(!geo.ok,"empty: evaluate not ok");

	capacity_result res;
	capacity_status st=capacity_solve(0,2,0,1,0,1,ids,pos,tgt,res,capacity_options(),w0);
	expect(st==capacity_empty_initial,"empty: solve status");
	expect(res.status!=capacity_converged,"empty: not reported as converged");
	expect(res.history.empty(),"empty: no iterations recorded");
	expect(res.total_geometry_rebuilds==1,"empty: single rebuild");
	case_done("empty_initial");
}

// ------------------------------------------------------------------
// 13. Iteration limits: zero iterations returns the initial state; one
//     iteration on a nonlinear problem returns a complete
//     non-converged state; an already-satisfied target converges
//     immediately.
// ------------------------------------------------------------------
static void iteration_limits() {
	// Zero iteration limit on the slab problem.
	std::vector<int> ids;
	std::vector<double> pos,tgt;
	int ii[3]={0,1,2};ids.assign(ii,ii+3);
	double pp[9]={0.5,0.5,0.5, 1.5,0.5,0.5, 2.5,0.5,0.5};pos.assign(pp,pp+9);
	double tt[3]={0.25,2.0,0.75};tgt.assign(tt,tt+3);
	capacity_result res;
	capacity_status st=capacity_solve(0,3,0,1,0,1,ids,pos,tgt,res,capacity_options(1e-6,0));
	expect(st==capacity_max_iterations,"limits: zero-iteration status");
	expect(res.status!=capacity_converged,"limits: zero-iteration not converged");
	expect(res.history.empty(),"limits: zero-iteration history");
	expect(res.total_geometry_rebuilds==1,"limits: zero-iteration rebuilds");
	// The returned state is the equal-weight starting state.
	for(int i=0;i<3;i++) {
		expect(close(res.weights[i],0,0),"limits: zero-iteration weights");
		expect(close(res.geometry.cells[i].volume,1.0,1e-12),"limits: zero-iteration volumes");
	}

	// One iteration on the asymmetric problem cannot converge.
	std::vector<int> ids2;
	std::vector<double> pos2,wstar;
	asym_setup(ids2,pos2,wstar);
	capacity_geometry gen;
	capacity_evaluate(AX,BX,AY,BY,AZ,BZ,ids2,pos2,wstar,gen);
	std::vector<double> tgt2;
	for(size_t i=0;i<gen.cells.size();i++) tgt2.push_back(gen.cells[i].volume);
	capacity_result r1;
	capacity_status s1=capacity_solve(AX,BX,AY,BY,AZ,BZ,ids2,pos2,tgt2,r1,
			capacity_options(1e-6,1));
	expect(s1==capacity_max_iterations,"limits: one-iteration status");
	expect(r1.status!=capacity_converged,"limits: one-iteration not converged");
	expect(r1.history.size()==1,"limits: one-iteration history");
	expect(r1.history[0].max_residual>1e-6,"limits: one-iteration residual");
	// The returned state is complete and consistent: re-evaluating the
	// returned weights reproduces the returned volumes.
	capacity_geometry recheck;
	capacity_evaluate(AX,BX,AY,BY,AZ,BZ,ids2,pos2,r1.weights,recheck);
	expect(recheck.ok,"limits: one-iteration state valid");
	for(size_t i=0;i<6;i++)
		expect(close(recheck.cells[i].volume,r1.geometry.cells[i].volume,1e-9),
				"limits: one-iteration state consistent");

	// A target already satisfied by equal weights converges immediately.
	std::vector<double> tgt3(3,1.0);
	capacity_result r3;
	capacity_status s3=capacity_solve(0,3,0,1,0,1,ids,pos,tgt3,r3);
	expect(s3==capacity_converged,"limits: immediate convergence");
	expect(r3.history.empty(),"limits: immediate convergence history");
	case_done("iteration_limits");
}

// ------------------------------------------------------------------
// 14. Iteration records: monotone residuals, positive minimum volumes,
//     consistent rebuild counts, and a case that must shorten its
//     first step (recorded as extra geometry rebuilds).
// ------------------------------------------------------------------
static void history_records() {
	std::vector<int> ids;
	std::vector<double> pos,wstar;
	asym_setup(ids,pos,wstar);
	capacity_geometry gen;
	capacity_evaluate(AX,BX,AY,BY,AZ,BZ,ids,pos,wstar,gen);
	std::vector<double> tgt;
	for(size_t i=0;i<gen.cells.size();i++) tgt.push_back(gen.cells[i].volume);

	capacity_result res;
	capacity_status st=capacity_solve(AX,BX,AY,BY,AZ,BZ,ids,pos,tgt,res);
	expect(st==capacity_converged,"history: converged");
	expect(!res.history.empty(),"history: records exist");
	double prev=1e300;
	int rebuild_sum=0;
	for(size_t k=0;k<res.history.size();k++) {
		const capacity_iteration &h=res.history[k];
		expect(h.max_residual<prev,"history: monotone residuals");
		expect(h.min_cell_volume>0,"history: positive min volume");
		expect(h.geometry_rebuilds>=1,"history: rebuild count");
		rebuild_sum+=h.geometry_rebuilds;
		prev=h.max_residual;
	}
	expect(rebuild_sum+1==res.total_geometry_rebuilds,"history: rebuild total");
	expect(prev<=1e-6,"history: final residual within default tolerance");
	expect(min_weight(res.weights)==0,"history: weights normalized");

	// Extreme targets force the first step to be shortened; the record
	// must show more than one geometry rebuild for that iteration.
	std::vector<double> hard(6,0.05);
	hard[5]=BOXVOL-0.25;
	capacity_result rh;
	capacity_status sh=capacity_solve(AX,BX,AY,BY,AZ,BZ,ids,pos,hard,rh);
	expect(sh==capacity_converged,"history: hard case converged");
	bool saw_backtrack=false;
	for(size_t k=0;k<rh.history.size();k++)
		if(rh.history[k].geometry_rebuilds>1) saw_backtrack=true;
	expect(saw_backtrack,"history: shortened step recorded");
	for(size_t i=0;i<6;i++)
		expect(relclose(rh.geometry.cells[i].volume,hard[i],1e-6),"history: hard volumes");
	case_done("history_records");
}

int main() {
	two_seeds_plane();
	three_slabs();
	asymmetric_roundtrip();
	adjacency_change();
	equal_vs_unequal_start();
	shift_invariance();
	permutation();
	scale_translate();
	derivative_checks();
	centroid_face_crosscheck();
	rejection();
	empty_initial();
	iteration_limits();
	history_records();
	printf("CAPACITY_OK cases=%d\n",cases_done);
	return 0;
}
