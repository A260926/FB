#include "solvespace.h"
#include <cmath>
#include <cstdio>
#include <stdexcept>
using namespace SolveSpace;
static void need(bool x,const char *s){if(!x)throw std::runtime_error(s);}
static void plane(SShell &sh,uint32_t id,bool broken=false) {
 SSurface s=SSurface::FromPlane({0,0,double(id)},{1,0,0},{0,1,0});s.h={id};s.face=id;s.color=RGBi(70,80,90);
 Vector ps[4]={{0,0,double(id)},{1,0,double(id)},{1,1,double(id)},{0,1,double(id)}};
 for(int i=0;i<(broken?3:4);++i){SCurve c={};c.h={id*10+unsigned(i)+1};c.isExact=true;c.exact=SBezier::From(ps[i],ps[(i+1)%4]);c.surfA=s.h;SCurvePt a={0,ps[i],true},b={0,ps[(i+1)%4],true};c.pts.Add(&a);c.pts.Add(&b);sh.curve.Add(&c);STrimBy t=STrimBy::EntireCurve(&sh,c.h,false);s.trim.Add(&t);}sh.surface.Add(&s);
}
int main(){try{
 SS.chordTolCalculated=.001;SS.maxSegments=128;SS.exportScale=1;SS.exportChordTol=.001;SS.exportMaxSegments=128;
 SShell sh={};plane(sh,1);SMesh m={};sh.TriangulateInto(&m);need(m.l.n==2,"plane triangle count");double area=0;for(auto &t:m.l)area+=t.Area();need(std::abs(area-1)<1e-9,"plane coverage");
 FILE *f=std::fopen(".task-build/normal.obj","wb"),*mt=std::fopen(".task-build/normal.mtl","wb");need(f&&mt,"output open");SS.ExportMeshAsObjTo(f,mt,&m);std::fclose(f);std::fclose(mt);std::puts("PRODUCTION_PLANE_OBJ_OK");
 SShell bad={};plane(bad,1);plane(bad,2,true);SMesh partial={};bad.TriangulateInto(&partial);std::printf("OBS_OLD_PARTIAL_SHELL=%d\n",partial.l.n);
 SSurface curved={};curved.degm=2;curved.degn=2;for(int i=0;i<3;i++)for(int j=0;j<3;j++){curved.ctrl[i][j]={i/2.,j/2.,(i==2&&j==2)?4.:0.};curved.weight[i][j]=1.;}
 SS.maxSegments=1;List<double> grid={};double zero=0;grid.Add(&zero);curved.MakeTriangulationGridInto(&grid,0,1,false,0);double worst=0;for(int k=1;k<grid.n;k++){double a=grid[k-1],b=grid[k];for(int i=0;i<4;i++){double u=i/3.;Vector p=curved.PointAt(u,a),q=curved.PointAt(u,b);for(int j=1;j<3;j++){auto x=curved.PointAt(u,a+(b-a)*j/3.);worst=std::max(worst,x.DistanceToLine(p,q.Minus(p)));}}}std::printf("OBS_OLD_GRID_MAX_ERROR=%.9g TOL=%.9g\n",worst,SS.chordTolCalculated);
 Group g={};g.h={1};g.type=Group::Type::EXTRUDE;g.displayDirty=true;g.runningShell=bad;g.thisShell.surface.Add(&bad.surface[0]);g.displayMesh.MakeFromCopyOf(&m);STriangle sentinel=m.l[0];sentinel.a.x+=10;sentinel.b.x+=10;sentinel.c.x+=10;sentinel.meta.face=999;g.displayMesh.AddTriangle(&sentinel);SK.group.Add(&g);hGroup gh={1};SK.groupOrder.Add(&gh);SS.GW.activeGroup=gh;auto *gp=SK.GetGroup(gh);gp->GenerateDisplayItems();std::printf("OBS_OLD_GROUP_DIRTY=%d MESH=%d BEFORE=3\n",gp->displayDirty,gp->displayMesh.l.n);
 FILE *old=std::fopen(".task-build/rejected.stl","wb");need(old,"sentinel output");std::fputs("KEEP_ME",old);std::fclose(old);gp->displayDirty=true;SS.exportMode=false;SS.ExportMeshTo(Platform::Path::From(".task-build/rejected.stl"));FILE *rejected=std::fopen(".task-build/rejected.stl","rb");need(rejected,"reopen rejected");std::fseek(rejected,0,SEEK_END);std::printf("OBS_OLD_FAILED_EXPORT_BYTES=%ld BEFORE=7\n",std::ftell(rejected));std::fclose(rejected);
 gp->runningShell=sh;gp->displayDirty=true;SS.maxSegments=128;SS.exportMode=false;SS.ExportMeshTo(Platform::Path::From(".task-build/production.stl"));FILE *stl=std::fopen(".task-build/production.stl","rb");need(stl,"production export exists");std::fseek(stl,0,SEEK_END);need(std::ftell(stl)>84,"production STL nonempty");std::fclose(stl);std::printf("OBS_OLD_EXPORT_MODE=%d\n",SS.exportMode);std::puts("PRODUCTION_GROUP_EXPORT_STL_OK");
 std::puts("BASELINE_NORMAL_PATH_COMPLETE");return 0;
 }catch(const std::exception &e){std::fprintf(stderr,"BASELINE_ERROR %s\n",e.what());return 1;}}
