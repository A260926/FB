#include <spatialindex/SpatialIndex.h>
#include <cassert>
#include <algorithm>
#include <memory>
#include <vector>
#include <iostream>
using namespace SpatialIndex;
struct V: IVisitor {
 std::vector<id_type> ids;
 void visitNode(const INode&) override {}
 void visitData(const IData& d) override {ids.push_back(d.getIdentifier());}
 void visitData(std::vector<const IData*>&) override {}
};
int main() {
 std::unique_ptr<IStorageManager> s(StorageManager::createNewMemoryStorageManager());
 id_type id;std::unique_ptr<ISpatialIndex> t(RTree::createNewRTree(*s,0.7,8,8,2,RTree::RV_RSTAR,id));
 for(int i=0;i<40;i++){double p[2]={double(i*10),0};Region r(p,p,2);t->insertData(0,nullptr,r,i);}
 double p[2]={5,0};Point query(p,2);V v;t->nearestNeighborQuery(1,query,v);
 std::sort(v.ids.begin(),v.ids.end());assert((v.ids==std::vector<id_type>{0,1}));assert(t->isIndexValid());
 std::cout<<"SINGLE_KNN_TIES=0,1 INDEX_VALID\n";
}
