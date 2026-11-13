#include <cal3d/trisort.h>
#include <cal3d/physique.h>
#include <cal3d/submesh.h>
#include <cal3d/coremorphtarget.h>
#include <cal3d/matrix.h>
#include <iostream>
#include <stdexcept>
#include <cmath>
static void require(bool b) { if(!b) throw std::runtime_error("baseline mismatch"); }
int main() {
    // Same XY footprint, but opposite slopes: z_A=x and z_B=-x.
    // Two interior rays at (+/-.25,0) require opposite triangle orders.
    float p[]={-1,-1,-1, 1,-1,1, 0,1,0, -1,-1,1, 1,-1,-1, 0,1,0};
    unsigned short idx[]={0,1,2,3,4,5};
    sortTrianglesTopologically(2,idx,p);
    int first=idx[0]/3;
    int conflicts=0;
    for (float x: {-0.25f,0.25f}) {
        // Eye is at positive Z. Smaller z must be drawn first.
        int far = x<0 ? 0 : 1;
        conflicts += first!=far;
    }
    require(conflicts==1);
    require(idx[0]<6 && idx[3]<6 && idx[0]/3!=idx[3]/3);
    auto core=ISTD::make_shared<CalCoreSubmesh>(3,true,1);
    for(int i=0;i<3;++i) {
        CalCoreSubmesh::Vertex v;
        v.position=CalPoint4(p[i*3],p[i*3+1],p[i*3+2]);
        v.normal=CalVector4(0,0,1);
        core->addVertex(v,0xff443322,{CalCoreSubmesh::Influence(0,.25f,false),CalCoreSubmesh::Influence(1,.75f,true)});
        core->setTextureCoordinate(i,CalCoreSubmesh::TextureCoordinate(float(i),float(i*i)));
    }
    core->addFace(CalCoreSubmesh::Face(0,1,2));
    CalCoreMorphTarget::VertexOffsetArray offsets;
    offsets.push_back(VertexOffset(0,CalPoint4(.5f,0,0),CalVector4(0,0,0)));
    core->addMorphTarget(ISTD::make_shared<CalCoreMorphTarget>("bend",3,offsets));
    CalSubmesh sub(core); sub.setMorphTargetWeight("bend",.5f);
    BoneTransform bones[]={BoneTransform(CalMatrix(),CalVector(0,0,2)),BoneTransform(CalMatrix(),CalVector(0,0,-2))};
    CalVector4 out[6];
    CalPhysique::calculateVerticesAndNormals(bones,&sub,&out[0].x);
    require(std::fabs(out[0].x+.75f)<1e-6 && std::fabs(out[0].z+2)<1e-6);
    require(core->getFaceCount()==1 && core->getVectorVertex()[0].position.x==-1);
    std::cout<<"BASELINE_OK conflicts=1 morph_skin=PASS source_unchanged=PASS\n";
}
