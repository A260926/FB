#pragma once
#include <cstdio>
#include <cstdlib>
#include <vector>
struct ClipCase{const char* name;void(*run)();};
inline std::vector<ClipCase>& clip_cases(){static std::vector<ClipCase> c;return c;}
inline unsigned clip_checks=0;
struct ClipRegister{ClipRegister(const char* n,void(*f)()){clip_cases().push_back({n,f});}};
#define CLIP_CASE(name) static void name();static ClipRegister clip_reg_##name(#name,name);static void name()
#define CLIP_CHECK(x) do{++clip_checks;if(!(x)){std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);std::exit(1);}}while(0)
int main(){if(clip_cases().size()<5)return 2;for(auto c:clip_cases()){unsigned before=clip_checks;c.run();if(clip_checks==before)return 3;std::printf("clip case %s ok\n",c.name);}std::printf("CLIPPED_MESH_OK cases=%zu checks=%u\n",clip_cases().size(),clip_checks);}
