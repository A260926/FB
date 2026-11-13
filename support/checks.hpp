#pragma once
#include <stdexcept>
#include <string>
namespace join_checks {
inline int count=0;
inline void require(bool ok,const char* message) {
  ++count;
  if(!ok) throw std::runtime_error(message);
}
}
void run_join_tests(const std::string& group);
