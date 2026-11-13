#include "check.h"
int main(){
  for(auto c:task_cases()) {task_checks()=0;c.fn();
    if(!task_checks()) return 3;
    std::printf("GROUP %s CHECKS %u DONE\n",c.name,task_checks());
  }
  std::printf("TARGET COMPLETE groups=%zu\n",task_cases().size());
}
