#include "../source/xatlas/xatlas.cpp"
#include <cstdio>
#include <cmath>
int main(){using namespace xatlas::internal;auto*c=opennl::nlNewContext();opennl::nlSolverParameteri(c,NL_NB_VARIABLES,3);opennl::nlBegin(c,NL_SYSTEM);opennl::nlSetVariable(c,2,1);opennl::nlLockVariable(c,2);opennl::nlBegin(c,NL_MATRIX);
 double rows[2][3]={{1,1,-3},{1,-1,1}};for(auto&r:rows){opennl::nlBegin(c,NL_ROW);for(unsigned j=0;j<3;++j)opennl::nlCoefficient(c,j,r[j]);opennl::nlEnd(c,NL_ROW);}opennl::nlEnd(c,NL_MATRIX);opennl::nlEnd(c,NL_SYSTEM);if(!opennl::nlSolve(c)||fabs(opennl::nlGetVariable(c,0)-1)>1e-8||fabs(opennl::nlGetVariable(c,1)-2)>1e-8)return 1;opennl::nlDeleteContext(c);
 Mesh m(1e-6f,6,4);for(unsigned y=0;y<2;++y)for(unsigned x=0;x<3;++x)m.addVertex(Vector3(float(x),float(y),.3f*x*x),Vector3(0),Vector2(float(x),float(y)));for(unsigned x=0;x<2;++x){uint32_t a[]={x,x+1,x+3},b[]={x+1,x+4,x+3};m.addFace(a);m.addFace(b);}m.createColocals();m.createBoundaries();if(!param::computeLeastSquaresConformalMap(&m))return 2;double area=0;for(unsigned f=0;f<4;++f)area+=fabs(m.computeFaceParametricArea(f));if(!(area>0))return 3;for(unsigned v=0;v<6;++v)if(!isFinite(m.texcoord(v)))return 4;
 std::puts("XATLAS_BASELINE locked-ls=x1,y2 lscm=6vertices,4faces finite=ok");}
