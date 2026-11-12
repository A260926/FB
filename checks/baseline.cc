#include "voro++.hh"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
int main() {
  voro::container_poly con(0,1,0,1,0,1,2,2,2,false,false,false,4);
  con.put(13,.25,.5,.5,0);con.put(7,.75,.5,.5,.5);
  voro::c_loop_all it(con);voro::voronoicell_neighbor cell;
  double total=0;int count=0;
  if(it.start()) do {
    assert(con.compute_cell(cell,it));
    const double vol=cell.volume();total+=vol;++count;
    assert(std::fabs(vol-(it.pid()==13?.25:.75))<1e-12);
    std::vector<int> neighbors;std::vector<double> areas;
    cell.neighbors(neighbors);cell.face_areas(areas);
    assert(neighbors.size()==areas.size());
    int internal=0;for(size_t j=0;j<areas.size();++j) if(neighbors[j]>=0){
      assert(neighbors[j]==(it.pid()==13?7:13));
      assert(std::fabs(areas[j]-1)<1e-12);++internal;
    }
    assert(internal==1);
  }while(it.inc());
  assert(count==2 && std::fabs(total-1)<1e-12);
  puts("VORO_BASELINE_OK cells=2 volume=1 interface_area=1");
}
