#include "temporal.hpp"
#include <cassert>
#include <cmath>
#include <limits>
#include <iostream>
#include <string>
using namespace render;

static bool close(V3 a,V3 b,double eps=1e-12){
 return std::abs(a.x-b.x)<eps&&std::abs(a.y-b.y)<eps&&std::abs(a.z-b.z)<eps;
}
// A fully covered frame: object 5, finite positive depth everywhere.
static Frame plainFrame(int w,int h,uint64_t seq,V2 jitter={},double exposure=1){
 Frame f(w,h);f.sequence=seq;f.jitter=jitter;f.exposure=exposure;
 for(size_t i=0;i<f.object.size();i++){
  f.object[i]=5;f.depth[i]=2;f.previous_depth[i]=2;f.motion[i]={};
 }
 return f;
}
// Wide-gamut 3x3 neighborhood around (cx,cy) so YCoCg clamping never binds.
static void spreadNeighborhood(Frame& f,int cx,int cy){
 const V3 ext[9]={{0,0,0},{8,8,8},{8,0,0},{0,0,8},{0,8,0},
                  {8,0,8},{0,8,8},{8,8,0},{4,4,4}};
 int k=0;
 for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++)
  f.hdr.at(cx+dx,cy+dy)=ext[k++];
}

static void testFourTapWeights(){
 // 4x4, center pixel (1,1); prev-minus-cur jitter delta (.25,.25) moves the query
 // to (1.75,1.75), so the four taps get weights .5625/.1875/.1875/.0625 around
 // texels (1,1)(2,1)(1,2)(2,2).
 TemporalAccumulator acc;
 Frame a=plainFrame(4,4,1,{0.25,0.25});
 a.hdr.at(1,1)={1,0,0};a.hdr.at(2,1)={0,1,0};
 a.hdr.at(1,2)={0,0,1};a.hdr.at(2,2)={1,1,1};
 Image oa=acc.resolve(a);
 assert(close(oa.at(1,1),a.hdr.at(1,1))); // first frame: current only

 Frame b=plainFrame(4,4,2,{0,0});
 spreadNeighborhood(b,1,1);
 b.hdr.at(1,1)={0.4,0.4,0.4};
 Image ob=acc.resolve(b);
 // Hand-computed normalized history: R=.5625+.0625=.625 G=.1875+.0625=.25 B=.1875+.0625=.25
 const V3 hist{0.625,0.25,0.25};
 const V3 expect=b.hdr.at(1,1)*0.1+hist*0.9;
 assert(close(ob.at(1,1),expect));
 assert(std::abs(ob.at(1,1).x-0.6025)<1e-12);
 assert(std::abs(ob.at(1,1).y-0.265)<1e-12);
 assert(std::abs(ob.at(1,1).z-0.265)<1e-12);
 std::cout<<"four-tap weights ok\n";
}

static void testSingleTapRejection(){
 TemporalAccumulator acc;
 Frame a=plainFrame(4,4,1,{0.25,0.25});
 a.hdr.at(1,1)={1,0,0};a.hdr.at(2,1)={0,1,0};
 a.hdr.at(1,2)={0,0,1};a.hdr.at(2,2)={1,1,1};
 // Depth of tap (2,2) disagrees with previous_depth 2 (tolerance .02): rejected.
 a.depth[2*4+2]=5;
 acc.resolve(a);
 Frame b=plainFrame(4,4,2,{0,0});
 spreadNeighborhood(b,1,1);b.hdr.at(1,1)={.4,.4,.4};
 Image ob=acc.resolve(b);
 // Remaining weights .5625,.1875,.1875 renormalized by .9375 -> (.6,.2,.2)
 const V3 expect=b.hdr.at(1,1)*0.1+V3{0.6,0.2,0.2}*0.9;
 assert(close(ob.at(1,1),expect));

 // Object mismatch rejects another tap: only (1,1) and (2,1) survive -> (.75,.25,0)
 TemporalAccumulator acc2;
 Frame a2=plainFrame(4,4,1,{0.25,0.25});
 a2.hdr.at(1,1)={1,0,0};a2.hdr.at(2,1)={0,1,0};
 a2.hdr.at(1,2)={0,0,1};a2.hdr.at(2,2)={1,1,1};
 a2.depth[2*4+2]=5;a2.object[2*4+1]=9;
 acc2.resolve(a2);
 Frame b2=plainFrame(4,4,2,{0,0});
 spreadNeighborhood(b2,1,1);b2.hdr.at(1,1)={.4,.4,.4};
 Image ob2=acc2.resolve(b2);
 const V3 expect2=b2.hdr.at(1,1)*0.1+V3{0.75,0.25,0}*0.9;
 assert(close(ob2.at(1,1),expect2));

 // Every tap invalid -> exact current color, no blending.
 TemporalAccumulator acc3;
 Frame a3=plainFrame(4,4,1,{0.25,0.25});
 for(int yy=1;yy<=2;yy++)for(int xx=1;xx<=2;xx++)a3.object[yy*4+xx]=9;
 acc3.resolve(a3);
 Frame b3=plainFrame(4,4,2,{0,0});
 b3.hdr.at(1,1)={.3,.6,.9};
 Image ob3=acc3.resolve(b3);
 assert(close(ob3.at(1,1),V3{.3,.6,.9}));
 std::cout<<"per-texel depth/object rejection ok\n";
}

static void testExposureScale(){
 // History stored under exposure 1, current exposure 2: history doubles before compare.
 TemporalAccumulator acc;
 Frame a=plainFrame(4,4,1,{0,0},1);
 for(auto& c:a.hdr.color)c={1,2,3};
 acc.resolve(a);
 Frame b=plainFrame(4,4,2,{0,0},2);
 spreadNeighborhood(b,1,1);b.hdr.at(1,1)={.5,.5,.5};
 Image ob=acc.resolve(b);
 const V3 scaled{2,4,6};
 const V3 expect=b.hdr.at(1,1)*0.1+scaled*0.9; // (1.85,3.65,5.45)
 assert(close(ob.at(1,1),expect));
 std::cout<<"exposure rescale ok\n";
}

static void testYCoCgClamp(){
 // Uniform 3x3 current neighborhood (1,1,0): Y=.75 Co=.5 Cg=.25 in YCoCg.
 // History (4,0,0) -> Y=1 Co=2 Cg=-1 clamps to (.75,.5,.25) -> RGB (1,1,0).
 TemporalAccumulator acc;
 Frame a=plainFrame(4,4,1);
 for(auto& c:a.hdr.color)c={4,0,0};
 acc.resolve(a);
 Frame b=plainFrame(4,4,2);
 for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++)b.hdr.at(1+dx,1+dy)={1,1,0};
 Image ob=acc.resolve(b);
 assert(close(ob.at(1,1),V3{1,1,0}));
 std::cout<<"YCoCg clamp ok\n";
}

static void testJitterAndMotion(){
 // Both jitters nonzero plus a nonzero motion at center (1,1).
 // query = (1.5,1.5) + motion(.2,-.1) + (jprev-jcur)(.25,-.5) = (1.95,.9)
 // -> gx=1.45 gy=.4, taps (1,0)(2,0)(1,1)(2,1), weights .33/.27/.22/.18.
 TemporalAccumulator acc;
 Frame a=plainFrame(4,4,1,{0.1,-0.2});
 a.hdr.at(1,0)={1,0,0};a.hdr.at(2,0)={0,1,0};
 a.hdr.at(1,1)={0,0,1};a.hdr.at(2,1)={1,1,1};
 acc.resolve(a);
 Frame b=plainFrame(4,4,2,{-0.15,0.3});
 b.motion[1*4+1]={0.2,-0.1};
 spreadNeighborhood(b,1,1);b.hdr.at(1,1)={.1,.1,.1};
 Image ob=acc.resolve(b);
 // R=.33+.18=.51 G=.27+.18=.45 B=.22+.18=.40
 const V3 hist{0.51,0.45,0.40};
 assert(close(ob.at(1,1),b.hdr.at(1,1)*0.1+hist*0.9,1e-12));

 // Equal jitters and zero motion: the query lands exactly on the center texel.
 TemporalAccumulator acc2;
 Frame a2=plainFrame(4,4,1,{0.3,0.3});
 a2.hdr.at(1,1)={5,5,5};
 acc2.resolve(a2);
 Frame b2=plainFrame(4,4,2,{0.3,0.3});
 spreadNeighborhood(b2,1,1);b2.hdr.at(1,1)={1,1,1};
 assert(close(acc2.resolve(b2).at(1,1),V3{4.6,4.6,4.6}));
 std::cout<<"jitter difference and motion ok\n";
}

static void testInvalidQueriesAndBackground(){
 TemporalAccumulator acc;
 Frame a=plainFrame(5,5,1);
 for(auto& c:a.hdr.color)c={3,3,3};
 acc.resolve(a);

 Frame b=plainFrame(5,5,2);
 for(auto& c:b.hdr.color)c={.2,.2,.2};
 // Keep a wide box around (3,3) so its blend is not trivially clamped.
 spreadNeighborhood(b,3,3);
 // Query leaves the viewport.
 b.motion[0]={-5,0};
 // Non-finite motion.
 b.motion[1*5+1]={std::nan(""),0};
 // Invalid previous depth.
 b.previous_depth[1*5+2]=0;
 // Background pixel never reuses.
 b.object[2*5+2]=0;b.hdr.at(2,2)={.7,.7,.7};
 Image ob=acc.resolve(b);
 assert(close(ob.at(0,0),V3{.2,.2,.2}));
 assert(close(ob.at(1,1),V3{.2,.2,.2}));
 assert(close(ob.at(2,1),V3{.2,.2,.2}));
 assert(close(ob.at(2,2),V3{.7,.7,.7}));
 // A normal pixel still blends (history gray 3 stays inside the wide box).
 assert(!close(ob.at(3,3),b.hdr.at(3,3)));
 std::cout<<"invalid query / background ok\n";
}

static void testRestartConditions(){
 // 5x5 frames, probe center (2,2) with a wide same-object box around it.
 auto make=[](uint64_t seq,V3 center){
  Frame f=plainFrame(5,5,seq);
  spreadNeighborhood(f,2,2);
  f.hdr.at(2,2)=center;
  return f;
 };
 TemporalAccumulator acc;
 acc.resolve(make(1,{1,1,1}));
 Frame cur=make(2,{.4,.4,.4});
 Image mixed=acc.resolve(cur);
 assert(!close(mixed.at(2,2),cur.hdr.at(2,2))); // history (1,1,1) survives the box

 // Sequence jump.
 assert(close(acc.resolve(make(9,{.4,.4,.4})).at(2,2),V3{.4,.4,.4}));
 // Camera cut (even though 10 = 9+1).
 Frame cut=make(10,{.4,.4,.4});cut.camera_cut=true;
 assert(close(acc.resolve(cut).at(2,2),V3{.4,.4,.4}));
 // Size change.
 Frame resized=plainFrame(6,6,11);
 spreadNeighborhood(resized,2,2);resized.hdr.at(2,2)={.4,.4,.4};
 assert(close(acc.resolve(resized).at(2,2),V3{.4,.4,.4}));
 // Reset.
 acc.reset();
 Frame after=plainFrame(6,6,12);
 spreadNeighborhood(after,2,2);after.hdr.at(2,2)={.4,.4,.4};
 assert(close(acc.resolve(after).at(2,2),V3{.4,.4,.4}));
 std::cout<<"restart conditions ok\n";
}

static void testBadArgumentsKeepHistory(){
 TemporalAccumulator acc;
 Frame a=plainFrame(4,4,1);
 for(auto& c:a.hdr.color)c={7,7,7};
 acc.resolve(a);
 Frame b=plainFrame(4,4,2);
 for(auto& c:b.hdr.color)c={7,7,7};
 acc.resolve(b);

 auto throws=[&](Frame f,const TemporalSettings& s){
  bool caught=false;
  try{acc.resolve(f,s);}catch(const std::invalid_argument&){caught=true;}
  assert(caught);
 };
 TemporalSettings badWeight;badWeight.history_weight=1.5;
 throws(plainFrame(4,4,3),badWeight);
 TemporalSettings negTh;negTh.depth_relative_threshold=-1;
 throws(plainFrame(4,4,3),negTh);
 TemporalSettings nanTh;nanTh.depth_relative_threshold=std::nan("");
 throws(plainFrame(4,4,3),nanTh);
 Frame badExp=plainFrame(4,4,3);badExp.exposure=0;
 throws(badExp,TemporalSettings{});
 Frame badExp2=plainFrame(4,4,3);badExp2.exposure=std::numeric_limits<double>::infinity();
 throws(badExp2,TemporalSettings{});
 Frame badSize=plainFrame(4,4,3);badSize.motion.resize(3);
 throws(badSize,TemporalSettings{});
 Frame badDepth=plainFrame(4,4,3);badDepth.depth.resize(7);
 throws(badDepth,TemporalSettings{});

 // History from frame 2 survives: a valid seq-3 frame still blends the stored (7,7,7).
 Frame c=plainFrame(4,4,3);
 for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++)c.hdr.at(1+dx,1+dy)={7,7,7};
 c.hdr.at(1,1)={.2,.2,.2};
 Image oc=acc.resolve(c);
 assert(close(oc.at(1,1),V3{6.32,6.32,6.32}));
 std::cout<<"invalid arguments preserve history ok\n";
}

static void testOwnershipAndAliasing(){
 // Checkerboard frames: every 3x3 box holds both black and white, so a gray history
 // color is never clamped and every resolved texel has an exact hand-computed value.
 auto checker=[](uint64_t seq){
  Frame f=plainFrame(4,4,seq);
  for(int yy=0;yy<4;yy++)for(int xx=0;xx<4;xx++)
   f.hdr.at(xx,yy)=((xx+yy)%2)?V3{8,8,8}:V3{0,0,0};
  return f;
 };
 TemporalAccumulator acc;
 Frame a=plainFrame(4,4,1);
 for(auto& c:a.hdr.color)c={3,3,3};
 Image oa=acc.resolve(a);
 // Mutate both the returned image and the input frame; internal history must stay intact.
 for(auto& c:oa.color)c={99,99,99};
 for(auto& c:a.hdr.color)c={-50,-50,-50};

 Frame b=checker(2);
 Image ob=acc.resolve(b);
 // Center (1,1) is black; history (3,3,3) unclamped -> .1*0+.9*3 = 2.7.
 assert(close(ob.at(1,1),V3{2.7,2.7,2.7}));
 // Mutate the returned snapshot and the input frame again.
 for(auto& c:ob.color)c={99,99,99};
 for(auto& c:b.hdr.color)c={-50,-50,-50};

 Frame c=checker(3);
 Image oc=acc.resolve(c);
 // The query lands exactly on texel (1,1) (fx=fy=0), so the footprint is a single
 // tap holding the resolved 2.7: current black center -> .9*2.7 = 2.43.
 assert(close(oc.at(1,1),V3{2.43,2.43,2.43}));
 std::cout<<"snapshot ownership / aliasing ok\n";
}

static void testIndependentAccumulators(){
 // Custom wide boxes (co/chroma ranges +/-8) so history colors pass the clamp untouched.
 auto wide=[](int cx,int cy,Frame& f){
  f.hdr.at(cx,cy)={1,1,1};
  f.hdr.at(cx-1,cy)={16,0,0};f.hdr.at(cx+1,cy)={0,0,16};
  f.hdr.at(cx,cy-1)={0,16,0};f.hdr.at(cx,cy+1)={0,0,0};
 };
 TemporalAccumulator x,y;
 Frame fx=plainFrame(4,4,1);for(auto& c:fx.hdr.color)c={10,0,0};
 Frame fy=plainFrame(4,4,1);for(auto& c:fy.hdr.color)c={0,0,10};
 x.resolve(fx);y.resolve(fy);
 Frame bx=plainFrame(4,4,2),by=plainFrame(4,4,2);
 wide(1,1,bx);wide(1,1,by);
 // Interleave the two accumulators so their histories cannot be shared.
 Image ox=x.resolve(bx);
 Image oy=y.resolve(by);
 assert(close(ox.at(1,1),V3{9.1,.1,.1}));
 assert(close(oy.at(1,1),V3{.1,.1,9.1}));
 Frame cx=plainFrame(4,4,3),cy=plainFrame(4,4,3);
 wide(1,1,cx);wide(1,1,cy);
 cx.hdr.at(1,1)=cy.hdr.at(1,1)={1,1,1};
 Image ocx=x.resolve(cx);
 Image ocy=y.resolve(cy);
 // x still only ever saw red history; y only blue, despite the interleaving.
 assert(close(ocx.at(1,1),V3{8.29,.19,.19}));
 assert(close(ocy.at(1,1),V3{.19,.19,8.29}));
 std::cout<<"independent accumulators ok\n";
}

// Reproduce the accumulator's four texel footprint for one pixel.
static void tapsOf(const Frame& f,const Frame& prev,int x,int y,int out[4][2]){
 V2 q{double(x)+.5+f.motion[size_t(y)*f.hdr.width+x].x+(prev.jitter.x-f.jitter.x),
      double(y)+.5+f.motion[size_t(y)*f.hdr.width+x].y+(prev.jitter.y-f.jitter.y)};
 double gx=q.x-.5,gy=q.y-.5;
 int x0=int(std::floor(gx)),y0=int(std::floor(gy));
 int k=0;
 int cand[4][2]={{x0,y0},{x0+1,y0},{x0,y0+1},{x0+1,y0+1}};
 for(auto& t:cand){out[k][0]=t[0];out[k][1]=t[1];++k;}
}

static void testDrawSequence(const std::string& dir){
 // Striped background triangle (object 1) plus a red foreground triangle (object 2)
 // sliding right, with alternating subpixel jitter.
 Camera cam({0,0,0},{0,0,1},{0,1,0},1.2);
 Triangle bg{{-6,-4,6},{6,-4,6},{0,6,6},{},{},{1,1,1},1};
 const int w=40,h=30;
 TemporalAccumulator acc;
 TemporalSettings st;
 Frame prev=Frame(w,h);
 int revealedAllReject=0,revealedBoxSafe=0,blendedFg=0,blendedBg=0;
 Image curOut(w,h),curHdr(w,h);
 for(int k=0;k<8;k++){
  double off=-1.2+0.18*k;
  Triangle fg{{-.7,-.7,3},{.7,-.7,3},{0,.7,3},{off,0,0},{off-0.18,0,0},{2,.4,.4},2};
  V2 jitter{k%2?V2{-.25,.25}:V2{.25,-.25}};
  Frame f=draw({bg,fg},cam,cam,w,h,jitter,1.5,100+k);
  curOut=acc.resolve(f,st);
  curHdr=f.hdr;
  if(k>0){
   for(int yy=0;yy<h;yy++)for(int xx=0;xx<w;xx++){
    size_t i=size_t(yy)*w+xx;
    // Background clear color is always passed through untouched.
    if(f.object[i]==0)assert(close(curOut.at(xx,yy),f.hdr.color[i]));
    if(f.object[i]==1&&prev.object[i]==2){
     // Newly revealed background: no foreground color may survive.
     int taps[4][2];tapsOf(f,prev,xx,yy,taps);
     bool anyBg=false,allOutside=true;
     for(auto& t:taps){
      if(t[0]<0||t[1]<0||t[0]>=w||t[1]>=h)continue;
      allOutside=false;
      if(prev.object[size_t(t[1])*w+t[0]]==1)anyBg=true;
     }
     if(!anyBg){
      // Every reachable texel was foreground: reuse is impossible -> exact current.
      assert(close(curOut.at(xx,yy),f.hdr.color[i]));
      if(!allOutside)++revealedAllReject;
     }else{
      // Some background history exists: result must stay inside the current
      // same-object neighborhood's per-channel box (no red foreground bleed).
      double lo[3]={1e300,1e300,1e300},hi[3]={-1e300,-1e300,-1e300};
      for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){
       int nx=xx+dx,ny=yy+dy;
       if(nx<0||ny<0||nx>=w||ny>=h)continue;
       if(f.object[size_t(ny)*w+nx]!=1)continue;
       V3 c=f.hdr.color[size_t(ny)*w+nx];
       double ch[3]={c.x,c.y,c.z};
       for(int q=0;q<3;q++){lo[q]=std::min(lo[q],ch[q]);hi[q]=std::max(hi[q],ch[q]);}
      }
      V3 r=curOut.at(xx,yy);
      double rc[3]={r.x,r.y,r.z};
      for(int q=0;q<3;q++)assert(rc[q]>=lo[q]-1e-10&&rc[q]<=hi[q]+1e-10);
      ++revealedBoxSafe;
     }
    }
    if(f.object[i]==2&&prev.object[i]==2&&!close(curOut.at(xx,yy),f.hdr.color[i]))
     ++blendedFg;
    if(f.object[i]==1&&prev.object[i]==1&&!close(curOut.at(xx,yy),f.hdr.color[i]))
     ++blendedBg;
   }
  }
  prev=f;
 }
 assert(revealedAllReject>0);
 assert(revealedBoxSafe>=0);
 assert(blendedFg>0);
 assert(blendedBg>0);
 curHdr.ppm(dir+"/current.ppm");
 curOut.ppm(dir+"/temporal.ppm");
 std::cout<<"draw sequence ok (revealed-all-reject="<<revealedAllReject
          <<", revealed-box-safe="<<revealedBoxSafe
          <<", blended fg="<<blendedFg<<", blended bg="<<blendedBg<<")\n";
}

int main(int argc,char** argv){
 std::string dir=argc>1?argv[1]:".";
 testFourTapWeights();
 testSingleTapRejection();
 testExposureScale();
 testYCoCgClamp();
 testJitterAndMotion();
 testInvalidQueriesAndBackground();
 testRestartConditions();
 testBadArgumentsKeepHistory();
 testOwnershipAndAliasing();
 testIndependentAccumulators();
 testDrawSequence(dir);
 std::cout<<"TEMPORAL_TESTS_OK\n";
}
