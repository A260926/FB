// Voro++, a 3D cell-based Voronoi library
//
// Regression tests for the capacity-constrained power diagram solver in
// src/capacity.cc. Each case function groups a set of related assertions;
// the number of completed groups is reported as "CAPACITY_OK cases=N".

#include "voro++.hh"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

using namespace voro;

namespace {

void fail(const char *expr,const char *file,int line) {
	fprintf(stderr,"CAPACITY_FAIL %s:%d: CHECK(%s)\n",file,line,expr);
	exit(1);
}

#define CHECK(x) do { if(!(x)) fail(#x,__FILE__,__LINE__); } while(0)

bool close_to(double a,double b,double tol) {return fabs(a-b)<=tol;}

// The shared asymmetric six-seed problem in the unit cube. The public
// weights wstar generate the target volumes used by several cases.
void problem6(std::vector<int> &ids,std::vector<double> &pos,std::vector<double> &wstar) {
	double p[18]={0.12,0.31,0.47, 0.83,0.22,0.66, 0.45,0.71,0.18,
	              0.66,0.42,0.81, 0.28,0.88,0.55, 0.91,0.62,0.33};
	double w[6]={0.00,0.05,-0.03,0.08,0.02,-0.01};
	ids.clear();pos.assign(p,p+18);wstar.assign(w,w+6);
	for(int i=0;i<6;i++) ids.push_back(100+i);
}

// Computes the volume and centroid of an exported cell from its ordered
// faces alone, using the divergence theorem (signed tetrahedra from the
// origin). This is independent of the library's own routines.
void face_based_geometry(const capacity_cell &cl,double &vol,double c[3]) {
	double svol=0,sc[3]={0,0,0};
	for(size_t f=0;f<cl.faces.size();f++) {
		const std::vector<int> &F=cl.faces[f];
		for(size_t k=1;k+1<F.size();k++) {
			const double *a=&cl.vertices[3*F[0]],
			             *b=&cl.vertices[3*F[k]],
			             *cc=&cl.vertices[3*F[k+1]];
			double s=a[0]*(b[1]*cc[2]-b[2]*cc[1])
			        +a[1]*(b[2]*cc[0]-b[0]*cc[2])
			        +a[2]*(b[0]*cc[1]-b[1]*cc[0]);
			svol+=s;
			for(int d=0;d<3;d++) sc[d]+=(a[d]+b[d]+cc[d])*s;
		}
	}
	vol=svol/6;
	for(int d=0;d<3;d++) c[d]=sc[d]/(4*svol);
}

// True if the cell with index idx lists nid as one of its face neighbors.
bool cell_adjacent(const capacity_eval &ev,int idx,int nid) {
	const capacity_cell &cl=ev.cells[idx];
	for(size_t f=0;f<cl.face_neighbors.size();f++)
		if(cl.face_neighbors[f]==nid) return true;
	return false;
}

// Returns the index of the face of cl whose neighbor is nid, or -1.
int face_with_neighbor(const capacity_cell &cl,int nid) {
	for(size_t f=0;f<cl.face_neighbors.size();f++)
		if(cl.face_neighbors[f]==nid) return (int) f;
	return -1;
}

void check_matrix_row_sums(const capacity_eval &ev,double tol) {
	int n=(int) ev.ids.size();
	for(int i=0;i<n;i++) {
		double s=0;
		for(int j=0;j<n;j++) s+=ev.dvolume[(size_t) i*n+j];
		CHECK(fabs(s)<tol);
	}
}

// Case 1: two seeds on the x axis. The shared face is a plane whose
// position, the weights, and the 2x2 derivative matrix are all analytic.
void case_two_seeds() {
	std::vector<int> ids; ids.push_back(10); ids.push_back(20);
	double p[6]={0.3,0.5,0.5, 0.7,0.5,0.5};
	std::vector<double> pos(p,p+6);
	double t[2]={0.4,0.6};
	std::vector<double> tgt(t,t+2);

	capacity_options opt; opt.rel_tol=1e-10;
	capacity_result res;
	CHECK(capacity_solve(0,1,0,1,0,1,ids,pos,tgt,res,opt)==capacity_converged);
	CHECK(res.weights.size()==2);
	CHECK(close_to(res.weights[0],0.0,1e-9));
	CHECK(close_to(res.weights[1],0.08,1e-7));
	CHECK(close_to(res.volumes[0],0.4,1e-9));
	CHECK(close_to(res.volumes[1],0.6,1e-9));

	capacity_eval ev;
	CHECK(capacity_evaluate(0,1,0,1,0,1,ids,pos,res.weights,ev));
	CHECK(ev.cells.size()==2);

	// The first cell is the box [0,0.4]x[0,1]x[0,1]: 8 vertices, 6
	// faces, exactly one of them shared with seed 20 at x=0.4, the rest
	// on the walls with negative IDs
	const capacity_cell &c0=ev.cells[0];
	CHECK(c0.id==10);
	CHECK(c0.vertices.size()==24);
	CHECK(c0.faces.size()==6);
	CHECK(c0.face_neighbors.size()==6);
	int f01=face_with_neighbor(c0,20),walls=0;
	CHECK(f01>=0);
	for(size_t f=0;f<6;f++) if(c0.face_neighbors[f]<0) walls++;
	CHECK(walls==5);
	for(size_t k=0;k<c0.faces[f01].size();k++) {
		double x=c0.vertices[3*c0.faces[f01][k]];
		CHECK(close_to(x,0.4,1e-9));
	}
	CHECK(close_to(c0.centroid[0],0.2,1e-9));
	CHECK(close_to(c0.centroid[1],0.5,1e-9));
	CHECK(close_to(c0.centroid[2],0.5,1e-9));
	CHECK(close_to(ev.cells[1].centroid[0],0.7,1e-9));

	// Face area 1, seed distance 0.4: dV/dw = 1/(2*0.4) = 1.25
	CHECK(ev.dvolume.size()==4);
	CHECK(close_to(ev.dvolume[0],1.25,1e-9));
	CHECK(close_to(ev.dvolume[1],-1.25,1e-9));
	CHECK(close_to(ev.dvolume[2],-1.25,1e-9));
	CHECK(close_to(ev.dvolume[3],1.25,1e-9));
	check_matrix_row_sums(ev,1e-12);
}

// Case 2: three seeds on the x axis producing non-equal thin slabs with
// analytic plane positions, weights, and derivative matrix.
void case_three_slabs() {
	std::vector<int> ids; ids.push_back(1); ids.push_back(2); ids.push_back(3);
	double p[9]={0.2,0.5,0.5, 0.5,0.5,0.5, 0.8,0.5,0.5};
	std::vector<double> pos(p,p+9);
	double t[3]={0.25,0.45,0.30};
	std::vector<double> tgt(t,t+3);

	capacity_options opt; opt.rel_tol=1e-10;
	capacity_result res;
	CHECK(capacity_solve(0,1,0,1,0,1,ids,pos,tgt,res,opt)==capacity_converged);
	CHECK(close_to(res.weights[0],0.0,1e-9));
	CHECK(close_to(res.weights[1],0.06,1e-7));
	CHECK(close_to(res.weights[2],0.03,1e-7));
	for(int i=0;i<3;i++) CHECK(close_to(res.volumes[i],tgt[i],1e-9));

	capacity_eval ev;
	CHECK(capacity_evaluate(0,1,0,1,0,1,ids,pos,res.weights,ev));
	// The 0-1 interface is the plane x=0.25, the 1-2 interface x=0.70
	int f01=face_with_neighbor(ev.cells[0],2);
	int f12=face_with_neighbor(ev.cells[1],3);
	CHECK(f01>=0&&f12>=0);
	for(size_t k=0;k<ev.cells[0].faces[f01].size();k++)
		CHECK(close_to(ev.cells[0].vertices[3*ev.cells[0].faces[f01][k]],0.25,1e-9));
	for(size_t k=0;k<ev.cells[1].faces[f12].size();k++)
		CHECK(close_to(ev.cells[1].vertices[3*ev.cells[1].faces[f12][k]],0.70,1e-9));

	// Both shared faces have area 1 and seed distance 0.3
	double c=1/(2*0.3);
	CHECK(ev.dvolume.size()==9);
	double expect[9]={c,-c,0, -c,2*c,-c, 0,-c,c};
	for(int k=0;k<9;k++) CHECK(close_to(ev.dvolume[k],expect[k],1e-9));
	check_matrix_row_sums(ev,1e-12);

	// Every accepted state must improve the residual and keep all
	// volumes positive; geometry rebuilds never go backwards
	CHECK(!res.history.empty());
	CHECK(res.history[0].iteration==0);
	for(size_t k=1;k<res.history.size();k++) {
		CHECK(res.history[k].max_rel_error<res.history[k-1].max_rel_error);
		CHECK(res.history[k].min_volume>0);
		CHECK(res.history[k].geometry_builds>=res.history[k-1].geometry_builds);
	}
}

// Case 3: a fixed asymmetric 3D point set. Target volumes are generated
// from publicly specified weights, then recovered from other starts.
void case_asymmetric_3d() {
	std::vector<int> ids;
	std::vector<double> pos,wstar;
	problem6(ids,pos,wstar);

	capacity_eval ev0;
	CHECK(capacity_evaluate(0,1,0,1,0,1,ids,pos,wstar,ev0));
	std::vector<double> tgt=ev0.volumes;

	// The normalized public weights are the unique solution
	double wn[6];
	for(int i=0;i<6;i++) wn[i]=wstar[i]+0.03;

	capacity_options opt; opt.rel_tol=1e-9;
	capacity_result res;
	CHECK(capacity_solve(0,1,0,1,0,1,ids,pos,tgt,res,opt)==capacity_converged);
	for(int i=0;i<6;i++) {
		CHECK(close_to(res.volumes[i],tgt[i],1e-8*tgt[i]));
		CHECK(close_to(res.weights[i],wn[i],1e-5));
	}

	// A valid non-equal starting point must lead to the same answer
	double s[6]={0.2,-0.1,0.15,-0.05,0.1,0.0};
	std::vector<double> start(s,s+6);
	capacity_result res2;
	CHECK(capacity_solve(0,1,0,1,0,1,ids,pos,tgt,res2,opt,&start)==capacity_converged);
	for(int i=0;i<6;i++) CHECK(close_to(res2.weights[i],wn[i],1e-5));
}

// Case 4: a target that changes the adjacency structure. With equal
// weights seeds 0 and 3 share a face and seeds 1 and 2 do not; at the
// solution this is reversed. The solver must rebuild adjacency from
// scratch at every accepted state.
void case_adjacency_change() {
	std::vector<int> ids;
	for(int i=0;i<4;i++) ids.push_back(i);
	double p[12]={0.2,0.5,0.5, 0.4,0.2,0.5, 0.4,0.8,0.5, 0.8,0.5,0.5};
	std::vector<double> pos(p,p+12);
	double t[4]={0.10,0.35,0.35,0.20};
	std::vector<double> tgt(t,t+4);

	std::vector<double> weq(4,0.0);
	capacity_eval ev0;
	CHECK(capacity_evaluate(0,1,0,1,0,1,ids,pos,weq,ev0));
	CHECK(cell_adjacent(ev0,0,3));
	CHECK(!cell_adjacent(ev0,1,2));

	capacity_options opt; opt.rel_tol=1e-9;
	capacity_result res;
	CHECK(capacity_solve(0,1,0,1,0,1,ids,pos,tgt,res,opt)==capacity_converged);
	for(int i=0;i<4;i++) CHECK(close_to(res.volumes[i],tgt[i],1e-8*tgt[i]));

	capacity_eval ev1;
	CHECK(capacity_evaluate(0,1,0,1,0,1,ids,pos,res.weights,ev1));
	CHECK(!cell_adjacent(ev1,0,3));
	CHECK(cell_adjacent(ev1,1,2));
}

// Case 5: the same problem solved from equal weights and from a valid
// non-equal start must converge to the same weights.
void case_equal_vs_unequal_start() {
	std::vector<int> ids;
	std::vector<double> pos,wstar;
	problem6(ids,pos,wstar);
	capacity_eval ev0;
	CHECK(capacity_evaluate(0,1,0,1,0,1,ids,pos,wstar,ev0));

	capacity_options opt; opt.rel_tol=1e-9;
	capacity_result ra,rb;
	CHECK(capacity_solve(0,1,0,1,0,1,ids,pos,ev0.volumes,ra,opt)==capacity_converged);
	double s[6]={0.2,-0.1,0.15,-0.05,0.1,0.0};
	std::vector<double> start(s,s+6);
	CHECK(capacity_solve(0,1,0,1,0,1,ids,pos,ev0.volumes,rb,opt,&start)==capacity_converged);
	for(int i=0;i<6;i++) CHECK(close_to(ra.weights[i],rb.weights[i],1e-6));
}

// Case 6: adding a constant to all weights leaves the geometry and the
// derivative matrix unchanged, and a shifted start solves identically.
void case_weight_shift_invariance() {
	std::vector<int> ids;
	std::vector<double> pos,wstar;
	problem6(ids,pos,wstar);

	std::vector<double> wshift(wstar);
	for(size_t i=0;i<wshift.size();i++) wshift[i]+=7.5;
	capacity_eval ea,eb;
	CHECK(capacity_evaluate(0,1,0,1,0,1,ids,pos,wstar,ea));
	CHECK(capacity_evaluate(0,1,0,1,0,1,ids,pos,wshift,eb));
	for(size_t i=0;i<ea.volumes.size();i++)
		CHECK(close_to(ea.volumes[i],eb.volumes[i],1e-12));
	for(size_t i=0;i<ea.centroids.size();i++)
		CHECK(close_to(ea.centroids[i],eb.centroids[i],1e-12));
	for(size_t i=0;i<ea.dvolume.size();i++)
		CHECK(close_to(ea.dvolume[i],eb.dvolume[i],1e-12));
	check_matrix_row_sums(ea,1e-12);

	// Solving from a start shifted by a constant gives the same
	// normalized weights as the unshifted start
	double s[6]={0.2,-0.1,0.15,-0.05,0.1,0.0};
	std::vector<double> sa(s,s+6),sb(s,s+6);
	for(size_t i=0;i<sb.size();i++) sb[i]+=7.5;
	capacity_options opt; opt.rel_tol=1e-9;
	capacity_result ra,rb;
	CHECK(capacity_solve(0,1,0,1,0,1,ids,pos,ea.volumes,ra,opt,&sa)==capacity_converged);
	CHECK(capacity_solve(0,1,0,1,0,1,ids,pos,ea.volumes,rb,opt,&sb)==capacity_converged);
	for(int i=0;i<6;i++) CHECK(close_to(ra.weights[i],rb.weights[i],1e-12));
}

// Case 7: reordering the input arrays must reorder the outputs
// correspondingly, leaving the per-seed answer unchanged.
void case_permutation() {
	std::vector<int> ids;
	std::vector<double> pos,wstar;
	problem6(ids,pos,wstar);
	capacity_eval ev0;
	CHECK(capacity_evaluate(0,1,0,1,0,1,ids,pos,wstar,ev0));

	int perm[6]={2,4,0,5,1,3};
	std::vector<int> pids(6);
	std::vector<double> ppos(18),ptgt(6);
	for(int k=0;k<6;k++) {
		pids[k]=ids[perm[k]];
		ptgt[k]=ev0.volumes[perm[k]];
		for(int d=0;d<3;d++) ppos[3*k+d]=pos[3*perm[k]+d];
	}

	capacity_options opt; opt.rel_tol=1e-9;
	capacity_result rp;
	CHECK(capacity_solve(0,1,0,1,0,1,pids,ppos,ptgt,rp,opt)==capacity_converged);
	capacity_result rr;
	CHECK(capacity_solve(0,1,0,1,0,1,ids,pos,ev0.volumes,rr,opt)==capacity_converged);
	for(int k=0;k<6;k++) {
		CHECK(close_to(rp.weights[k],rr.weights[perm[k]],1e-6));
		CHECK(close_to(rp.volumes[k],rr.volumes[perm[k]],1e-8));
		CHECK(rp.cells[k].id==ids[perm[k]]);
	}
}

// Case 8: translating and uniformly scaling the whole problem must scale
// the weights by s^2 and the volumes by s^3.
void case_scaling_translation() {
	std::vector<int> ids;
	std::vector<double> pos,wstar;
	problem6(ids,pos,wstar);
	capacity_eval ev0;
	CHECK(capacity_evaluate(0,1,0,1,0,1,ids,pos,wstar,ev0));
	std::vector<double> tgt=ev0.volumes;

	capacity_options opt; opt.rel_tol=1e-9;
	capacity_result rr;
	CHECK(capacity_solve(0,1,0,1,0,1,ids,pos,tgt,rr,opt)==capacity_converged);

	double s=2.5,tr[3]={0.3,-0.7,1.1};
	std::vector<double> spos(18),stgt(6);
	for(int i=0;i<6;i++) {
		for(int d=0;d<3;d++) spos[3*i+d]=s*pos[3*i+d]+tr[d];
		stgt[i]=s*s*s*tgt[i];
	}
	double sax=0.3,sbx=0.3+2.5,say=-0.7,sby=-0.7+2.5,saz=1.1,sbz=1.1+2.5;
	capacity_result rs;
	CHECK(capacity_solve(sax,sbx,say,sby,saz,sbz,ids,spos,stgt,rs,opt)==capacity_converged);
	for(int i=0;i<6;i++) {
		CHECK(close_to(rs.weights[i],s*s*rr.weights[i],1e-5));
		CHECK(close_to(rs.volumes[i],stgt[i],1e-8*stgt[i]));
	}

	// The scaling law also holds for a pure geometry evaluation at the
	// scaled weights, without any solve
	std::vector<double> sw(6);
	for(int i=0;i<6;i++) sw[i]=s*s*rr.weights[i];
	capacity_eval evs;
	CHECK(capacity_evaluate(sax,sbx,say,sby,saz,sbz,ids,spos,sw,evs));
	for(int i=0;i<6;i++) CHECK(close_to(evs.volumes[i],s*s*s*rr.volumes[i],1e-9));

	// A pure translation leaves the weights unchanged
	std::vector<double> tpos(18);
	for(int i=0;i<6;i++) for(int d=0;d<3;d++) tpos[3*i+d]=pos[3*i+d]+tr[d];
	capacity_result rt;
	CHECK(capacity_solve(tr[0],tr[0]+1,tr[1],tr[1]+1,tr[2],tr[2]+1,ids,tpos,tgt,rt,opt)==capacity_converged);
	for(int i=0;i<6;i++) CHECK(close_to(rt.weights[i],rr.weights[i],1e-6));
}

// Case 9: the analytic derivative matrix must match central differences
// of the volumes produced by the same evaluate entry point.
void case_derivative_central_difference() {
	std::vector<int> ids;
	std::vector<double> pos,wstar;
	problem6(ids,pos,wstar);

	capacity_eval ev;
	CHECK(capacity_evaluate(0,1,0,1,0,1,ids,pos,wstar,ev));
	check_matrix_row_sums(ev,1e-12);

	double h=1e-6;
	for(int j=0;j<6;j++) {
		std::vector<double> wp(wstar),wm(wstar);
		wp[j]+=h;wm[j]-=h;
		capacity_eval ep,em;
		CHECK(capacity_evaluate(0,1,0,1,0,1,ids,pos,wp,ep));
		CHECK(capacity_evaluate(0,1,0,1,0,1,ids,pos,wm,em));
		for(int i=0;i<6;i++) {
			double fd=(ep.volumes[i]-em.volumes[i])/(2*h);
			CHECK(close_to(fd,ev.dvolume[(size_t) i*6+j],5e-6));
		}
	}
}

// Case 10: volumes and centroids recomputed from the exported ordered
// faces must agree with the reported values, and the volumes must
// partition the box.
void case_centroid_face_volume_crosscheck() {
	std::vector<int> ids;
	std::vector<double> pos,wstar;
	problem6(ids,pos,wstar);
	capacity_eval ev0;
	CHECK(capacity_evaluate(0,1,0,1,0,1,ids,pos,wstar,ev0));

	capacity_options opt; opt.rel_tol=1e-9;
	capacity_result res;
	CHECK(capacity_solve(0,1,0,1,0,1,ids,pos,ev0.volumes,res,opt)==capacity_converged);
	CHECK(res.cells.size()==6);

	double vsum=0;
	for(size_t i=0;i<res.cells.size();i++) {
		const capacity_cell &cl=res.cells[i];
		CHECK(cl.id==ids[i]);
		CHECK(!cl.vertices.empty());
		CHECK(cl.faces.size()==cl.face_neighbors.size());
		double vol,c[3];
		face_based_geometry(cl,vol,c);
		CHECK(close_to(fabs(vol),cl.volume,1e-9*cl.volume));
		for(int d=0;d<3;d++) CHECK(close_to(c[d],cl.centroid[d],1e-9));
		vsum+=cl.volume;
	}
	CHECK(close_to(vsum,1.0,1e-12));
}

// Case 11: invalid problem descriptions must be rejected before any work
// is done, and a valid solve must never modify the caller's arrays.
void case_invalid_inputs() {
	std::vector<int> ids;
	std::vector<double> pos,wstar;
	problem6(ids,pos,wstar);
	capacity_eval ev0;
	CHECK(capacity_evaluate(0,1,0,1,0,1,ids,pos,wstar,ev0));
	std::vector<double> tgt=ev0.volumes;
	capacity_options opt;
	capacity_result res;
	capacity_eval ev;

	// Target sum mismatch beyond the 1e-10 relative bound
	std::vector<double> bad(tgt); bad[0]+=1e-9;
	CHECK(capacity_solve(0,1,0,1,0,1,ids,pos,bad,res,opt)==capacity_invalid_input);
	// A mismatch within the bound is accepted and the targets are used
	// as given, not renormalized
	std::vector<double> ok(tgt); ok[0]+=5e-11;
	CHECK(capacity_solve(0,1,0,1,0,1,ids,pos,ok,res,opt)!=capacity_invalid_input);
	CHECK(close_to(ok[0],tgt[0]+5e-11,0.0));

	// Duplicate IDs
	{
		std::vector<int> b(ids); b[1]=b[0];
		CHECK(capacity_solve(0,1,0,1,0,1,b,pos,tgt,res,opt)==capacity_invalid_input);
		CHECK(!capacity_evaluate(0,1,0,1,0,1,b,pos,wstar,ev));
	}
	// Negative ID
	{
		std::vector<int> b(ids); b[0]=-1;
		CHECK(capacity_solve(0,1,0,1,0,1,b,pos,tgt,res,opt)==capacity_invalid_input);
	}
	// Duplicate positions
	{
		std::vector<double> b(pos);
		for(int d=0;d<3;d++) b[3+d]=b[d];
		CHECK(capacity_solve(0,1,0,1,0,1,ids,b,tgt,res,opt)==capacity_invalid_input);
	}
	// Non-finite inputs
	{
		double nan=std::numeric_limits<double>::quiet_NaN();
		double inf=std::numeric_limits<double>::infinity();
		std::vector<double> b(pos); b[4]=nan;
		CHECK(capacity_solve(0,1,0,1,0,1,ids,b,tgt,res,opt)==capacity_invalid_input);
		std::vector<double> bt(tgt); bt[2]=inf;
		CHECK(capacity_solve(0,1,0,1,0,1,ids,pos,bt,res,opt)==capacity_invalid_input);
		std::vector<double> bw(wstar); bw[1]=nan;
		CHECK(!capacity_evaluate(0,1,0,1,0,1,ids,pos,bw,ev));
		CHECK(capacity_solve(0,1,0,1,0,1,ids,pos,tgt,res,opt,&bw)==capacity_invalid_input);
	}
	// Non-positive box dimensions
	CHECK(capacity_solve(0.5,0.5,0,1,0,1,ids,pos,tgt,res,opt)==capacity_invalid_input);
	CHECK(capacity_solve(1,0,0,1,0,1,ids,pos,tgt,res,opt)==capacity_invalid_input);
	// Position outside the box
	{
		std::vector<double> b(pos); b[0]=1.2;
		CHECK(capacity_solve(0,1,0,1,0,1,ids,b,tgt,res,opt)==capacity_invalid_input);
	}
	// Too few or too many seeds
	{
		std::vector<int> i1(1,7);
		std::vector<double> p1(3,0.5),t1(1,1.0);
		CHECK(capacity_solve(0,1,0,1,0,1,i1,p1,t1,res,opt)==capacity_invalid_input);
		std::vector<int> i65(65);
		std::vector<double> p65(3*65),t65(65,1.0/65.0);
		for(int k=0;k<65;k++) {
			i65[k]=k;
			p65[3*k]=0.1+0.8*(k%5)/4.0;
			p65[3*k+1]=0.1+0.8*((k/5)%4)/3.0;
			p65[3*k+2]=0.1+0.8*(k/20)/3.0;
		}
		CHECK(capacity_solve(0,1,0,1,0,1,i65,p65,t65,res,opt)==capacity_invalid_input);
	}
	// Non-positive target volume
	{
		std::vector<double> b(tgt); b[3]=0.0;
		CHECK(capacity_solve(0,1,0,1,0,1,ids,pos,b,res,opt)==capacity_invalid_input);
		b[3]=-0.1;
		CHECK(capacity_solve(0,1,0,1,0,1,ids,pos,b,res,opt)==capacity_invalid_input);
	}
	// Malformed solver options
	{
		capacity_options bo;
		bo.rel_tol=0;
		CHECK(capacity_solve(0,1,0,1,0,1,ids,pos,tgt,res,bo)==capacity_invalid_input);
		bo.rel_tol=-1;
		CHECK(capacity_solve(0,1,0,1,0,1,ids,pos,tgt,res,bo)==capacity_invalid_input);
		bo.rel_tol=1e-6;bo.max_iterations=-2;
		CHECK(capacity_solve(0,1,0,1,0,1,ids,pos,tgt,res,bo)==capacity_invalid_input);
	}
	// Wrongly sized arrays
	{
		std::vector<double> b(tgt); b.pop_back();
		CHECK(capacity_solve(0,1,0,1,0,1,ids,pos,b,res,opt)==capacity_invalid_input);
		std::vector<double> w5(5,0.0);
		CHECK(capacity_solve(0,1,0,1,0,1,ids,pos,tgt,res,opt,&w5)==capacity_invalid_input);
		CHECK(!capacity_evaluate(0,1,0,1,0,1,ids,pos,w5,ev));
	}
	// The documented defaults
	{
		capacity_options def;
		CHECK(def.rel_tol==1e-6);
		CHECK(def.max_iterations==80);
	}

	// A valid solve must not modify the caller's arrays
	std::vector<int> ids_copy(ids);
	std::vector<double> pos_copy(pos),tgt_copy(tgt),iw(wstar);
	std::vector<double> iw_copy(iw);
	CHECK(capacity_solve(0,1,0,1,0,1,ids,pos,tgt,res,opt,&iw)==capacity_converged);
	CHECK(ids==ids_copy&&pos==pos_copy&&tgt==tgt_copy&&iw==iw_copy);
}

// Case 12: an initial state with an empty cell must be reported as
// empty_initial, and evaluate must fail on it.
void case_empty_initial() {
	std::vector<int> ids; ids.push_back(0); ids.push_back(1);
	double p[6]={0.3,0.5,0.5, 0.7,0.5,0.5};
	std::vector<double> pos(p,p+6);
	double t[2]={0.5,0.5};
	std::vector<double> tgt(t,t+2);
	double w[2]={100.0,0.0};
	std::vector<double> iw(w,w+2);

	capacity_options opt;
	capacity_result res;
	CHECK(capacity_solve(0,1,0,1,0,1,ids,pos,tgt,res,opt,&iw)==capacity_empty_initial);
	CHECK(res.history.empty());
	CHECK(res.cells.empty());
	CHECK(res.iterations==0);

	capacity_eval ev;
	CHECK(!capacity_evaluate(0,1,0,1,0,1,ids,pos,iw,ev));

	// A six-seed start that empties one cell
	std::vector<int> ids6;
	std::vector<double> pos6,wstar;
	problem6(ids6,pos6,wstar);
	capacity_eval ev6;
	CHECK(capacity_evaluate(0,1,0,1,0,1,ids6,pos6,wstar,ev6));
	double b[6]={0.5,-0.4,0.3,-0.2,0.1,0.0};
	std::vector<double> bad(b,b+6);
	capacity_result res6;
	CHECK(capacity_solve(0,1,0,1,0,1,ids6,pos6,ev6.volumes,res6,opt,&bad)==capacity_empty_initial);
}

// Case 13: iteration limits, step rejection, and the no-progress status
// must return the last accepted complete state without reporting
// convergence.
void case_iteration_limits() {
	std::vector<int> ids;
	std::vector<double> pos,wstar;
	problem6(ids,pos,wstar);
	capacity_eval ev0;
	CHECK(capacity_evaluate(0,1,0,1,0,1,ids,pos,wstar,ev0));

	// A zero iteration limit returns the evaluated initial state
	capacity_options o0; o0.max_iterations=0;
	capacity_result r0;
	CHECK(capacity_solve(0,1,0,1,0,1,ids,pos,ev0.volumes,r0,o0)==capacity_max_iterations);
	CHECK(r0.iterations==0);
	CHECK(r0.history.size()==1);
	CHECK(r0.cells.size()==6);
	std::vector<double> weq(6,0.0);
	capacity_eval eveq;
	CHECK(capacity_evaluate(0,1,0,1,0,1,ids,pos,weq,eveq));
	for(int i=0;i<6;i++) CHECK(close_to(r0.volumes[i],eveq.volumes[i],1e-14));

	// A one iteration limit returns the last accepted state, which is
	// complete but not converged
	capacity_options o1; o1.rel_tol=1e-9; o1.max_iterations=1;
	capacity_result r1;
	CHECK(capacity_solve(0,1,0,1,0,1,ids,pos,ev0.volumes,r1,o1)==capacity_max_iterations);
	CHECK(r1.iterations==1);
	CHECK(r1.history.size()==2);
	CHECK(r1.cells.size()==6);
	CHECK(r1.max_rel_error>o1.rel_tol);
	double vsum=0;
	for(int i=0;i<6;i++) vsum+=r1.volumes[i];
	CHECK(close_to(vsum,1.0,1e-12));

	// An unreachable tolerance must not be reported as converged; the
	// returned state is still complete
	capacity_options ot; ot.rel_tol=1e-300;
	capacity_result rt;
	capacity_status st=capacity_solve(0,1,0,1,0,1,ids,pos,ev0.volumes,rt,ot);
	CHECK(st!=capacity_converged);
	CHECK(st==capacity_no_progress||st==capacity_max_iterations);
	CHECK(rt.cells.size()==6);
	CHECK(!rt.history.empty());
	vsum=0;
	for(int i=0;i<6;i++) vsum+=rt.volumes[i];
	CHECK(close_to(vsum,1.0,1e-12));

	// A far-away start forces rejected trial steps: the solve still
	// converges, and the rebuild count shows the extra trials
	std::vector<int> ids4;
	for(int i=0;i<4;i++) ids4.push_back(i);
	double p4[12]={0.2,0.5,0.5, 0.4,0.2,0.5, 0.4,0.8,0.5, 0.8,0.5,0.5};
	std::vector<double> pos4(p4,p4+12);
	double t4[4]={0.02,0.47,0.47,0.04};
	std::vector<double> tgt4(t4,t4+4);
	double s4[4]={0.0,0.0,0.0,0.4};
	std::vector<double> start4(s4,s4+4);
	capacity_options o4; o4.rel_tol=1e-9;
	capacity_result r4;
	CHECK(capacity_solve(0,1,0,1,0,1,ids4,pos4,tgt4,r4,o4,&start4)==capacity_converged);
	CHECK(r4.geometry_builds>r4.iterations+1);
	for(size_t k=1;k<r4.history.size();k++) {
		CHECK(r4.history[k].max_rel_error<r4.history[k-1].max_rel_error);
		CHECK(r4.history[k].min_volume>0);
	}
	CHECK(r4.history.back().geometry_builds==r4.geometry_builds);
}

} // namespace

int main() {
	int cases=0;
	case_two_seeds(); cases++;
	case_three_slabs(); cases++;
	case_asymmetric_3d(); cases++;
	case_adjacency_change(); cases++;
	case_equal_vs_unequal_start(); cases++;
	case_weight_shift_invariance(); cases++;
	case_permutation(); cases++;
	case_scaling_translation(); cases++;
	case_derivative_central_difference(); cases++;
	case_centroid_face_volume_crosscheck(); cases++;
	case_invalid_inputs(); cases++;
	case_empty_initial(); cases++;
	case_iteration_limits(); cases++;
	printf("CAPACITY_OK cases=%d\n",cases);
	return 0;
}
