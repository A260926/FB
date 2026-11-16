/////////////////////////////////////////////
//
// Mesh Simplification Tutorial
//
// (C) by Sven Forstmann in 2014
//
// License : MIT
// http://opensource.org/licenses/MIT
//
//https://github.com/sp4cerat/Fast-Quadric-Mesh-Simplification
//
// 5/2016: Chris Rorden created minimal version for OSX/Linux/Windows compile

//#include <iostream>
//#include <stddef.h>
//#include <functional>
//#include <sys/stat.h>
//#include <stdbool.h>
#include <string.h>
//#include <ctype.h>
//#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <map>
#include <vector>
#include <string>
#include <algorithm>
#include <stdexcept>
#include <cmath>
#include <math.h>
#include <float.h> //FLT_EPSILON, DBL_EPSILON

#define loopi(start_l,end_l) for ( int i=start_l;i<end_l;++i )
#define loopi(start_l,end_l) for ( int i=start_l;i<end_l;++i )
#define loopj(start_l,end_l) for ( int j=start_l;j<end_l;++j )
#define loopk(start_l,end_l) for ( int k=start_l;k<end_l;++k )

struct vector3
{
double x, y, z;
};

struct vec3f
{
	double x, y, z;

	inline vec3f( void ) {}

	//inline vec3f operator =( vector3 a )
	// { vec3f b ; b.x = a.x; b.y = a.y; b.z = a.z; return b;}

	inline vec3f( vector3 a )
	 { x = a.x; y = a.y; z = a.z; }

	inline vec3f( const double X, const double Y, const double Z )
	{ x = X; y = Y; z = Z; }

	inline vec3f operator + ( const vec3f& a ) const
	{ return vec3f( x + a.x, y + a.y, z + a.z ); }

	inline vec3f operator += ( const vec3f& a ) const
	{ return vec3f( x + a.x, y + a.y, z + a.z ); }

	inline vec3f operator * ( const double a ) const
	{ return vec3f( x * a, y * a, z * a ); }

	inline vec3f operator * ( const vec3f a ) const
	{ return vec3f( x * a.x, y * a.y, z * a.z ); }

	inline vec3f v3 () const
	{ return vec3f( x , y, z ); }

	inline vec3f operator = ( const vector3 a )
	{ x=a.x;y=a.y;z=a.z;return *this; }

	inline vec3f operator = ( const vec3f a )
	{ x=a.x;y=a.y;z=a.z;return *this; }

	inline vec3f operator / ( const vec3f a ) const
	{ return vec3f( x / a.x, y / a.y, z / a.z ); }

	inline vec3f operator - ( const vec3f& a ) const
	{ return vec3f( x - a.x, y - a.y, z - a.z ); }

	inline vec3f operator / ( const double a ) const
	{ return vec3f( x / a, y / a, z / a ); }

	inline double dot( const vec3f& a ) const
	{ return a.x*x + a.y*y + a.z*z; }

	inline vec3f cross( const vec3f& a , const vec3f& b )
	{
		x = a.y * b.z - a.z * b.y;
		y = a.z * b.x - a.x * b.z;
		z = a.x * b.y - a.y * b.x;
		return *this;
	}

	inline double angle( const vec3f& v )
	{
		vec3f a = v , b = *this;
		double dot = v.x*x + v.y*y + v.z*z;
		double len = a.length() * b.length();
		if(len==0)len=0.00001f;
		double input = dot  / len;
		if (input<-1) input=-1;
		if (input>1) input=1;
		return (double) acos ( input );
	}

	inline double angle2( const vec3f& v , const vec3f& w )
	{
		vec3f a = v , b= *this;
		double dot = a.x*b.x + a.y*b.y + a.z*b.z;
		double len = a.length() * b.length();
		if(len==0)len=1;

		vec3f plane; plane.cross( b,w );

		if ( plane.x * a.x + plane.y * a.y + plane.z * a.z > 0 )
			return (double) -acos ( dot  / len );

		return (double) acos ( dot  / len );
	}

	inline vec3f rot_x( double a )
	{
		double yy = cos ( a ) * y + sin ( a ) * z;
		double zz = cos ( a ) * z - sin ( a ) * y;
		y = yy; z = zz;
		return *this;
	}
	inline vec3f rot_y( double a )
	{
		double xx = cos ( -a ) * x + sin ( -a ) * z;
		double zz = cos ( -a ) * z - sin ( -a ) * x;
		x = xx; z = zz;
		return *this;
	}
	inline void clamp( double min, double max )
	{
		if (x<min) x=min;
		if (y<min) y=min;
		if (z<min) z=min;
		if (x>max) x=max;
		if (y>max) y=max;
		if (z>max) z=max;
	}
	inline vec3f rot_z( double a )
	{
		double yy = cos ( a ) * y + sin ( a ) * x;
		double xx = cos ( a ) * x - sin ( a ) * y;
		y = yy; x = xx;
		return *this;
	}
	inline vec3f invert()
	{
		x=-x;y=-y;z=-z;return *this;
	}
	inline vec3f frac()
	{
		return vec3f(
			x-double(int(x)),
			y-double(int(y)),
			z-double(int(z))
			);
	}

	inline vec3f integer()
	{
		return vec3f(
			double(int(x)),
			double(int(y)),
			double(int(z))
			);
	}

	inline double length() const
	{
		return (double)sqrt(x*x + y*y + z*z);
	}

	inline vec3f normalize( double desired_length = 1 )
	{
		double square = sqrt(x*x + y*y + z*z);
		/*
		if (square <= 0.00001f )
		{
			x=1;y=0;z=0;
			return *this;
		}*/
		//double len = desired_length / square;
		x/=square;y/=square;z/=square;

		return *this;
	}
	static vec3f normalize( vec3f a );

	static void random_init();
	static double random_double();
	static vec3f random();

	static int random_number;

	double random_double_01(double a){
		double rnf=a*14.434252+a*364.2343+a*4213.45352+a*2341.43255+a*254341.43535+a*223454341.3523534245+23453.423412;
		int rni=((int)rnf)%100000;
		return double(rni)/(100000.0f-1.0f);
	}

	vec3f random01_fxyz(){
		x=(double)random_double_01(x);
		y=(double)random_double_01(y);
		z=(double)random_double_01(z);
		return *this;
	}

};

vec3f barycentric(const vec3f &p, const vec3f &a, const vec3f &b, const vec3f &c){
	vec3f v0 = b-a;
	vec3f v1 = c-a;
	vec3f v2 = p-a;
	double d00 = v0.dot(v0);
	double d01 = v0.dot(v1);
	double d11 = v1.dot(v1);
	double d20 = v2.dot(v0);
	double d21 = v2.dot(v1);
	double denom = d00*d11-d01*d01;
	double v = (d11 * d20 - d01 * d21) / denom;
	double w = (d00 * d21 - d01 * d20) / denom;
	double u = 1.0 - v - w;
	return vec3f(u,v,w);
}

vec3f interpolate(const vec3f &p, const vec3f &a, const vec3f &b, const vec3f &c, const vec3f attrs[3])
{
	vec3f bary = barycentric(p,a,b,c);
	vec3f out = vec3f(0,0,0);
	out = out + attrs[0] * bary.x;
	out = out + attrs[1] * bary.y;
	out = out + attrs[2] * bary.z;
	return out;
}

double min(double v1, double v2) {
	return fmin(v1,v2);
}


class SymetricMatrix {

	public:

	// Constructor

	SymetricMatrix(double c=0) { loopi(0,10) m[i] = c;  }

	SymetricMatrix(	double m11, double m12, double m13, double m14,
		            double m22, double m23, double m24,
		                        double m33, double m34,
		                                    double m44) {
		m[0] = m11;  m[1] = m12;  m[2] = m13;  m[3] = m14;
		             m[4] = m22;  m[5] = m23;  m[6] = m24;
		                          m[7] = m33;  m[8] = m34;
		                                       m[9] = m44;
	}

	// Make plane

	SymetricMatrix(double a,double b,double c,double d)
	{
		m[0] = a*a;  m[1] = a*b;  m[2] = a*c;  m[3] = a*d;
		             m[4] = b*b;  m[5] = b*c;  m[6] = b*d;
		                          m[7 ] =c*c; m[8 ] = c*d;
		                                       m[9 ] = d*d;
	}

	double operator[](int c) const { return m[c]; }

	// Determinant

	double det(	int a11, int a12, int a13,
				int a21, int a22, int a23,
				int a31, int a32, int a33)
	{
		double det =  m[a11]*m[a22]*m[a33] + m[a13]*m[a21]*m[a32] + m[a12]*m[a23]*m[a31]
					- m[a13]*m[a22]*m[a31] - m[a11]*m[a23]*m[a32]- m[a12]*m[a21]*m[a33];
		return det;
	}

	const SymetricMatrix operator+(const SymetricMatrix& n) const
	{
		return SymetricMatrix( m[0]+n[0],   m[1]+n[1],   m[2]+n[2],   m[3]+n[3],
						                    m[4]+n[4],   m[5]+n[5],   m[6]+n[6],
						                                 m[ 7]+n[ 7], m[ 8]+n[8 ],
						                                              m[ 9]+n[9 ]);
	}

	SymetricMatrix& operator+=(const SymetricMatrix& n)
	{
		 m[0]+=n[0];   m[1]+=n[1];   m[2]+=n[2];   m[3]+=n[3];
		 m[4]+=n[4];   m[5]+=n[5];   m[6]+=n[6];   m[7]+=n[7];
		 m[8]+=n[8];   m[9]+=n[9];
		return *this;
	}

	double m[10];
};
///////////////////////////////////////////

namespace Simplify
{
	// Global Variables & Strctures
	enum Attributes {
		NONE,
		NORMAL = 2,
		TEXCOORD = 4,
		COLOR = 8
	};
	struct Triangle { int v[3];double err[4];int deleted,dirty,attr;vec3f n;vec3f uvs[3];int material; };
	struct Vertex { vec3f p;int tstart,tcount;SymetricMatrix q;int border;};
	struct Ref { int tid,tvertex; };
	std::vector<Triangle> triangles;
	std::vector<Vertex> vertices;
	std::vector<Ref> refs;
	std::string mtllib;
	std::vector<std::string> materials;

	// Reflection symmetry description used by the optional symmetry-aware
	// simplification. The mirror plane is normal.p + offset == 0 with `normal`
	// a unit vector. `mate[i]` is the index of the vertex that mirrors vertex i
	// (mate[mate[i]] == i); a self-mate lies on the plane. Input coordinates are
	// only required to mirror up to `input_tolerance`. After a simplification
	// call `original_to_vertex` maps every pre-call vertex to its final
	// representative index, or -1 when the vertex is not referenced by any
	// surviving face. `mate` is rewritten with compacted indices.
	struct SymmetryContext
	{
		vec3f normal;
		double offset;
		double input_tolerance;
		std::vector<int> mate;
		std::vector<int> original_to_vertex;
	};

	// Helper functions

	double vertex_error(SymetricMatrix q, double x, double y, double z);
	double calculate_error(int id_v1, int id_v2, vec3f &p_result);
	bool flipped(vec3f p,int i0,int i1,Vertex &v0,Vertex &v1,std::vector<int> &deleted);
	void update_uvs(int i0,const Vertex &v,const vec3f &p,std::vector<int> &deleted);
	void update_triangles(int i0,Vertex &v,std::vector<int> &deleted,int &deleted_triangles);
	void update_mesh(int iteration);
	void compact_mesh();
	struct SymmetryContext;
	void simplify_mesh_symmetric(SymmetryContext &ctx, int target_count,
		double agressiveness, bool verbose, bool lossless);
	//
	// Main simplification function
	//
	// target_count  : target nr. of triangles
	// agressiveness : sharpness to increase the threshold.
	//                 5..8 are good numbers
	//                 more iterations yield higher quality
	//

	void simplify_mesh(int target_count, double agressiveness=7, bool verbose=false, SymmetryContext *symmetry=0)
	{
		if (symmetry) { simplify_mesh_symmetric(*symmetry, target_count, agressiveness, verbose, false); return; }
		// init
		loopi(0,triangles.size())
		{
			triangles[i].deleted=0;
		}

		// main iteration loop
		int deleted_triangles=0;
		std::vector<int> deleted0,deleted1;
		int triangle_count=triangles.size();
		//int iteration = 0;
		//loop(iteration,0,100)
		for (int iteration = 0; iteration < 100; iteration ++)
		{
			if(triangle_count-deleted_triangles<=target_count)break;

			// update mesh once in a while
			if(iteration%5==0)
			{
				update_mesh(iteration);
			}

			// clear dirty flag
			loopi(0,triangles.size()) triangles[i].dirty=0;

			//
			// All triangles with edges below the threshold will be removed
			//
			// The following numbers works well for most models.
			// If it does not, try to adjust the 3 parameters
			//
			double threshold = 0.000000001*pow(double(iteration+3),agressiveness);

			// target number of triangles reached ? Then break
			if ((verbose) && (iteration%5==0)) {
				printf("iteration %d - triangles %d threshold %g\n",iteration,triangle_count-deleted_triangles, threshold);
			}

			// remove vertices & mark deleted triangles
			loopi(0,triangles.size())
			{
				Triangle &t=triangles[i];
				if(t.err[3]>threshold) continue;
				if(t.deleted) continue;
				if(t.dirty) continue;

				loopj(0,3)if(t.err[j]<threshold)
				{

					int i0=t.v[ j     ]; Vertex &v0 = vertices[i0];
					int i1=t.v[(j+1)%3]; Vertex &v1 = vertices[i1];
					// Border check
					if(v0.border != v1.border)  continue;

					// Compute vertex to collapse to
					vec3f p;
					calculate_error(i0,i1,p);
					deleted0.resize(v0.tcount); // normals temporarily
					deleted1.resize(v1.tcount); // normals temporarily
					// don't remove if flipped
					if( flipped(p,i0,i1,v0,v1,deleted0) ) continue;

					if( flipped(p,i1,i0,v1,v0,deleted1) ) continue;

					if ( (t.attr & TEXCOORD) == TEXCOORD  )
					{
						update_uvs(i0,v0,p,deleted0);
						update_uvs(i0,v1,p,deleted1);
					}

					// not flipped, so remove edge
					v0.p=p;
					v0.q=v1.q+v0.q;
					int tstart=refs.size();

					update_triangles(i0,v0,deleted0,deleted_triangles);
					update_triangles(i0,v1,deleted1,deleted_triangles);

					int tcount=refs.size()-tstart;

					if(tcount<=v0.tcount)
					{
						// save ram
						if(tcount)memcpy(&refs[v0.tstart],&refs[tstart],tcount*sizeof(Ref));
					}
					else
						// append
						v0.tstart=tstart;

					v0.tcount=tcount;
					break;
				}
				// done?
				if(triangle_count-deleted_triangles<=target_count)break;
			}
		}
		// clean up mesh
		compact_mesh();
	} //simplify_mesh()

	void simplify_mesh_lossless(bool verbose=false, SymmetryContext *symmetry=0)
	{
		if (symmetry) { simplify_mesh_symmetric(*symmetry, 0, 0, verbose, true); return; }
		// init
		loopi(0,triangles.size()) triangles[i].deleted=0;

		// main iteration loop
		int deleted_triangles=0;
		std::vector<int> deleted0,deleted1;
		int triangle_count=triangles.size();
		//int iteration = 0;
		//loop(iteration,0,100)
		for (int iteration = 0; iteration < 9999; iteration ++)
		{
			// update mesh constantly
			update_mesh(iteration);
			// clear dirty flag
			loopi(0,triangles.size()) triangles[i].dirty=0;
			//
			// All triangles with edges below the threshold will be removed
			//
			// The following numbers works well for most models.
			// If it does not, try to adjust the 3 parameters
			//
			double threshold = DBL_EPSILON; //1.0E-3 EPS;
			if (verbose) {
				printf("lossless iteration %d\n", iteration);
			}

			// remove vertices & mark deleted triangles
			loopi(0,triangles.size())
			{
				Triangle &t=triangles[i];
				if(t.err[3]>threshold) continue;
				if(t.deleted) continue;
				if(t.dirty) continue;

				loopj(0,3)if(t.err[j]<threshold)
				{
					int i0=t.v[ j     ]; Vertex &v0 = vertices[i0];
					int i1=t.v[(j+1)%3]; Vertex &v1 = vertices[i1];

					// Border check
					if(v0.border != v1.border)  continue;

					// Compute vertex to collapse to
					vec3f p;
					calculate_error(i0,i1,p);

					deleted0.resize(v0.tcount); // normals temporarily
					deleted1.resize(v1.tcount); // normals temporarily

					// don't remove if flipped
					if( flipped(p,i0,i1,v0,v1,deleted0) ) continue;
					if( flipped(p,i1,i0,v1,v0,deleted1) ) continue;

					if ( (t.attr & TEXCOORD) == TEXCOORD )
					{
						update_uvs(i0,v0,p,deleted0);
						update_uvs(i0,v1,p,deleted1);
					}

					// not flipped, so remove edge
					v0.p=p;
					v0.q=v1.q+v0.q;
					int tstart=refs.size();

					update_triangles(i0,v0,deleted0,deleted_triangles);
					update_triangles(i0,v1,deleted1,deleted_triangles);

					int tcount=refs.size()-tstart;

					if(tcount<=v0.tcount)
					{
						// save ram
						if(tcount)memcpy(&refs[v0.tstart],&refs[tstart],tcount*sizeof(Ref));
					}
					else
						// append
						v0.tstart=tstart;

					v0.tcount=tcount;
					break;
				}
			}
			if(deleted_triangles<=0)break;
			deleted_triangles=0;
		} //for each iteration
		// clean up mesh
		compact_mesh();
	} //simplify_mesh_lossless()


	/////////////////////////////////////////////////////////////////////
	// Reflection-symmetry constrained simplification.
	//
	// A candidate edge {a,b} is always considered together with its mirror
	// image {mate[a],mate[b]}. The whole orbit is merged into one or two result
	// clusters and the two result positions are constrained to be exact
	// mirror images of each other. Nothing is committed unless the entire
	// orbit is legal, so the two sides of the mesh are always processed
	// together and stay in exact correspondence.

	struct SymOrbit
	{
		int a, b;        // candidate edge
		int c, d;        // mate[a], mate[b]
		int nmem;        // number of distinct vertices involved
		int mem[4];      // distinct members (current indices)
		int cl[4];       // cluster of each member (0 or 1)
		int nclusters;   // 1 for a self-corresponding orbit, 2 otherwise
		int tgt[2];      // representative index of each cluster
	};

	struct SymSolution
	{
		bool ok;
		vec3f pos[2];
		double cost;
	};

	// State of the symmetry constrained run (null in plain mode)
	SymmetryContext *g_sym_ctx = 0;
	std::vector<int> g_sym_cur_orig; // current vertex -> canonical original id
	std::vector<int> g_sym_uf;       // union find over original ids

	inline int sym_uf_find(int x)
	{
		while (g_sym_uf[x] != x) { g_sym_uf[x] = g_sym_uf[g_sym_uf[x]]; x = g_sym_uf[x]; }
		return x;
	}

	inline void sym_uf_union(int a, int b)
	{
		a = sym_uf_find(a); b = sym_uf_find(b);
		if (a == b) return;
		if (a < b) g_sym_uf[b] = a; else g_sym_uf[a] = b;
	}

	inline bool sym_finite(double v) { return std::isfinite(v); }

	inline bool sym_finite_vec(const vec3f &p)
	{
		return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
	}

	inline vec3f sym_reflect_point(const SymmetryContext &c, const vec3f &p)
	{
		double t = c.normal.x*p.x + c.normal.y*p.y + c.normal.z*p.z + c.offset;
		return vec3f(p.x - 2.0*t*c.normal.x,
		             p.y - 2.0*t*c.normal.y,
		             p.z - 2.0*t*c.normal.z);
	}

	inline void sym_reflection_matrix(const SymmetryContext &c, double S[9], double v0[3])
	{
		double nx = c.normal.x, ny = c.normal.y, nz = c.normal.z;
		S[0] = 1-2*nx*nx; S[1] = -2*nx*ny;  S[2] = -2*nx*nz;
		S[3] = -2*ny*nx;  S[4] = 1-2*ny*ny; S[5] = -2*ny*nz;
		S[6] = -2*nz*nx;  S[7] = -2*nz*ny;  S[8] = 1-2*nz*nz;
		v0[0] = -2*c.offset*nx; v0[1] = -2*c.offset*ny; v0[2] = -2*c.offset*nz;
	}

	inline void sym_mat_mul(const double A[9], const double B[9], double C[9])
	{
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
				C[i*3+j] = A[i*3+0]*B[0*3+j] + A[i*3+1]*B[1*3+j] + A[i*3+2]*B[2*3+j];
	}

	inline void sym_mat_vec(const double A[9], const double v[3], double out[3])
	{
		for (int i = 0; i < 3; ++i)
			out[i] = A[i*3+0]*v[0] + A[i*3+1]*v[1] + A[i*3+2]*v[2];
	}

	inline void sym_matT_vec(const double A[9], const double v[3], double out[3])
	{
		for (int j = 0; j < 3; ++j)
			out[j] = A[0*3+j]*v[0] + A[1*3+j]*v[1] + A[2*3+j]*v[2];
	}

	// Packed quadric -> quadratic form x^T A x + 2 b.x + c
	inline void sym_quadric_parts(const SymetricMatrix &q, double A[9], double b[3], double &c)
	{
		A[0] = q.m[0]; A[1] = q.m[1]; A[2] = q.m[2];
		A[3] = q.m[1]; A[4] = q.m[4]; A[5] = q.m[5];
		A[6] = q.m[2]; A[7] = q.m[5]; A[8] = q.m[7];
		b[0] = q.m[3]; b[1] = q.m[6]; b[2] = q.m[8];
		c    = q.m[9];
	}

	// Cyclic Jacobi eigen decomposition of a small symmetric matrix.
	// On return d[i] are the eigenvalues and column i of V is the eigenvector.
	inline void sym_jacobi(int n, const double Ain[3][3], double V[3][3], double d[3])
	{
		double A[3][3];
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
			{
				A[i][j] = (i < n && j < n) ? Ain[i][j] : (i == j ? 1.0 : 0.0);
				V[i][j] = (i == j) ? 1.0 : 0.0;
			}
		for (int sweep = 0; sweep < 80; ++sweep)
		{
			double off = 0, diag = 0;
			for (int p = 0; p < n; ++p)
			{
				diag += A[p][p]*A[p][p];
				for (int q = p+1; q < n; ++q) off += A[p][q]*A[p][q];
			}
			if (off <= 1e-36*diag || off == 0) break;
			for (int p = 0; p < n; ++p)
				for (int q = p+1; q < n; ++q)
				{
					if (A[p][q] == 0) continue;
					double theta = (A[q][q]-A[p][p]) / (2.0*A[p][q]);
					double t = ((theta >= 0) ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta*theta + 1.0));
					double c = 1.0 / std::sqrt(t*t + 1.0);
					double s = t*c;
					double app = A[p][p], aqq = A[q][q], apq = A[p][q];
					for (int i = 0; i < n; ++i)
					{
						if (i == p || i == q) continue;
						double aip = A[i][p], aiq = A[i][q];
						A[i][p] = A[p][i] = c*aip - s*aiq;
						A[i][q] = A[q][i] = s*aip + c*aiq;
					}
					A[p][p] = c*c*app - 2.0*s*c*apq + s*s*aqq;
					A[q][q] = s*s*app + 2.0*s*c*apq + c*c*aqq;
					A[p][q] = A[q][p] = 0.0;
					for (int i = 0; i < 3; ++i)
					{
						double vip = V[i][p], viq = V[i][q];
						V[i][p] = c*vip - s*viq;
						V[i][q] = s*vip + c*viq;
					}
				}
		}
		for (int i = 0; i < n; ++i) d[i] = A[i][i];
	}

	// Minimum norm solution of A x = rhs for a symmetric positive
	// semi-definite A (directions with a vanishing eigenvalue are skipped).
	inline void sym_solve_minnorm(int n, const double Ain[3][3], const double rhs[3],
		double x[3], double scale)
	{
		double V[3][3], d[3];
		sym_jacobi(n, Ain, V, d);
		x[0] = x[1] = x[2] = 0;
		double maxd = 0;
		for (int i = 0; i < n; ++i) maxd = std::max(maxd, std::fabs(d[i]));
		if (!(maxd > 0)) return;
		// Numerical rank: directions whose eigenvalue is negligible compared to
		// both the largest eigenvalue and the original quadratic form are
		// treated as flat (the QEM is minimised on that whole direction), so
		// round-off does not amplify into a wildly displaced "solution".
		double tol = std::max(maxd*1e-10, scale*1e-13);
		for (int i = 0; i < n; ++i)
		{
			if (!(std::fabs(d[i]) > tol)) continue;
			double alpha = (V[0][i]*rhs[0] + V[1][i]*rhs[1] + V[2][i]*rhs[2]) / d[i];
			for (int j = 0; j < n; ++j) x[j] += alpha*V[j][i];
		}
	}

	inline bool sym_build_orbit(int a, int b, SymOrbit &o)
	{
		if (!g_sym_ctx) return false;
		const std::vector<int> &mate = g_sym_ctx->mate;
		int n = (int)vertices.size();
		if (a < 0 || b < 0 || a >= n || b >= n || a == b) return false;
		if ((int)mate.size() != n) return false;
		int c = mate[a], d = mate[b];
		if (c < 0 || d < 0 || c >= n || d >= n) return false;
		if (mate[c] != a || mate[d] != b) { return false; }
		o.a = a; o.b = b; o.c = c; o.d = d;
		const int raw[4] = {a, b, c, d};
		const int craw[4] = {0, 0, 1, 1};
		o.nmem = 0;
		for (int k = 0; k < 4; ++k)
		{
			int v = raw[k], found = -1;
			for (int m = 0; m < o.nmem; ++m) if (o.mem[m] == v) { found = m; break; }
			if (found < 0) { o.mem[o.nmem] = v; o.cl[o.nmem] = craw[k]; ++o.nmem; }
		}
		bool single = (c == a) || (c == b) || (d == a) || (d == b);
		if (single)
		{
			o.nclusters = 1;
			for (int m = 0; m < o.nmem; ++m) o.cl[m] = 0;
			o.tgt[0] = a;
		}
		else
		{
			o.nclusters = 2;
			o.tgt[0] = a;
			o.tgt[1] = c;
		}
		return true;
	}

	inline SymetricMatrix sym_cluster_quadric(const SymOrbit &o, int cluster)
	{
		SymetricMatrix q(0.0);
		for (int m = 0; m < o.nmem; ++m)
			if (o.cl[m] == cluster) q += vertices[o.mem[m]].q;
		return q;
	}

	// Minimise the total QEM of the orbit under the reflection constraint.
	// Ordinary orbits carry two free positions (one being the mirror image of
	// the other), self-corresponding orbits carry one position on the plane.
	// A singular system resolves to the minimiser closest to the (reflected)
	// endpoint centre.
	inline bool sym_solve_orbit(const SymOrbit &o, SymSolution &sol)
	{
		sol.ok = false;
		sol.cost = DBL_MAX;
		if (!g_sym_ctx) return false;
		const SymmetryContext &ctx = *g_sym_ctx;
		for (int m = 0; m < o.nmem; ++m)
			if (o.mem[m] < 0 || o.mem[m] >= (int)vertices.size()) return false;
		if (!sym_finite_vec(ctx.normal) || !sym_finite(ctx.offset)) return false;

		if (o.nclusters == 2)
		{
			SymetricMatrix qa = sym_cluster_quadric(o, 0);
			SymetricMatrix qb = sym_cluster_quadric(o, 1);
			double A1[9], b1[3], c1, A2[9], b2[3], c2;
			sym_quadric_parts(qa, A1, b1, c1);
			sym_quadric_parts(qb, A2, b2, c2);
			double S[9], v0[3];
			sym_reflection_matrix(ctx, S, v0);
			double A2s[9], tmp[9], w[3], b2s[3], A2v0[3];
			sym_mat_mul(A2, S, tmp);
			sym_mat_mul(S, tmp, A2s);
			sym_mat_vec(A2, v0, A2v0);
			for (int i = 0; i < 3; ++i) w[i] = A2v0[i] + b2[i];
			sym_matT_vec(S, w, b2s);
			double A[9], b[3];
			for (int i = 0; i < 9; ++i) A[i] = A1[i] + A2s[i];
			for (int i = 0; i < 3; ++i) b[i] = b1[i] + b2s[i];

			vec3f pa = vertices[o.a].p, pb = vertices[o.b].p;
			vec3f pc = sym_reflect_point(ctx, vertices[o.c].p);
			vec3f pd = sym_reflect_point(ctx, vertices[o.d].p);
			vec3f center = (pa + pb + pc + pd) * 0.25;
			double x0[3] = {center.x, center.y, center.z};
			double Ax0[3], rhs[3];
			sym_mat_vec(A, x0, Ax0);
			for (int i = 0; i < 3; ++i) rhs[i] = -(Ax0[i] + b[i]);
			double Af[3][3] = {{A[0],A[1],A[2]},{A[3],A[4],A[5]},{A[6],A[7],A[8]}};
			double t[3];
			double qscale = 0;
			for (int i = 0; i < 10; ++i)
				qscale = std::max(qscale, std::fabs(qa.m[i]) + std::fabs(qb.m[i]));
			sym_solve_minnorm(3, Af, rhs, t, qscale);
			vec3f x(center.x+t[0], center.y+t[1], center.z+t[2]);
			vec3f y = sym_reflect_point(ctx, x);
			if (!sym_finite_vec(x) || !sym_finite_vec(y)) return false;
			sol.pos[0] = x;
			sol.pos[1] = y;
			sol.cost = vertex_error(qa, x.x, x.y, x.z) + vertex_error(qb, y.x, y.y, y.z);
			sol.ok = sym_finite(sol.cost);
			return sol.ok;
		}

		// self-corresponding orbit: single position constrained to the plane
		SymetricMatrix q = sym_cluster_quadric(o, 0);
		double A[9], b[3], c;
		sym_quadric_parts(q, A, b, c);
		vec3f sum(0,0,0);
		for (int m = 0; m < o.nmem; ++m) sum = sum + vertices[o.mem[m]].p;
		vec3f center = sum * (1.0/double(o.nmem));
		double nd = ctx.normal.x*center.x + ctx.normal.y*center.y + ctx.normal.z*center.z + ctx.offset;
		vec3f x0(center.x - nd*ctx.normal.x, center.y - nd*ctx.normal.y, center.z - nd*ctx.normal.z);

		double nx = ctx.normal.x, ny = ctx.normal.y, nz = ctx.normal.z;
		double ax = std::fabs(nx), ay = std::fabs(ny), az = std::fabs(nz);
		double e[3] = {0,0,0};
		if (ax <= ay && ax <= az) e[0] = 1;
		else if (ay <= az) e[1] = 1;
		else e[2] = 1;
		double u[3] = {ny*e[2]-nz*e[1], nz*e[0]-nx*e[2], nx*e[1]-ny*e[0]};
		double ulen = std::sqrt(u[0]*u[0]+u[1]*u[1]+u[2]*u[2]);
		if (!(ulen > 0)) return false;
		for (int i = 0; i < 3; ++i) u[i] /= ulen;
		double v[3] = {ny*u[2]-nz*u[1], nz*u[0]-nx*u[2], nx*u[1]-ny*u[0]};

		double Au[3], Av[3];
		sym_mat_vec(A, u, Au);
		sym_mat_vec(A, v, Av);
		double Ar[3][3] = {{0,0,0},{0,0,0},{0,0,0}};
		Ar[0][0] = u[0]*Au[0]+u[1]*Au[1]+u[2]*Au[2];
		Ar[0][1] = u[0]*Av[0]+u[1]*Av[1]+u[2]*Av[2];
		Ar[1][0] = Ar[0][1];
		Ar[1][1] = v[0]*Av[0]+v[1]*Av[1]+v[2]*Av[2];
		double x0v[3] = {x0.x, x0.y, x0.z};
		double g[3], Ax0[3];
		sym_mat_vec(A, x0v, Ax0);
		for (int i = 0; i < 3; ++i) g[i] = Ax0[i] + b[i];
		double rhs[3] = {-(u[0]*g[0]+u[1]*g[1]+u[2]*g[2]),
		                 -(v[0]*g[0]+v[1]*g[1]+v[2]*g[2]), 0};
		double t[3];
		double qscale = 0;
		for (int i = 0; i < 10; ++i) qscale = std::max(qscale, std::fabs(q.m[i]));
		sym_solve_minnorm(2, Ar, rhs, t, qscale);
		vec3f z(x0.x + u[0]*t[0] + v[0]*t[1],
		        x0.y + u[1]*t[0] + v[1]*t[1],
		        x0.z + u[2]*t[0] + v[2]*t[1]);
		// enforce the constraint exactly
		double zd = ctx.normal.x*z.x + ctx.normal.y*z.y + ctx.normal.z*z.z + ctx.offset;
		z = vec3f(z.x - zd*ctx.normal.x, z.y - zd*ctx.normal.y, z.z - zd*ctx.normal.z);
		if (!sym_finite_vec(z)) return false;
		sol.pos[0] = z;
		sol.pos[1] = z;
		sol.cost = vertex_error(q, z.x, z.y, z.z);
		sol.ok = sym_finite(sol.cost);
		return sol.ok;
	}

	// Cost used for threshold scheduling: the joint orbit QEM.
	inline double sym_edge_error(int a, int b)
	{
		if (!g_sym_ctx) return DBL_MAX;
		SymOrbit o;
		if (!sym_build_orbit(a, b, o)) return DBL_MAX;
		SymSolution s;
		if (!sym_solve_orbit(o, s)) return DBL_MAX;
		return s.cost;
	}

	struct SymLocalFace
	{
		int tid;
		int ov[3];
		int cv[3];
		int cl[3];
		bool del;
	};

	// Apply one whole orbit.  All legality checks happen before any public
	// state is touched, so a rejected candidate leaves adjacency, quadrics,
	// mate, UVs and the source mapping untouched.
	inline bool try_symmetry_collapse(int a, int b, int &deleted_triangles,
		bool *flip_side0 = 0, bool *flip_side1 = 0)
	{
		SymOrbit o;
		if (!sym_build_orbit(a, b, o)) { return false; }
		if (g_sym_cur_orig.size() != vertices.size() || g_sym_uf.size() != vertices.size()) { return false; }

		int border = vertices[o.mem[0]].border;
		for (int m = 1; m < o.nmem; ++m)
			if (vertices[o.mem[m]].border != border) return false;

		SymSolution sol;
		if (!sym_solve_orbit(o, sol)) { return false; }

		int nv = (int)vertices.size();

		// gather the local face star of the whole orbit
		std::vector<int> tids;
		for (int m = 0; m < o.nmem; ++m)
		{
			int v = o.mem[m];
			if (v < 0 || v >= nv) { return false; }
			Vertex &vv = vertices[v];
			for (int k = 0; k < vv.tcount; ++k)
			{
				int tid = refs[vv.tstart+k].tid;
				if (tid < 0 || tid >= (int)triangles.size()) { return false; }
				Triangle &t = triangles[tid];
				if (t.deleted) continue;
				if (t.v[0] != v && t.v[1] != v && t.v[2] != v) continue;
				bool seen = false;
				for (size_t q = 0; q < tids.size(); ++q) if (tids[q] == tid) { seen = true; break; }
				if (!seen) tids.push_back(tid);
			}
		}
		if (tids.empty()) { return false; }

		std::vector<SymLocalFace> faces;
		faces.reserve(tids.size());
		for (size_t fi = 0; fi < tids.size(); ++fi)
		{
			SymLocalFace f;
			f.tid = tids[fi];
			Triangle &t = triangles[f.tid];
			int cnt[2] = {0,0};
			for (int k = 0; k < 3; ++k)
			{
				f.ov[k] = t.v[k];
				f.cl[k] = -1;
				for (int m = 0; m < o.nmem; ++m)
					if (o.mem[m] == f.ov[k]) { f.cl[k] = o.cl[m]; break; }
				if (f.cl[k] >= 0) ++cnt[f.cl[k]];
				f.cv[k] = f.ov[k];
			}
			f.del = (cnt[0] >= 2 || cnt[1] >= 2);
			if (!f.del)
			{
				for (int k = 0; k < 3; ++k)
					if (f.cl[k] >= 0) f.cv[k] = o.tgt[f.cl[k]];
				if (f.cv[0] == f.cv[1] || f.cv[1] == f.cv[2] || f.cv[2] == f.cv[0])
					f.del = true;
			}
			faces.push_back(f);
		}

		// neighbouring vertices (vertices of deleted faces are affected too)
		std::vector<int> nbrs;
		for (size_t fi = 0; fi < faces.size(); ++fi)
			for (int k = 0; k < 3; ++k)
			{
				int v = faces[fi].ov[k];
				if (faces[fi].cl[k] >= 0) continue;
				bool seen = false;
				for (size_t q = 0; q < nbrs.size(); ++q) if (nbrs[q] == v) { seen = true; break; }
				if (!seen) nbrs.push_back(v);
			}

		// no duplicate faces in the result
		for (size_t i = 0; i < faces.size(); ++i) if (!faces[i].del)
			for (size_t j = i+1; j < faces.size(); ++j) if (!faces[j].del)
			{
				int a0 = faces[i].cv[0], a1 = faces[i].cv[1], a2 = faces[i].cv[2];
				int b0 = faces[j].cv[0], b1 = faces[j].cv[1], b2 = faces[j].cv[2];
				bool same = true;
				int ua[3] = {a0,a1,a2}, ub[3] = {b0,b1,b2};
				for (int x = 0; x < 3; ++x)
				{
					bool found = false;
					for (int y = 0; y < 3; ++y) if (ua[x] == ub[y]) { found = true; ub[y] = -1; break; }
					if (!found) { same = false; break; }
				}
				if (same) { return false; }
			}

		// geometric flip test of the resulting local faces
		for (size_t fi = 0; fi < faces.size(); ++fi)
		{
			if (faces[fi].del) continue;
			vec3f P[3];
			for (int k = 0; k < 3; ++k)
				P[k] = (faces[fi].cl[k] >= 0) ? sol.pos[faces[fi].cl[k]] : vertices[faces[fi].ov[k]].p;
			vec3f d1 = P[1]-P[0], d2 = P[2]-P[0];
			vec3f n1 = d1, n2 = d2;
			double l1 = n1.length(), l2 = n2.length();
			bool sideFlipped = false;
			if (!(l1 > 0) || !(l2 > 0)) sideFlipped = true;
			else
			{
				n1.normalize(); n2.normalize();
				if (std::fabs(n1.dot(n2)) > 0.999) sideFlipped = true;
				else
				{
					vec3f nrm; nrm.cross(d1, d2);
					double area = nrm.length();
					if (!(area > 0) || !sym_finite(area)) sideFlipped = true;
					else
					{
						nrm.normalize();
						if (nrm.dot(triangles[faces[fi].tid].n) < 0.2) sideFlipped = true;
					}
				}
			}
			if (sideFlipped)
			{
				int side = 0;
				for (int k = 0; k < 3; ++k) if (faces[fi].cl[k] == 1) side = 1;
				if (side == 1) { if (flip_side1) *flip_side1 = true; }
				else { if (flip_side0) *flip_side0 = true; }
				return false;
			}
		}

		// link of every resulting representative must be a single fan
		for (int cluster = 0; cluster < o.nclusters; ++cluster)
		{
			int tgt = o.tgt[cluster];
			std::vector<int> nodes;
			std::vector<int> edgeA, edgeB;
			for (size_t fi = 0; fi < faces.size(); ++fi)
			{
				if (faces[fi].del) continue;
				int k = -1;
				for (int q = 0; q < 3; ++q) if (faces[fi].cv[q] == tgt) { k = q; break; }
				if (k < 0) continue;
				int u = faces[fi].cv[(k+1)%3], w = faces[fi].cv[(k+2)%3];
				bool us = false, ws = false;
				for (size_t q = 0; q < nodes.size(); ++q)
				{
					if (nodes[q] == u) us = true;
					if (nodes[q] == w) ws = true;
				}
				if (!us) nodes.push_back(u);
				if (!ws) nodes.push_back(w);
				// one link edge per incident face; a repeated edge means the
				// surface is pinched along it (non manifold)
				for (size_t e = 0; e < edgeA.size(); ++e)
					if ((edgeA[e] == u && edgeB[e] == w) || (edgeA[e] == w && edgeB[e] == u)) { return false; }
				edgeA.push_back(u); edgeB.push_back(w);
			}
			if (nodes.empty() || edgeA.empty()) { return false; }
			int deg[3];
			(void)deg;
			int boundaryNodes = 0;
			std::vector<int> degv(nodes.size(), 0);
			for (size_t e = 0; e < edgeA.size(); ++e)
			{
				for (size_t q = 0; q < nodes.size(); ++q)
				{
					if (nodes[q] == edgeA[e]) ++degv[q];
					if (nodes[q] == edgeB[e]) ++degv[q];
				}
			}
			for (size_t q = 0; q < nodes.size(); ++q)
			{
				if (degv[q] == 1) ++boundaryNodes;
				else if (degv[q] != 2) { return false; }
			}
			bool cycle = (boundaryNodes == 0);
			bool path = (boundaryNodes == 2);
			if (!cycle && !path) { return false; }
			if (border == 0 && !cycle) { return false; }
			if (border == 1 && !path) { return false; }
			// connectivity of the link
			std::vector<int> comp(nodes.size(), -1);
			std::vector<int> stack;
			comp[0] = 0; stack.push_back(0);
			while (!stack.empty())
			{
				int ci = stack.back(); stack.pop_back();
				for (size_t e = 0; e < edgeA.size(); ++e)
				{
					int other = -1;
					int ia = -1, ib = -1;
					for (size_t q = 0; q < nodes.size(); ++q)
					{
						if (nodes[q] == edgeA[e]) ia = (int)q;
						if (nodes[q] == edgeB[e]) ib = (int)q;
					}
					if (ia == ci) other = ib;
					else if (ib == ci) other = ia;
					if (other >= 0 && comp[other] < 0) { comp[other] = 0; stack.push_back(other); }
				}
			}
			for (size_t q = 0; q < nodes.size(); ++q) if (comp[q] < 0) { return false; }
		}

		// every neighbouring vertex keeps a single connected face fan
		for (size_t ni = 0; ni < nbrs.size(); ++ni)
		{
			int x = nbrs[ni];
			Vertex &vv = vertices[x];
			std::vector<int> fa, fb, fc;
			for (int k = 0; k < vv.tcount; ++k)
			{
				int tid = refs[vv.tstart+k].tid;
				if (tid < 0 || tid >= (int)triangles.size()) continue;
				if (triangles[tid].deleted) continue;
				int li = -1;
				for (size_t fi = 0; fi < faces.size(); ++fi) if (faces[fi].tid == tid) { li = (int)fi; break; }
				if (li >= 0)
				{
					if (faces[li].del) continue;
					if (!(faces[li].cv[0] == x || faces[li].cv[1] == x || faces[li].cv[2] == x)) continue;
					fa.push_back(faces[li].cv[0]);
					fb.push_back(faces[li].cv[1]);
					fc.push_back(faces[li].cv[2]);
				}
				else
				{
					Triangle &t = triangles[tid];
					if (t.v[0] != x && t.v[1] != x && t.v[2] != x) continue;
					fa.push_back(t.v[0]); fb.push_back(t.v[1]); fc.push_back(t.v[2]);
				}
			}
			if (fa.empty()) { return false; }
			size_t m = fa.size();
			std::vector<int> comp(m, -1);
			std::vector<int> stack;
			comp[0] = 0; stack.push_back(0);
			while (!stack.empty())
			{
				int ci = stack.back(); stack.pop_back();
				int tri_i[3] = {fa[ci], fb[ci], fc[ci]};
				for (size_t j = 0; j < m; ++j)
				{
					if (comp[j] >= 0) continue;
					int tri_j[3] = {fa[j], fb[j], fc[j]};
					int shared = 0;
					for (int p = 0; p < 3; ++p)
					{
						if (tri_i[p] == x) continue;
						for (int q = 0; q < 3; ++q) if (tri_j[q] == tri_i[p]) { ++shared; break; }
					}
					if (shared >= 1) { comp[j] = 0; stack.push_back((int)j); }
				}
			}
			for (size_t j = 0; j < m; ++j) if (comp[j] < 0) { return false; }
		}

		// ---------------- commit ----------------
		// UV interpolation uses the pre-collapse geometry and UV values
		for (size_t fi = 0; fi < faces.size(); ++fi)
		{
			if (faces[fi].del) continue;
			Triangle &t = triangles[faces[fi].tid];
			vec3f ouv[3] = {t.uvs[0], t.uvs[1], t.uvs[2]};
			vec3f p1 = vertices[t.v[0]].p, p2 = vertices[t.v[1]].p, p3 = vertices[t.v[2]].p;
			for (int k = 0; k < 3; ++k)
				if (faces[fi].cl[k] >= 0)
					t.uvs[k] = interpolate(sol.pos[faces[fi].cl[k]], p1, p2, p3, ouv);
		}

		// remember old adjacency before mutating vertex records
		std::vector<int> affected;
		for (int m = 0; m < o.nmem; ++m) affected.push_back(o.mem[m]);
		for (size_t q = 0; q < nbrs.size(); ++q) affected.push_back(nbrs[q]);
		std::vector<int> oldStart(affected.size()), oldCount(affected.size());
		for (size_t q = 0; q < affected.size(); ++q)
		{
			oldStart[q] = vertices[affected[q]].tstart;
			oldCount[q] = vertices[affected[q]].tcount;
		}

		// triangle updates
		for (size_t fi = 0; fi < faces.size(); ++fi)
		{
			Triangle &t = triangles[faces[fi].tid];
			if (faces[fi].del)
			{
				if (!t.deleted) { t.deleted = 1; ++deleted_triangles; }
			}
			else
			{
				for (int k = 0; k < 3; ++k) t.v[k] = faces[fi].cv[k];
				t.dirty = 1;
			}
		}

		// vertex updates
		SymetricMatrix qc[2];
		for (int m = 0; m < o.nmem; ++m) qc[o.cl[m]] += vertices[o.mem[m]].q;
		for (int cluster = 0; cluster < o.nclusters; ++cluster)
		{
			int tgt = o.tgt[cluster];
			vertices[tgt].p = sol.pos[cluster];
			vertices[tgt].q = qc[cluster];
			vertices[tgt].border = border;
		}
		for (int m = 0; m < o.nmem; ++m)
		{
			int v = o.mem[m];
			bool isTgt = false;
			for (int cluster = 0; cluster < o.nclusters; ++cluster)
				if (o.tgt[cluster] == v) isTgt = true;
			if (!isTgt) vertices[v].tcount = 0;
		}

		// source mapping
		for (int cluster = 0; cluster < o.nclusters; ++cluster)
		{
			int tgt = o.tgt[cluster];
			for (int m = 0; m < o.nmem; ++m)
				if (o.cl[m] == cluster) sym_uf_union(g_sym_cur_orig[tgt], g_sym_cur_orig[o.mem[m]]);
			g_sym_cur_orig[tgt] = sym_uf_find(g_sym_cur_orig[tgt]);
		}

		// mate update
		if (o.nclusters == 2)
		{
			g_sym_ctx->mate[o.tgt[0]] = o.tgt[1];
			g_sym_ctx->mate[o.tgt[1]] = o.tgt[0];
		}
		else
		{
			g_sym_ctx->mate[o.tgt[0]] = o.tgt[0];
		}

		// rebuild refs for the whole affected neighbourhood
		for (size_t q = 0; q < affected.size(); ++q)
		{
			int x = affected[q];
			bool isTgt = false;
			for (int cluster = 0; cluster < o.nclusters; ++cluster)
				if (o.tgt[cluster] == x) isTgt = true;
			if (!isTgt)
			{
				bool isMember = false;
				for (int m = 0; m < o.nmem; ++m) if (o.mem[m] == x) isMember = true;
				if (isMember) continue; // dead member
			}
			std::vector<Ref> list;
			for (int k = 0; k < oldCount[q]; ++k)
			{
				int idx = oldStart[q] + k;
				if (idx < 0 || idx >= (int)refs.size()) continue;
				Ref r = refs[idx];
				if (r.tid < 0 || r.tid >= (int)triangles.size()) continue;
				if (triangles[r.tid].deleted) continue;
				bool local = false;
				for (size_t fi = 0; fi < faces.size(); ++fi) if (faces[fi].tid == r.tid) { local = true; break; }
				if (local) continue;
				list.push_back(r);
			}
			for (size_t fi = 0; fi < faces.size(); ++fi)
			{
				if (faces[fi].del) continue;
				for (int k = 0; k < 3; ++k)
					if (faces[fi].cv[k] == x)
					{
						Ref r; r.tid = faces[fi].tid; r.tvertex = k;
						list.push_back(r);
					}
			}
			int start = (int)refs.size();
			for (size_t k = 0; k < list.size(); ++k) refs.push_back(list[k]);
			vertices[x].tstart = start;
			vertices[x].tcount = (int)list.size();
		}

		// refresh scheduling errors of the updated faces
		for (size_t fi = 0; fi < faces.size(); ++fi)
		{
			if (faces[fi].del) continue;
			Triangle &t = triangles[faces[fi].tid];
			for (int j = 0; j < 3; ++j)
				t.err[j] = sym_edge_error(t.v[j], t.v[(j+1)%3]);
			t.err[3] = min(t.err[0], min(t.err[1], t.err[2]));
		}
		return true;
	}

	void compact_mesh_symmetric()
	{
		if (!g_sym_ctx) return;
		int nv = (int)vertices.size();
		std::vector<int> old2new(nv, -1);
		for (int i = 0; i < nv; ++i) vertices[i].tcount = 0;
		for (size_t i = 0; i < triangles.size(); ++i)
			if (!triangles[i].deleted)
				for (int j = 0; j < 3; ++j) vertices[triangles[i].v[j]].tcount = 1;
		int dst = 0;
		for (int i = 0; i < nv; ++i)
			if (vertices[i].tcount)
			{
				old2new[i] = dst;
				vertices[dst].p = vertices[i].p;
				vertices[dst].q = vertices[i].q;
				vertices[dst].border = vertices[i].border;
				vertices[dst].tstart = dst;
				++dst;
			}
		int tdst = 0;
		for (size_t i = 0; i < triangles.size(); ++i)
			if (!triangles[i].deleted)
			{
				Triangle t = triangles[i];
				for (int j = 0; j < 3; ++j) t.v[j] = old2new[t.v[j]];
				t.deleted = 0;
				triangles[tdst++] = t;
			}
		triangles.resize(tdst);

		std::vector<int> newmate(dst, -1);
		for (int i = 0; i < nv; ++i)
			if (old2new[i] >= 0)
			{
				int m = (i < (int)g_sym_ctx->mate.size()) ? g_sym_ctx->mate[i] : i;
				int nm = (m >= 0 && m < nv) ? old2new[m] : -1;
				newmate[old2new[i]] = (nm >= 0) ? nm : old2new[i];
			}
		g_sym_ctx->mate.swap(newmate);

		// original -> final representative
		std::vector<int> oldcur = g_sym_cur_orig;
		g_sym_cur_orig.assign(dst, 0);
		for (int i = 0; i < nv; ++i)
			if (old2new[i] >= 0) g_sym_cur_orig[old2new[i]] = oldcur[i];
		std::vector<int> rootFinal(g_sym_uf.size(), -1);
		for (int j = 0; j < dst; ++j) rootFinal[sym_uf_find(g_sym_cur_orig[j])] = j;
		std::vector<int> out(g_sym_uf.size(), -1);
		for (size_t oi = 0; oi < g_sym_uf.size(); ++oi)
			out[oi] = rootFinal[sym_uf_find((int)oi)];
		g_sym_ctx->original_to_vertex.swap(out);

		vertices.resize(dst);
	}

	// ---- input validation and run setup ----------------------------------

	// Canonical (sorted) vertex triple of a face.
	inline void sym_sorted_tri(int a, int b, int c, int out[3])
	{
		if (a > b) std::swap(a, b);
		if (b > c) std::swap(b, c);
		if (a > b) std::swap(a, b);
		out[0] = a; out[1] = b; out[2] = c;
	}

	// Validate the symmetry description against the current mesh.  Throws
	// std::invalid_argument on any malformed description or non-mirroring
	// input. Nothing in the public mesh or context is modified.
	void sym_validate(const SymmetryContext &ctx, const std::vector<int> &face_canon)
	{
		int nv = (int)vertices.size();
		int nf = (int)triangles.size();
		const vec3f &nrm = ctx.normal;
		if (!sym_finite_vec(nrm)) throw std::invalid_argument("symmetry: non-finite normal");
		double nlen = std::sqrt(nrm.x*nrm.x + nrm.y*nrm.y + nrm.z*nrm.z);
		if (!(std::fabs(nlen - 1.0) <= 1e-6))
			throw std::invalid_argument("symmetry: normal is not a unit vector");
		if (!sym_finite(ctx.offset))
			throw std::invalid_argument("symmetry: non-finite offset");
		if (!sym_finite(ctx.input_tolerance) || ctx.input_tolerance < 0)
			throw std::invalid_argument("symmetry: invalid input tolerance");
		if ((int)ctx.mate.size() != nv)
			throw std::invalid_argument("symmetry: mate size does not match vertex count");
		for (int f = 0; f < nf; ++f)
			for (int k = 0; k < 3; ++k)
				if (triangles[f].v[k] < 0 || triangles[f].v[k] >= nv)
					throw std::invalid_argument("symmetry: triangle index out of range");
		for (int i = 0; i < nv; ++i)
		{
			int m = ctx.mate[i];
			if (m < 0 || m >= nv)
				throw std::invalid_argument("symmetry: mate index out of range");
			if (ctx.mate[m] != i)
				throw std::invalid_argument("symmetry: mate is not an involution");
			if (!sym_finite_vec(vertices[i].p))
				throw std::invalid_argument("symmetry: non-finite vertex position");
		}
		// vertex coordinates must match their mirror image
		double tol = ctx.input_tolerance;
		for (int i = 0; i < nv; ++i)
		{
			int m = ctx.mate[i];
			vec3f r = sym_reflect_point(ctx, vertices[i].p);
			vec3f diff = r - vertices[m].p;
			double d = diff.length();
			if (!(d <= tol))
				throw std::invalid_argument("symmetry: vertex pair exceeds input tolerance");
			if (m == i)
			{
				double pd = nrm.x*vertices[i].p.x + nrm.y*vertices[i].p.y + nrm.z*vertices[i].p.z + ctx.offset;
				if (!(std::fabs(pd) <= tol))
					throw std::invalid_argument("symmetry: self-mate is not on the plane");
			}
		}
		// face set must be invariant under reflection with reversed winding
		std::vector<std::vector<int> > face_list(nf);
		for (int f = 0; f < nf; ++f)
			face_list[f] = std::vector<int>(face_canon.begin()+f*3, face_canon.begin()+f*3+3);
		std::sort(face_list.begin(), face_list.end());
		for (int f = 0; f < nf; ++f)
		{
			int a = triangles[f].v[0], b = triangles[f].v[1], c = triangles[f].v[2];
			int m0 = ctx.mate[a], m1 = ctx.mate[b], m2 = ctx.mate[c];
			int key[3];
			// reversed winding order: (m2, m1, m0)
			sym_sorted_tri(m2, m1, m0, key);
			std::vector<int> k(key, key+3);
			if (!std::binary_search(face_list.begin(), face_list.end(), k))
				throw std::invalid_argument("symmetry: unpaired face");
		}
	}

	void simplify_mesh_symmetric(SymmetryContext &ctx, int target_count,
		double agressiveness, bool verbose, bool lossless)
	{
		int nv = (int)vertices.size();
		int nf = (int)triangles.size();

		// canonical face keys for the pairing check (throw before touching
		// any public state)
		std::vector<int> face_canon((size_t)nf*3);
		for (int f = 0; f < nf; ++f)
			sym_sorted_tri(triangles[f].v[0], triangles[f].v[1], triangles[f].v[2], &face_canon[(size_t)f*3]);
		{
			std::vector<std::vector<int> > sorted_faces(nf);
			for (int f = 0; f < nf; ++f)
				sorted_faces[f] = std::vector<int>(face_canon.begin()+(size_t)f*3, face_canon.begin()+(size_t)f*3+3);
			std::sort(sorted_faces.begin(), sorted_faces.end());
			sym_validate(ctx, face_canon);
			// duplicate detection
			for (int f = 1; f < nf; ++f)
				if (sorted_faces[f] == sorted_faces[f-1])
					throw std::invalid_argument("symmetry: duplicate face");
		}

		// ---- run state (mesh/context untouched so far) ----
		for (int i = 0; i < nf; ++i) triangles[i].deleted = 0;
		g_sym_ctx = &ctx;
		g_sym_cur_orig.resize(nv);
		g_sym_uf.resize(nv);
		for (int i = 0; i < nv; ++i) { g_sym_cur_orig[i] = i; g_sym_uf[i] = i; }
		ctx.original_to_vertex.assign(nv, -1);

		update_mesh(0); // builds border flags, quadrics, refs

		// The scheduling costs come from the original per-face plane quadrics
		// (no re-initialisation from compacted faces), evaluated on the whole
		// reflection orbit.
		for (size_t i = 0; i < triangles.size(); ++i)
		{
			Triangle &t = triangles[i];
			for (int j = 0; j < 3; ++j)
				t.err[j] = sym_edge_error(t.v[j], t.v[(j+1)%3]);
			t.err[3] = min(t.err[0], min(t.err[1], t.err[2]));
		}

		int deleted_triangles = 0;
		int triangle_count = (int)triangles.size();

		for (int iteration = 0; ; ++iteration)
		{
			if (!lossless && iteration >= 100) break;
			if (!lossless && triangle_count - deleted_triangles <= target_count) break;
			if (lossless && iteration >= 9999) break;

			// translate-only bookkeeping (triangle compaction and the
			// reference list); iteration 0 was already initialised above, and
			// later calls must not redo the initial plane quadrics
			if (!lossless)
			{
				if (iteration > 0 && iteration % 5 == 0) update_mesh(iteration);
			}
			else
			{
				if (iteration > 0) update_mesh(iteration);
				deleted_triangles = 0;
			}
			for (size_t i = 0; i < triangles.size(); ++i) triangles[i].dirty = 0;

			double threshold = lossless ? DBL_EPSILON
				: 0.000000001*pow(double(iteration+3), agressiveness);
			if (verbose)
				printf("symmetry %s %d - triangles %d threshold %g\n",
					lossless ? "lossless iteration" : "iteration", iteration,
					triangle_count - deleted_triangles, threshold);

			for (size_t i = 0; i < triangles.size(); ++i)
			{
				Triangle &t = triangles[i];
				if (t.deleted || t.dirty) continue;
				if (t.err[3] > threshold) continue;
				for (int j = 0; j < 3; ++j)
				{
					if (!(t.err[j] <= threshold)) continue;
					int a = t.v[j], b = t.v[(j+1)%3];
					if (try_symmetry_collapse(a, b, deleted_triangles))
						break;
				}
				if (!lossless && triangle_count - deleted_triangles <= target_count) break;
			}

			if (lossless)
			{
				if (deleted_triangles <= 0) break;
				deleted_triangles = 0;
			}
		}

		// periodic/terminal compaction; mate, source mapping and refs follow
		compact_mesh_symmetric();
		g_sym_ctx = 0;
	}

	// Check if a triangle flips when this edge is removed

	bool flipped(vec3f p,int i0,int i1,Vertex &v0,Vertex &v1,std::vector<int> &deleted)
	{

		loopk(0,v0.tcount)
		{
			Triangle &t=triangles[refs[v0.tstart+k].tid];
			if(t.deleted)continue;

			int s=refs[v0.tstart+k].tvertex;
			int id1=t.v[(s+1)%3];
			int id2=t.v[(s+2)%3];

			if(id1==i1 || id2==i1) // delete ?
			{

				deleted[k]=1;
				continue;
			}
			vec3f d1 = vertices[id1].p-p; d1.normalize();
			vec3f d2 = vertices[id2].p-p; d2.normalize();
			if(fabs(d1.dot(d2))>0.999) return true;
			vec3f n;
			n.cross(d1,d2);
			n.normalize();
			deleted[k]=0;
			if(n.dot(t.n)<0.2) return true;
		}
		return false;
	}

	// update_uvs

	void update_uvs(int i0,const Vertex &v,const vec3f &p,std::vector<int> &deleted)
	{
		loopk(0,v.tcount)
		{
			Ref &r=refs[v.tstart+k];
			Triangle &t=triangles[r.tid];
			if(t.deleted)continue;
			if(deleted[k])continue;
			vec3f p1=vertices[t.v[0]].p;
			vec3f p2=vertices[t.v[1]].p;
			vec3f p3=vertices[t.v[2]].p;
			t.uvs[r.tvertex] = interpolate(p,p1,p2,p3,t.uvs);
		}
	}

	// Update triangle connections and edge error after a edge is collapsed

	void update_triangles(int i0,Vertex &v,std::vector<int> &deleted,int &deleted_triangles)
	{
		vec3f p;
		loopk(0,v.tcount)
		{
			Ref &r=refs[v.tstart+k];
			Triangle &t=triangles[r.tid];
			if(t.deleted)continue;
			if(deleted[k])
			{
				t.deleted=1;
				deleted_triangles++;
				continue;
			}
			t.v[r.tvertex]=i0;
			t.dirty=1;
			t.err[0]=calculate_error(t.v[0],t.v[1],p);
			t.err[1]=calculate_error(t.v[1],t.v[2],p);
			t.err[2]=calculate_error(t.v[2],t.v[0],p);
			t.err[3]=min(t.err[0],min(t.err[1],t.err[2]));
			refs.push_back(r);
		}
	}

	// compact triangles, compute edge error and build reference list

	void update_mesh(int iteration)
	{
		if(iteration>0) // compact triangles
		{
			int dst=0;
			loopi(0,triangles.size())
			if(!triangles[i].deleted)
			{
				triangles[dst++]=triangles[i];
			}
			triangles.resize(dst);
		}
		//

		// Init Reference ID list
		loopi(0,vertices.size())
		{
			vertices[i].tstart=0;
			vertices[i].tcount=0;
		}
		loopi(0,triangles.size())
		{
			Triangle &t=triangles[i];
			loopj(0,3) vertices[t.v[j]].tcount++;
		}
		int tstart=0;
		loopi(0,vertices.size())
		{
			Vertex &v=vertices[i];
			v.tstart=tstart;
			tstart+=v.tcount;
			v.tcount=0;
		}

		// Write References
		refs.resize(triangles.size()*3);
		loopi(0,triangles.size())
		{
			Triangle &t=triangles[i];
			loopj(0,3)
			{
				Vertex &v=vertices[t.v[j]];
				refs[v.tstart+v.tcount].tid=i;
				refs[v.tstart+v.tcount].tvertex=j;
				v.tcount++;
			}
		}

		// Init Quadrics by Plane & Edge Errors
		//
		// required at the beginning ( iteration == 0 )
		// recomputing during the simplification is not required,
		// but mostly improves the result for closed meshes
		//
		if( iteration == 0 )
		{
			// Identify boundary : vertices[].border=0,1

			std::vector<int> vcount,vids;

			loopi(0,vertices.size())
				vertices[i].border=0;

			loopi(0,vertices.size())
			{
				Vertex &v=vertices[i];
				vcount.clear();
				vids.clear();
				loopj(0,v.tcount)
				{
					int k=refs[v.tstart+j].tid;
					Triangle &t=triangles[k];
					loopk(0,3)
					{
						int ofs=0,id=t.v[k];
						while(ofs<vcount.size())
						{
							if(vids[ofs]==id)break;
							ofs++;
						}
						if(ofs==vcount.size())
						{
							vcount.push_back(1);
							vids.push_back(id);
						}
						else
							vcount[ofs]++;
					}
				}
				loopj(0,vcount.size()) if(vcount[j]==1)
					vertices[vids[j]].border=1;
			}
			//initialize errors
			loopi(0,vertices.size())
				vertices[i].q=SymetricMatrix(0.0);

			loopi(0,triangles.size())
			{
				Triangle &t=triangles[i];
				vec3f n,p[3];
				loopj(0,3) p[j]=vertices[t.v[j]].p;
				n.cross(p[1]-p[0],p[2]-p[0]);
				n.normalize();
				t.n=n;
				loopj(0,3) vertices[t.v[j]].q =
					vertices[t.v[j]].q+SymetricMatrix(n.x,n.y,n.z,-n.dot(p[0]));
			}
			loopi(0,triangles.size())
			{
				// Calc Edge Error
				Triangle &t=triangles[i];vec3f p;
				loopj(0,3) t.err[j]=calculate_error(t.v[j],t.v[(j+1)%3],p);
				t.err[3]=min(t.err[0],min(t.err[1],t.err[2]));
			}
		}
	}

	// Finally compact mesh before exiting

	void compact_mesh()
	{
		int dst=0;
		loopi(0,vertices.size())
		{
			vertices[i].tcount=0;
		}
		loopi(0,triangles.size())
		if(!triangles[i].deleted)
		{
			Triangle &t=triangles[i];
			triangles[dst++]=t;
			loopj(0,3)vertices[t.v[j]].tcount=1;
		}
		triangles.resize(dst);
		dst=0;
		loopi(0,vertices.size())
		if(vertices[i].tcount)
		{
			vertices[i].tstart=dst;
			vertices[dst].p=vertices[i].p;
			dst++;
		}
		loopi(0,triangles.size())
		{
			Triangle &t=triangles[i];
			loopj(0,3)t.v[j]=vertices[t.v[j]].tstart;
		}
		vertices.resize(dst);
	}

	// Error between vertex and Quadric

	double vertex_error(SymetricMatrix q, double x, double y, double z)
	{
 		return   q[0]*x*x + 2*q[1]*x*y + 2*q[2]*x*z + 2*q[3]*x + q[4]*y*y
 		     + 2*q[5]*y*z + 2*q[6]*y + q[7]*z*z + 2*q[8]*z + q[9];
	}

	// Error for one edge

	double calculate_error(int id_v1, int id_v2, vec3f &p_result)
	{
		// compute interpolated vertex

		SymetricMatrix q = vertices[id_v1].q + vertices[id_v2].q;
		bool border = vertices[id_v1].border & vertices[id_v2].border;
		double error=0;
		double det = q.det(0, 1, 2, 1, 4, 5, 2, 5, 7);
		if ( det != 0 && !border )
		{

			// q_delta is invertible
			p_result.x = -1/det*(q.det(1, 2, 3, 4, 5, 6, 5, 7 , 8));	// vx = A41/det(q_delta)
			p_result.y =  1/det*(q.det(0, 2, 3, 1, 5, 6, 2, 7 , 8));	// vy = A42/det(q_delta)
			p_result.z = -1/det*(q.det(0, 1, 3, 1, 4, 6, 2, 5,  8));	// vz = A43/det(q_delta)

			error = vertex_error(q, p_result.x, p_result.y, p_result.z);
		}
		else
		{
			// det = 0 -> try to find best result
			vec3f p1=vertices[id_v1].p;
			vec3f p2=vertices[id_v2].p;
			vec3f p3=(p1+p2)/2;
			double error1 = vertex_error(q, p1.x,p1.y,p1.z);
			double error2 = vertex_error(q, p2.x,p2.y,p2.z);
			double error3 = vertex_error(q, p3.x,p3.y,p3.z);
			error = min(error1, min(error2, error3));
			if (error1 == error) p_result=p1;
			if (error2 == error) p_result=p2;
			if (error3 == error) p_result=p3;
		}
		return error;
	}

	char *trimwhitespace(char *str)
	{
		char *end;

		// Trim leading space
		while(isspace((unsigned char)*str)) str++;

		if(*str == 0)  // All spaces?
		return str;

		// Trim trailing space
		end = str + strlen(str) - 1;
		while(end > str && isspace((unsigned char)*end)) end--;

		// Write new null terminator
		*(end+1) = 0;

		return str;
	}

	//Option : Load OBJ
	void load_obj(const char* filename, bool process_uv=false){
		vertices.clear();
		triangles.clear();
		//printf ( "Loading Objects %s ... \n",filename);
		FILE* fn;
		if(filename==NULL)		return ;
		if((char)filename[0]==0)	return ;
		if ((fn = fopen(filename, "rb")) == NULL)
		{
			printf ( "File %s not found!\n" ,filename );
			return;
		}
		char line[1000];
		memset ( line,0,1000 );
		int vertex_cnt = 0;
		int material = -1;
		std::map<std::string, int> material_map;
		std::vector<vec3f> uvs;
		std::vector<std::vector<int> > uvMap;

		while(fgets( line, 1000, fn ) != NULL)
		{
			Vertex v;
			vec3f uv;

			if (strncmp(line, "mtllib", 6) == 0)
			{
				mtllib = trimwhitespace(&line[7]);
			}
			if (strncmp(line, "usemtl", 6) == 0)
			{
				std::string usemtl = trimwhitespace(&line[7]);
				if (material_map.find(usemtl) == material_map.end())
				{
					material_map[usemtl] = materials.size();
					materials.push_back(usemtl);
				}
				material = material_map[usemtl];
			}

			if ( line[0] == 'v' && line[1] == 't' )
			{
				if ( line[2] == ' ' )
				{
					if(sscanf(line,"vt %lf %lf",
						&uv.x,&uv.y)==2)
					{
						uv.z = 0;
						uvs.push_back(uv);
					} else
					if(sscanf(line,"vt %lf %lf %lf",
						&uv.x,&uv.y,&uv.z)==3)
					{
						uvs.push_back(uv);
					}
				}
			}
			else if ( line[0] == 'v' )
			{
				if ( line[1] == ' ' )
				if(sscanf(line,"v %lf %lf %lf",
					&v.p.x,	&v.p.y,	&v.p.z)==3)
				{
					vertices.push_back(v);
				}
			}
			int integers[9];
			if ( line[0] == 'f' )
			{
				Triangle t;
				bool tri_ok = false;
				bool has_uv = false;

				if(sscanf(line,"f %d %d %d",
					&integers[0],&integers[1],&integers[2])==3)
				{
					tri_ok = true;
				}else
				if(sscanf(line,"f %d// %d// %d//",
					&integers[0],&integers[1],&integers[2])==3)
				{
					tri_ok = true;
				}else
				if(sscanf(line,"f %d//%d %d//%d %d//%d",
					&integers[0],&integers[3],
					&integers[1],&integers[4],
					&integers[2],&integers[5])==6)
				{
					tri_ok = true;
				}else
				if(sscanf(line,"f %d/%d/%d %d/%d/%d %d/%d/%d",
					&integers[0],&integers[6],&integers[3],
					&integers[1],&integers[7],&integers[4],
					&integers[2],&integers[8],&integers[5])==9)
				{
					tri_ok = true;
					has_uv = true;
				}else // Add Support for v/vt only meshes
				if (sscanf(line, "f %d/%d %d/%d %d/%d",
					&integers[0], &integers[6],
					&integers[1], &integers[7],
					&integers[2], &integers[8]) == 6)
				{
					tri_ok = true;
					has_uv = true;
				}
				else
				{
					printf("unrecognized sequence\n");
					printf("%s\n",line);
					exit(1);
				}
				if ( tri_ok )
				{
					t.v[0] = integers[0]-1-vertex_cnt;
					t.v[1] = integers[1]-1-vertex_cnt;
					t.v[2] = integers[2]-1-vertex_cnt;
					t.attr = 0;

					if ( process_uv && has_uv )
					{
						std::vector<int> indices;
						indices.push_back(integers[6]-1-vertex_cnt);
						indices.push_back(integers[7]-1-vertex_cnt);
						indices.push_back(integers[8]-1-vertex_cnt);
						uvMap.push_back(indices);
						t.attr |= TEXCOORD;
					}

					t.material = material;
					//geo.triangles.push_back ( tri );
					triangles.push_back(t);
					//state_before = state;
					//state ='f';
				}
			}
		}
		if ( process_uv && uvs.size() )
		{
			loopi(0,triangles.size())
			{
				loopj(0,3)
				triangles[i].uvs[j] = uvs[uvMap[i][j]];
			}
		}

		fclose(fn);

		//printf("load_obj: vertices = %lu, triangles = %lu, uvs = %lu\n", vertices.size(), triangles.size(), uvs.size() );
	} // load_obj()

	// Optional : Store as OBJ

	void write_obj(const char* filename)
	{
		FILE *file=fopen(filename, "w");
		int cur_material = -1;
		bool has_uv = (triangles.size() && (triangles[0].attr & TEXCOORD) == TEXCOORD);

		if (!file)
		{
			printf("write_obj: can't write data file \"%s\".\n", filename);
			exit(0);
		}
		if (!mtllib.empty())
		{
			fprintf(file, "mtllib %s\n", mtllib.c_str());
		}
		loopi(0,vertices.size())
		{
			//fprintf(file, "v %lf %lf %lf\n", vertices[i].p.x,vertices[i].p.y,vertices[i].p.z);
			fprintf(file, "v %g %g %g\n", vertices[i].p.x,vertices[i].p.y,vertices[i].p.z); //more compact: remove trailing zeros
		}
		if (has_uv)
		{
			loopi(0,triangles.size()) if(!triangles[i].deleted)
			{
				fprintf(file, "vt %g %g\n", triangles[i].uvs[0].x, triangles[i].uvs[0].y);
				fprintf(file, "vt %g %g\n", triangles[i].uvs[1].x, triangles[i].uvs[1].y);
				fprintf(file, "vt %g %g\n", triangles[i].uvs[2].x, triangles[i].uvs[2].y);
			}
		}
		int uv = 1;
		loopi(0,triangles.size()) if(!triangles[i].deleted)
		{
			if (triangles[i].material != cur_material)
			{
				cur_material = triangles[i].material;
				fprintf(file, "usemtl %s\n", materials[triangles[i].material].c_str());
			}
			if (has_uv)
			{
				fprintf(file, "f %d/%d %d/%d %d/%d\n", triangles[i].v[0]+1, uv, triangles[i].v[1]+1, uv+1, triangles[i].v[2]+1, uv+2);
				uv += 3;
			}
			else
			{
				fprintf(file, "f %d %d %d\n", triangles[i].v[0]+1, triangles[i].v[1]+1, triangles[i].v[2]+1);
			}
			//fprintf(file, "f %d// %d// %d//\n", triangles[i].v[0]+1, triangles[i].v[1]+1, triangles[i].v[2]+1); //more compact: remove trailing zeros
		}
		fclose(file);
	}

	#ifdef _MSC_VER
	#pragma pack(2)
	struct mz3hdr {
		uint16_t SIGNATURE, ATTR;
		uint32_t NFACE, NVERT, NSKIP;
	};
	#pragma pack()	
	#else
	struct __attribute__((__packed__)) mz3hdr {
		uint16_t SIGNATURE, ATTR;
		uint32_t NFACE, NVERT, NSKIP;
	};
	#endif

	void load_mz3(const char* filename) {
		vertices.clear();
		triangles.clear();
		int material = -1;
		std::map<std::string, int> material_map;
		std::vector<vec3f> uvs;
		std::vector<std::vector<int> > uvMap;
		FILE *fp = fopen(filename,"rb");
		struct mz3hdr h;
		size_t bytes_read = fread(&h, sizeof(struct mz3hdr), 1, fp);
		if (bytes_read <= 0) {
			printf("Unable to read %s\n", filename);
			exit(EXIT_FAILURE);
		}
		uint16_t sig = 23117;
		if (sig != h.SIGNATURE) {
			fclose(fp);
			printf("Unable to read mz3 (unable to read gz compressed) %s\n", filename);
			exit(EXIT_FAILURE);
		}
		triangles.resize(h.NFACE);
		vertices.resize(h.NVERT);
		fseek(fp, (int)(sizeof(struct mz3hdr) + h.NSKIP), SEEK_SET);
		uint32_t tribytes = h.NFACE * 3 * sizeof(uint32_t);
		uint32_t *tris = (uint32_t *) malloc(tribytes);
		bytes_read = fread(tris, tribytes, 1, fp);
		if (bytes_read <= 0) {
			printf("Unable to read triangles %s\n", filename);
			exit(EXIT_FAILURE);
		}
		uint32_t vertbytes32 = h.NVERT * 3 * sizeof(float);
		float * verts32 = (float *) malloc(vertbytes32);
		bytes_read = fread(verts32, vertbytes32, 1, fp);
		if (bytes_read <= 0) {
			printf("Unable to read vertices %s\n", filename);
			exit(EXIT_FAILURE);
		}
		int j = 0;
		for (int i = 0; i < h.NVERT; i++) {
			vertices[i].p.x = verts32[j++];
			vertices[i].p.y = verts32[j++];
			vertices[i].p.z = verts32[j++];
		}
		j = 0;
		for (int i = 0; i < h.NFACE; i++) {
			triangles[i].v[0] = tris[j++];
			triangles[i].v[1] = tris[j++];
			triangles[i].v[2] = tris[j++];
			triangles[i].attr = 0;
			triangles[i].material = material;
		}
		free(tris);
		free(verts32);
		fclose(fp);
	}
	
	void write_mz3(const char* filename) {
		// https://github.com/neurolabusc/surf-ice/tree/master/mz3
		bool has_uv = (triangles.size() && (triangles[0].attr & TEXCOORD) == TEXCOORD);
		if (has_uv) {
			printf("write_mz3 can not store UVs (use .obj format).\n");
		}
		struct mz3hdr h;
		h.SIGNATURE = 0x5A4D;
		h.ATTR = 3;//isFACE +1 isVERT +2
		h.NVERT = vertices.size();
		h.NSKIP = 0;
		h.NFACE = 0;
		for (int i = 0; i < triangles.size(); i++) {
			if(!triangles[i].deleted)
				h.NFACE++;
		}
		if ((h.NFACE < 1) || (h.NVERT < 3)) {
			printf("write_mz3: no surviving triangles.\n");
			exit(1);
		}
		FILE *fp;
		fp = fopen(filename, "wb");
		if (fp == NULL) {
			printf("write_mz3: can't write data file \"%s\".\n", filename);
			exit(1);
		}
		fwrite(&h, sizeof(struct mz3hdr), 1, fp);
		uint32_t tribytes = h.NFACE * 3 * sizeof(uint32_t);
		uint32_t *tris = (uint32_t *) malloc(tribytes);
		int j = 0;
		for (int i = 0; i < triangles.size(); i++) {
			if(triangles[i].deleted)
				continue;
			tris[j++] = triangles[i].v[0];
			tris[j++] = triangles[i].v[1];
			tris[j++] = triangles[i].v[2];
		}
		fwrite(tris, tribytes, 1, fp);
		free(tris);
		uint32_t vertbytes32 = h.NVERT * 3 * sizeof(float);
		float * verts32 = (float *) malloc(vertbytes32);
		j = 0;
		for (int i = 0; i < h.NVERT; i++) {
			verts32[j++] = vertices[i].p.x;
			verts32[j++] = vertices[i].p.y;
			verts32[j++] = vertices[i].p.z;
		}
		fwrite(verts32, vertbytes32, 1, fp);
		free(verts32);
		fclose(fp);
	}
};

///////////////////////////////////////////
