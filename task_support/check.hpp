#pragma once
#include <stdexcept>
struct Check {unsigned n=0;void operator()(bool x,const char* m){++n;if(!x)throw std::runtime_error(m);}};
