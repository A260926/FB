#include "relacy/relacy.hpp"
#include <iostream>
struct benign_wait : rl::test_suite<benign_wait,2> {
 rl::mutex m; rl::condition_variable cv; rl::var<int> ready;
 void before(){ready($)=0;}
 void thread(unsigned i){
  m.lock($);
  if(i==0){ready($)=1;cv.notify_all($);}
  else {while(ready($)==0)cv.wait(m,$);RL_ASSERT(ready($)==1);}
  m.unlock($);
 }
};
int main(){rl::test_params p;p.iteration_count=64;
 bool ok=rl::simulate<benign_wait>(p);
 if(!ok || p.test_result!=rl::test_result_success)return 1;
 std::cout<<"RELACY_NORMAL_WAIT_OK iterations="<<p.iteration_count<<"\n";
}
