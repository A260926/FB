#include "fixture_io.h"
int main(){try{ordered_fixture::write(".task-build/ordered-input.mkv");return ordered_fixture::inspect(".task-build/ordered-input.mkv");}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
