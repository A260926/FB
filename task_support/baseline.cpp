#include "libHh/GMesh.h"
#include <sstream>
#include <iostream>
using namespace hh;
int main() {
 GMesh m;
 std::istringstream in("Vertex 1 1 1 1\nVertex 2 1 -1 -1\nVertex 3 -1 1 -1\nVertex 4 -1 -1 1\nFace 1 1 2 3\nFace 2 1 3 4\nFace 3 1 4 2\nFace 4 2 4 3\n");
 m.read(in); m.ok();
 if (m.num_vertices()!=4 || m.num_faces()!=4) return 1;
 std::ostringstream seed, edit;
 m.write(seed); m.record_changes(&edit);
 Vertex v=m.split_edge(m.edge(m.id_vertex(1),m.id_vertex(2)));
 m.record_changes(nullptr); m.ok();
 if (m.num_vertices()!=5 || m.num_faces()!=6 || m.point(v)!=Point(1,0,0)) return 2;
 GMesh replay; std::istringstream whole(seed.str()+edit.str());replay.read(whole);replay.ok();
 if(replay.num_vertices()!=5 || replay.num_faces()!=6) return 3;
 std::cout << "BASELINE_OK tetra=4/4 split=5/6 replay=5/6\n";
}
