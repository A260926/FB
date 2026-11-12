#include "voro++.hh"
#include <iostream>
#include <set>
#include <cmath>
int main(){
 voro::container_periodic c(1,0,1,0,0,1,2,2,2,8);c.put(17,.2,.3,.4);
 voro::c_loop_all_periodic loop(c);voro::voronoicell_neighbor cell(c);
 if(!loop.start()||!c.compute_cell(cell,loop))return 1;
 std::vector<int> ids,f;cell.neighbors(ids);cell.face_vertices(f);
 std::set<int> distinct(ids.begin(),ids.end());
 std::cout<<"VORO_PERIODIC_BASELINE_OK volume="<<cell.volume()<<" faces="<<ids.size()<<" labels=";
 for(int i:distinct)std::cout<<i<<",";
 std::cout<<"\n";return std::abs(cell.volume()-1)>1e-10||ids.size()!=6;
}
