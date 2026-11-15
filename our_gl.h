#pragma once
#include "tgaimage.h"
#include "geometry.h"
#include <vector>

extern mat<4,4> ModelView, Viewport, Perspective; // "OpenGL" state matrices
extern std::vector<double> zbuffer;               // the depth buffer

void lookat(const vec3 eye, const vec3 center, const vec3 up);
void init_perspective(const double f);
void init_viewport(const int x, const int y, const int w, const int h);
void init_zbuffer(const int width, const int height);

struct IShader {
    static TGAColor sample2D(const TGAImage &img, const vec2 &uvf) {
        return img.get(uvf[0] * img.width(), uvf[1] * img.height());
    }
    virtual std::pair<bool,TGAColor> fragment(const vec3 bar) const = 0;
};

typedef vec4 Triangle[3]; // a triangle primitive is made of three ordered points
void rasterize(const Triangle &clip, const IShader &shader, TGAImage &framebuffer);

// Same coverage, perspective correction and depth-commit kernel as rasterize(),
// but without a fragment shader: it only runs the z-buffer against the given
// depth buffer. The global Viewport matrix is used exactly like rasterize() does.
void rasterize_depth(const Triangle &clip, std::vector<double> &depth_buffer,
                     const int width, const int height);
