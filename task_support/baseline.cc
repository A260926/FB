#include "task_support/line_fixture.hh"
#include <iostream>
int main(){
 auto b=fixture::ordinary();auto t=fixture::table(b);unsigned rows=0,terminal=0;
 for(auto const& r:t){++rows;terminal+=r.end_sequence;std::cout<<"ROW "<<r.address<<":"<<r.op_index<<" "<<r.file->path<<":"<<r.line<<" end="<<r.end_sequence<<"\n";}
 auto it=t.find_address(0x102);if(it==t.end()||it->line!=1)return 2;
 std::cout<<"NORMAL_LOOKUP=1 rows="<<rows<<" terminal="<<terminal<<" tail_present="<<(t.find_address(0x106)!=t.end())<<"\n";
 if(rows<2)return 3;
}
