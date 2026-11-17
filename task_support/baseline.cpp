#include <polymesh/Mesh.hh>
#include <polymesh/formats/obj.hh>
#include <sstream>
#include <iostream>
int main(){
 const char* verts="v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n";
 polymesh::Mesh good; std::istringstream a(std::string(verts)+"f 1 2 3\nf 1 3 4\n");
 polymesh::obj_reader<double> first(a,good);good.assert_consistency();
 if(first.error_faces()!=0||good.faces().size()!=2) return 1;
 polymesh::Mesh bad; std::istringstream b(std::string(verts)+"f 1 2 3\nf 1 4 3\n");
 polymesh::obj_reader<double> second(b,bad);bad.assert_consistency();
 std::cout<<"NORMAL faces="<<good.faces().size()<<" errors="<<first.error_faces()<<"\n";
 std::cout<<"WINDING_CONFLICT faces="<<bad.faces().size()<<" errors="<<second.error_faces()<<"\n";
 if(second.error_faces()!=1||bad.faces().size()!=1) return 2;
 auto edge=*good.edges().begin();auto v=good.edges().split(edge); (void)v;good.assert_consistency();
 std::cout<<"EDITABLE vertices="<<good.vertices().size()<<"\nPOLYMESH_BASELINE_OK\n";
}
