#include "src/meshoptimizer.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
void runTests();
struct Vertex {float x,y,z,u,v;};
int main(){
 runTests();
 const unsigned int n=16;
 std::vector<Vertex> vertices;
 std::vector<unsigned int> indices;
 for(unsigned int j=0;j<=n;++j)for(unsigned int i=0;i<=n;++i){Vertex v={float(i)/n,float(j)/n,0,float(i)/n,float(j)/n};vertices.push_back(v);}
 for(unsigned int j=0;j<n;++j)for(unsigned int i=0;i<n;++i){unsigned int a=j*(n+1)+i,b=a+1,c=a+n+1,d=c+1;unsigned int f[]={a,b,d,a,d,c};indices.insert(indices.end(),f,f+6);}
 std::vector<unsigned int> out(indices.size());
 const float weights[]={0.1f,0.1f};float error=-1;
 size_t count=meshopt_simplifyWithAttributes(out.data(),indices.data(),indices.size(),&vertices[0].x,vertices.size(),sizeof(Vertex),&vertices[0].u,sizeof(Vertex),weights,2,NULL,128*3,1,meshopt_SimplifyLockBorder|meshopt_SimplifyErrorAbsolute,&error);
 assert(count>0&&count<indices.size()&&count%3==0&&std::isfinite(error));
 for(size_t k=0;k<count;k+=3){for(int q=0;q<3;++q)assert(out[k+q]<vertices.size());const Vertex &a=vertices[out[k]],&b=vertices[out[k+1]],&c=vertices[out[k+2]];assert((b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x)>0);}
 std::printf("MESHOPT_COUNTS input_triangles=%zu output_triangles=%zu qem_error=%.9g\n",indices.size()/3,count/3,error);
 std::puts("MESHOPT_BASELINE production_simplify=ok original_tests=ok");return 0;
}
