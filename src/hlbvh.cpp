// src/hlbvh.cpp — LBVH frame-to-frame refit and structural check for Scene.
//
// The parallel build pipeline (Morton coding, key sort, radix hierarchy,
// bottom-up bounds reduction) lives in minpt_hlbvh.h; this translation unit
// carries the out-of-line parts of the new build:
//
// Scene::refitLBVH — the caller may move G.ps between frames (object/face
//   topology fixed). The triangle cache and the leaf bounds are recomputed
//   from the original faces and the ancestor bounds are updated bottom-up.
//   Connectivity, leaf slots and ti are preserved; no builder and no sort
//   is invoked.
//
// Scene::checkLBVH — structural validation of the actual live tree: root
//   reachability, parent/child slot intervals, complete and duplicate-free
//   leaf coverage, and parent-box containment.
#define main minpt_hlbvh_unused_main
#include "../minpt.cpp"
#undef main

void Scene::refitLBVH(unsigned W){
 if(!useL){return;}
 W=W<1?1:W>64?64:W;
 lbv::collect(*this);                     // geometry cache from the original faces
 if(I(ti.size())!=I(trs.size())){return;} // object/face topology must be unchanged
 lbv::bndPass(*this,W,nullptr);           // leaf bounds + ancestor bounds update
}

bool Scene::checkLBVH() const{
 if(!useL){return false;}
 I nt=I(ti.size());
 if(!nt){return lns.empty();}
 if(I(lns.size())!=2*nt-1){return false;}
 auto contains=[](const B& p,const B& c){
  for(I i=0;i<3;i++){if(p.mi[i]>c.mi[i]||p.ma[i]<c.ma[i]){return false;}} return true;};
 const LN& r0=lns[0];
 if(r0.par!=-1||r0.s!=0||r0.e!=nt){return false;}
 vector<char> vis(lns.size(),0),cov(nt,0);
 vector<I> st; st.push_back(0); I nv=0;
 while(!st.empty()){
  I i=st.back(); st.pop_back();
  if(i<0||i>=I(lns.size())||vis[i]){return false;}
  vis[i]=1; nv++;
  const LN& n=lns[i];
  if(n.leaf){
   if(n.c1!=-1||n.c2!=-1||n.e-n.s!=1){return false;}
   if(i!=nt-1+n.s||n.s<0||n.s>=nt||cov[n.s]){return false;}
   cov[n.s]=1;
   if(!contains(n.b,trs[ti[n.s]].b)){return false;}
  }else{
   if(n.c1<0||n.c2<0||n.c1>=I(lns.size())||n.c2>=I(lns.size())){return false;}
   const LN& a=lns[n.c1]; const LN& b=lns[n.c2];
   if(a.par!=i||b.par!=i){return false;}
   if(a.s!=n.s||b.e!=n.e||a.e!=b.s||a.e<=a.s||b.e<=b.s){return false;}
   if(!contains(n.b,a.b)||!contains(n.b,b.b)){return false;}
   st.push_back(n.c1); st.push_back(n.c2);
  }
 }
 if(nv!=I(lns.size())){return false;}
 for(I s=0;s<nt;s++){if(!cov[s]){return false;}}
 return true;
}
