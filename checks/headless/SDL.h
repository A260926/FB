#pragma once
// Minimal display adapter for the unchanged upstream VertexProcessorTest.cpp.
// It does not rasterize, interpolate, or supply any reflection implementation.
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <stdexcept>
using Uint8=std::uint8_t;
using Uint32=std::uint32_t;
constexpr int SDL_INIT_VIDEO=1, SDL_WINDOWPOS_UNDEFINED=0, SDL_QUIT=256;
struct SDL_Surface { void* pixels; int pitch; };
struct SDL_Window {
    std::vector<Uint32> pixels;
    SDL_Surface surface;
    explicit SDL_Window(int w,int h):pixels(w*h,0),surface{pixels.data(),w*4} {}
};
struct SDL_Event { int type=SDL_QUIT; };
inline int SDL_Init(int){return 0;}
inline SDL_Window* SDL_CreateWindow(const char*,int,int,int w,int h,int){return new SDL_Window(w,h);}
inline SDL_Surface* SDL_GetWindowSurface(SDL_Window* w){return &w->surface;}
inline int SDL_UpdateWindowSurface(SDL_Window* w){
    std::size_t count=0; for(auto p:w->pixels) if(p) ++count;
    if(count<1000) throw std::runtime_error("upstream example did not paint");
    std::cout<<"UPSTREAM_VERTEX_EXAMPLE_OK colored="<<count<<'\n'; return 0;
}
inline int SDL_WaitEvent(SDL_Event* e){e->type=SDL_QUIT;return 1;}
inline void SDL_DestroyWindow(SDL_Window* w){delete w;}
inline void SDL_Quit(){}
