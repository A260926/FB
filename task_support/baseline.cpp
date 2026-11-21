#include <polymesh/Mesh.hh>
#include <polymesh/formats/obj.hh>
#include <sstream>
#include <iostream>
int main() {
 namespace pm=polymesh;
 pm::Mesh m;
 std::istringstream in("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0 0\nvt 1 0 0\nvt 1 1 0\nvt 0 1 0\nvn 0 0 1\nf 1/1/1 2/2/1 3/3/1\nf 1/4/1 3/3/1 4/2/1\n");
 pm::obj_reader<double> reader(in,m);
 if(reader.error_faces() || m.faces().size()!=2) return 1;
 auto pos=reader.get_positions(); auto uv=reader.get_tex_coords(); auto nor=reader.get_normals();
 std::ostringstream out; {pm::obj_writer<double> w(out);w.write_mesh(pos,&uv,&nor);}
 pm::Mesh copy; std::istringstream back(out.str()); pm::obj_reader<double> r(back,copy);
 if(r.error_faces() || copy.faces().size()!=2 || copy.vertices().size()!=4) return 2;
 m.edges().split(*m.edges().begin());m.assert_consistency();copy.assert_consistency();
 if(m.vertices().size()!=5) return 3;
 std::cout<<"OBJ_ROUNDTRIP faces="<<copy.faces().size()<<" vertices="<<copy.vertices().size()<<"\nSPLIT vertices="<<m.vertices().size()<<"\nPOLYMESH_BASELINE_OK\n";
}
