#pragma once
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>
using namespace std;
#include "tiny_bvh.h"
#include <cstdio>
#include <stdexcept>
#ifndef _MSC_VER
template<size_t N, class... Args> int sprintf_s(char (&dst)[N], const char* fmt, Args... args) {
    return std::snprintf(dst,N,fmt,args...);
}
template<size_t N> int strcat_s(char (&dst)[N], const char* suffix) {
    size_t used=std::strlen(dst), added=std::strlen(suffix);
    if(used+added>=N) { dst[0]=0; return 1; }
    std::memcpy(dst+used,suffix,added+1);return 0;
}
#endif
// Use TinyScene's documented custom-type integration, as in its glTF example.
#define TINYSCENE_USE_CUSTOM_VECTOR_TYPES
namespace tinyscene {
using ts_int2=tinybvh::bvhint2; using ts_int3=tinybvh::bvhint3;
using ts_uint2=tinybvh::bvhuint2; using ts_uint3=tinybvh::bvhuint3;
using ts_uint4=tinybvh::bvhuint4;
using ts_vec2=tinybvh::bvhvec2; using ts_vec3=tinybvh::bvhvec3;
using ts_vec4=tinybvh::bvhvec4; using ts_mat4=tinybvh::bvhmat4;
}
#include "tiny_scene.h"
