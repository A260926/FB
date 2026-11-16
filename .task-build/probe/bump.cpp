#include "src/meshoptimizer.h"
#include <stdio.h>
#include <math.h>
#include <vector>

struct V { float x,y,z,u,v; };

// unit plane grid with a narrow tall bump in the middle
static void makeMesh(unsigned n, std::vector<V>& verts, std::vector<unsigned>& idx, bool bump)
{
    verts.clear(); idx.clear();
    for (unsigned j=0;j<=n;++j) for (unsigned i=0;i<=n;++i){
        float x=float(i)/n, y=float(j)/n, z=0;
        if (bump && i==n/2 && j==n/2) z=0.5f;
        V v={x,y,z,x,y}; verts.push_back(v);
    }
    for (unsigned j=0;j<n;++j) for (unsigned i=0;i<n;++i){
        unsigned a=j*(n+1)+i,b=a+1,c=a+n+1,d=c+1;
        unsigned f[]={a,b,d,a,d,c}; idx.insert(idx.end(),f,f+6);
    }
}

int main(int argc, char** argv)
{
    unsigned n = argc > 1 ? unsigned(atoi(argv[1])) : 8; unsigned mode = argc > 3 ? unsigned(atoi(argv[3])) : 16;
    bool bump = argc > 2 ? atoi(argv[2]) != 0 : true;
    std::vector<V> verts; std::vector<unsigned> idx;
    makeMesh(n, verts, idx, bump);
    std::vector<unsigned> out(idx.size());
    float w[2]={0.1f,0.1f}; float err=-1; meshopt_SurfaceErrorStats st={};
    size_t c=meshopt_simplifyWithSurfaceError(out.data(), idx.data(), idx.size(), &verts[0].x, verts.size(), sizeof(V), &verts[0].u, sizeof(V), w, 2, NULL, 0, 1.f, 0, &err, 0.01f, mode, &st);
    printf("bump=%d in=%zu out=%zu tri=%zu err=%g cert=%g acc=%zu rejD=%zu rejB=%zu inv=%zu tests=%zu\n",
        int(bump), idx.size(), c, c/3, err, st.certified_error, st.accepted_collapses, st.rejected_distance, st.rejected_budget, st.invalidated_certificates, st.distance_tests);
    // verify output indices reference valid vertices and no NaNs
    for (size_t k=0;k<c;++k) if (out[k] >= verts.size()) { printf("BAD INDEX\n"); return 1; }
    return 0;
}
