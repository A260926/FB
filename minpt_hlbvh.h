// minpt_hlbvh.h — LBVH (linear BVH) support for minpt's Scene.
//
// Part 1 (always): LBVH data types and Morton-coding helpers, included by
// minpt.cpp right before struct Scene (after I, F, V, B are defined).
// Part 2 (with MINPT_HLBVH_IMPL set): the parallel build pipeline, included
// by minpt.cpp right after struct Scene, so that every translation unit
// pulling in minpt.cpp (including the baseline, which links no other file)
// can resolve Scene::buildLBVH. Scene::refitLBVH and Scene::checkLBVH are
// defined out-of-line in src/hlbvh.cpp. Only the C++17 standard library is
// used; all parallelism is std::thread based.
#ifndef MINPT_HLBVH_H
#define MINPT_HLBVH_H
#include <vector>
#include <cstdint>

// LBVH node. With n faces there are n-1 internal nodes numbered in interval
// preorder starting at the root (node 0); the leaf for sorted slot s is node
// n-1+s and holds exactly one face. Children of an internal node cover the
// left/right key sub-intervals of its slot range.
struct LN{
 B b;                    // node bounds
 I c1=-1,c2=-1;          // children node ids (internal nodes); -1 on leaves
 I s=0,e=0;              // covered sorted-slot interval [s,e)
 I par=-1;               // parent node id, -1 at the root
 bool leaf=0;            // leaf flag
};

// Per-thread build task counters, accumulated at the real work locations.
struct LT{ unsigned topo=0;   // internal-node topology tasks completed
           unsigned bnd=0; }; // bottom-up bounds reductions completed

// Read-only build diagnostics, returned by Scene::lbvhDiag().
struct LD{
 std::vector<LN> ns;        // node snapshot of the last buildLBVH
 std::vector<I> ti;         // leaf order: sorted slot -> triangle cache index
 std::vector<uint32_t> mc;  // 30-bit Morton code per cached triangle
 std::vector<LT> th;        // per-worker task counters
 unsigned workers=0;        // worker count used by the last buildLBVH
};

// Spread the low 10 bits of x into every third bit (30-bit result).
inline uint32_t lspread(uint32_t x){
 x&=0x3ffu;
 x=(x|(x<<16))&0x030000FFu;
 x=(x|(x<<8))&0x0300F00Fu;
 x=(x|(x<<4))&0x030C30C3u;
 x=(x|(x<<2))&0x09249249u;
 return x;
}
// 30-bit Morton code of three 10-bit axis keys (x in the high bits).
inline uint32_t lmorton(uint32_t x,uint32_t y,uint32_t z){
 return (lspread(x)<<2)|(lspread(y)<<1)|lspread(z);
}
#endif

#if defined(MINPT_HLBVH_IMPL) && !defined(MINPT_HLBVH_IMPL_H)
#define MINPT_HLBVH_IMPL_H
// Parallel LBVH build pipeline (std::thread workers only):
//   1. collect triangles into Scene::trs in the original object/face order
//      (ti carries the reordering, trs keeps the collection order);
//   2. 10-bit-per-axis Morton codes from centroids: zero-span axes map to 0,
//      otherwise the centroid is normalized into the scene centroid range and
//      quantized with min(1023, floor(1024*x));
//   3. sort (code, original triangle index) keys with a deterministic
//      parallel merge sort;
//   4. deterministic binary radix hierarchy over the sorted 64-bit keys
//      (code<<32|index): every split sits at the longest common prefix of
//      the interval's first/last keys, internal nodes are numbered in
//      interval preorder, the leaf of slot s is node n-1+s holding exactly
//      one face, children cover the left/right key intervals — internal-node
//      tasks are distributed to the workers through a shared queue;
//   5. leaf bounds plus bottom-up internal bounds reduction, again split
//      across the workers (per-node arrival counters).
// Results (node fields and ti) are identical for any worker count.
namespace lbv{

// Longest common prefix length of two 64-bit keys.
inline I lcp64(uint64_t a,uint64_t b){
 uint64_t x=a^b; if(!x){return 64;} I n=0;
 if(!(x>>32)){n+=32;x<<=32;}
 if(!(x&0xFFFF000000000000ull)){n+=16;x<<=16;}
 if(!(x&0xFF00000000000000ull)){n+=8;x<<=8;}
 if(!(x&0xF000000000000000ull)){n+=4;x<<=4;}
 if(!(x&0xC000000000000000ull)){n+=2;x<<=2;}
 if(!(x&0x8000000000000000ull)){n+=1;}
 return n;
}

// Deterministic parallel merge sort: each worker sorts one chunk, then the
// sorted runs are merged pairwise with independent merges in parallel.
// Keys are unique, so the result is identical for any worker count.
inline void psort(vector<uint64_t>& a,unsigned W){
 I n=I(a.size()); if(n<2){return;}
 struct RN{I s,e;}; vector<RN> rs;
 for(unsigned w=0;w<W;w++){I s=I(int64_t(n)*w/W),e=I(int64_t(n)*(w+1)/W);
  if(s<e){rs.push_back({s,e});}}
 {vector<thread> ths;
  for(auto& r:rs){ths.emplace_back([&a,r]{sort(a.begin()+r.s,a.begin()+r.e);});}
  for(auto& t:ths){t.join();}}
 vector<uint64_t> tmp(n);
 while(rs.size()>1){
  vector<RN> nr; vector<thread> ths;
  for(size_t i=0;i<rs.size();i+=2){
   if(i+1==rs.size()){nr.push_back(rs[i]);continue;}
   I s=rs[i].s,m=rs[i].e,e=rs[i+1].e; nr.push_back({s,e});
   ths.emplace_back([&a,&tmp,s,m,e]{
    merge(a.begin()+s,a.begin()+m,a.begin()+m,a.begin()+e,tmp.begin()+s);
    copy(tmp.begin()+s,tmp.begin()+e,a.begin()+s);});
  }
  for(auto& t:ths){t.join();}
  rs=nr;
 }
}

// Collect triangles from the original faces into trs (original order kept).
inline void collect(Scene& sc){
 sc.trs.clear();
 for(size_t oi=0;oi<sc.os.size();oi++){auto& fs=sc.os[oi].fs;
  for(size_t fi=0;fi<fs.size();fi+=3){V a=sc.G.ps[fs[fi].p],b=sc.G.ps[fs[fi+1].p];
   V c=sc.G.ps[fs[fi+2].p]; sc.trs.emplace_back(a,b,c,I(oi),I(fi));}}
}

// Leaf bounds from the triangle cache plus bottom-up internal bounds
// reduction, split across W threads. When cnt is non-null, cnt[w].bnd is
// incremented at each reduction that thread actually performs.
inline void bndPass(Scene& sc,unsigned W,LT* cnt){
 I nt=I(sc.ti.size()); if(!nt){return;}
 I ni=nt-1;
 vector<atomic<I>> arr(ni); for(auto& a:arr){a.store(0);}
 vector<thread> ths;
 for(unsigned w=0;w<W;w++){
  I s0=I(int64_t(nt)*w/W),s1=I(int64_t(nt)*(w+1)/W);
  ths.emplace_back([&,w,s0,s1]{
   for(I sl=s0;sl<s1;sl++){
    I lid=ni+sl; sc.lns[lid].b=sc.trs[sc.ti[sl]].b;
    I p=sc.lns[lid].par;
    while(p>=0){
     if(arr[p].fetch_add(1)==0){break;}
     LN& nd=sc.lns[p]; nd.b=merge(sc.lns[nd.c1].b,sc.lns[nd.c2].b);
     if(cnt){cnt[w].bnd++;}
     p=nd.par;
    }
   }});
 }
 for(auto& t:ths){t.join();}
}

} // namespace lbv

inline void Scene::buildLBVH(unsigned W){
 W=W<1?1:W>64?64:W;
 useL=0; lns.clear(); ti.clear(); ldg=LD();
 lbv::collect(*this);
 I nt=I(trs.size());
 ldg.workers=W; ldg.th.assign(W,LT{});
 if(!nt){useL=1; return;}
 // Morton codes from centroids (parallel).
 vector<uint32_t> mc(nt);
 {
  vector<B> pbs(W); vector<thread> ths;
  for(unsigned w=0;w<W;w++){I s0=I(int64_t(nt)*w/W),s1=I(int64_t(nt)*(w+1)/W);
   ths.emplace_back([&,w,s0,s1]{B b; for(I i=s0;i<s1;i++){b=merge(b,trs[i].c);} pbs[w]=b;});}
  for(auto& t:ths){t.join();}
  B cb; for(auto& b:pbs){cb=merge(cb,b);}
  ths.clear();
  for(unsigned w=0;w<W;w++){I s0=I(int64_t(nt)*w/W),s1=I(int64_t(nt)*(w+1)/W);
   ths.emplace_back([&,s0,s1]{
    for(I i=s0;i<s1;i++){V c=trs[i].c; uint32_t q[3];
     for(I a=0;a<3;a++){F lo=cb.mi[a],sp=cb.ma[a]-lo;
      q[a]=sp>0?uint32_t(min(F(1023),floor(F(1024)*(c[a]-lo)/sp))):0u;}
     mc[i]=lmorton(q[0],q[1],q[2]);}});}
  for(auto& t:ths){t.join();}
 }
 // Sort (code, original index) keys (parallel).
 vector<uint64_t> sk(nt);
 for(I i=0;i<nt;i++){sk[i]=(uint64_t(mc[i])<<32)|uint32_t(i);}
 lbv::psort(sk,W);
 ti.assign(nt,0);
 for(I s=0;s<nt;s++){ti[s]=I(uint32_t(sk[s]));}
 // Radix hierarchy (parallel internal-node tasks from a shared queue).
 I ni=nt-1;
 lns.assign(2*nt-1,LN{});
 if(nt==1){LN& n0=lns[0]; n0.leaf=1; n0.s=0; n0.e=1;}
 else{
  struct TK{I id,s,e;};
  queue<TK> q; mutex mu; condition_variable cv;
  atomic<I> dn{0}; bool fin=0;
  q.push({0,0,nt});
  auto work=[&](I tid){
   for(;;){
    TK t;
    {unique_lock<mutex> lk(mu);
     cv.wait(lk,[&]{return fin||!q.empty();});
     if(q.empty()){return;}
     t=q.front(); q.pop();}
    I s=t.s,e=t.e; uint64_t ks=sk[s]; I ell=lbv::lcp64(ks,sk[e-1]);
    I lo=s,hi=e-2;
    while(lo<hi){I mid=(lo+hi+1)/2; if(lbv::lcp64(ks,sk[mid])>ell){lo=mid;}else{hi=mid-1;}}
    I g=lo,m=g+1,ls=m-s,rs=e-m;
    I c1=ls==1?ni+s:t.id+1;
    I c2=rs==1?ni+m:t.id+ls;
    {LN& nd=lns[t.id]; nd.s=s; nd.e=e; nd.leaf=0; nd.c1=c1; nd.c2=c2;}
    auto ch=[&](I cid,I cs,I ce){LN& c=lns[cid]; c.par=t.id; c.s=cs; c.e=ce;
     if(ce-cs==1){c.leaf=1; c.c1=-1; c.c2=-1;}};
    ch(c1,s,m); ch(c2,m,e);
    ldg.th[tid].topo++;
    {unique_lock<mutex> lk(mu);
     if(ls>1){q.push({c1,s,m});}
     if(rs>1){q.push({c2,m,e});}
     if(++dn==ni){fin=1;}
     cv.notify_all();}
   }};
  vector<thread> ths;
  for(unsigned w=0;w<W;w++){ths.emplace_back(work,I(w));}
  for(auto& t:ths){t.join();}
 }
 // Bounds (parallel bottom-up reduction).
 lbv::bndPass(*this,W,ldg.th.data());
 ldg.ns=lns; ldg.ti=ti; ldg.mc=mc;
 useL=1;
}
#endif
