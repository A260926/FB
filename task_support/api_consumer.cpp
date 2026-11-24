#include "relacy/relacy.hpp"
#include <cstring>
#include <sstream>
struct acyclic_consumer : rl::test_suite<acyclic_consumer,1> {
 rl::mutex a,b;
 void thread(unsigned){a.lock($);b.lock($);b.unlock($);a.unlock($);}
};
int main(){rl::test_params p;
 if(p.check_lock_order)return 1;
 if(std::strcmp(rl::test_result_str(rl::test_result_lock_order_inversion),"LOCK ORDER INVERSION"))return 2;
 p.check_lock_order=true;p.iteration_count=2;std::ostringstream out;p.output_stream=&out;
 if(!rl::simulate<acyclic_consumer>(p)||p.test_result!=rl::test_result_success)return 3;
 if(out.str().find("LOCK ORDER INVERSION")!=std::string::npos)return 4;
}
