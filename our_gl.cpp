#include <algorithm>
#include "our_gl.h"
#include "raster_core.h"

mat<4,4> ModelView, Viewport, Perspective; // "OpenGL" state matrices
std::vector<double> zbuffer;               // depth buffer

void lookat(const vec3 eye, const vec3 center, const vec3 up) {
    vec3 n = normalized(eye-center);
    vec3 l = normalized(cross(up,n));
    vec3 m = normalized(cross(n, l));
    ModelView = mat<4,4>{{{l.x,l.y,l.z,0}, {m.x,m.y,m.z,0}, {n.x,n.y,n.z,0}, {0,0,0,1}}} *
                mat<4,4>{{{1,0,0,-center.x}, {0,1,0,-center.y}, {0,0,1,-center.z}, {0,0,0,1}}};
}

void init_perspective(const double f) {
    Perspective = {{{1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0, -1/f,1}}};
}

void init_viewport(const int x, const int y, const int w, const int h) {
    Viewport = {{{w/2., 0, 0, x+w/2.}, {0, h/2., 0, y+h/2.}, {0,0,1,0}, {0,0,0,1}}};
}

void init_zbuffer(const int width, const int height) {
    zbuffer = std::vector(width*height, -1000.);
}

namespace raster_core {

Prepared prepare(const Triangle &clip, const mat<4,4> &viewport) {
    Prepared p;
    for (int i=0; i<3; i++) {
        p.ndc[i]    = clip[i]/clip[i].w;                          // normalized device coordinates
        p.clipw[i]  = clip[i].w;
        p.screen[i] = (viewport*p.ndc[i]).xy();                   // screen coordinates
    }
    p.abc = {{ {p.screen[0].x, p.screen[0].y, 1.}, {p.screen[1].x, p.screen[1].y, 1.}, {p.screen[2].x, p.screen[2].y, 1.} }};
    if (p.abc.det()<1) return p; // backface culling + discarding triangles that cover less than a pixel
    p.inv_abc_t = p.abc.invert_transpose();
    p.valid = true;
    return p;
}

vec3 bary_screen(const Prepared &p, const int x, const int y) {
    return p.inv_abc_t * vec3{static_cast<double>(x), static_cast<double>(y), 1.}; // barycentric coordinates of {x,y} w.r.t the triangle
}

Bounds pixel_bounds(const Prepared &p, const int width, const int height) {
    auto [bbminx,bbmaxx] = std::minmax({p.screen[0].x, p.screen[1].x, p.screen[2].x});
    auto [bbminy,bbmaxy] = std::minmax({p.screen[0].y, p.screen[1].y, p.screen[2].y});
    return { std::max<int>(bbminx, 0), std::min<int>(bbmaxx, width-1),
             std::max<int>(bbminy, 0), std::min<int>(bbmaxy, height-1) };
}

vec3 bary_clip(const Prepared &p, const vec3 &bc_screen) {
    vec3 bc = { bc_screen.x/p.clipw[0], bc_screen.y/p.clipw[1], bc_screen.z/p.clipw[2] }; // check https://github.com/ssloy/tinyrenderer/wiki/Technical-difficulties-linear-interpolation-with-perspective-deformations
    bc = bc / (bc.x + bc.y + bc.z);
    return bc;
}

double depth(const Prepared &p, const vec3 &bc_screen) {
    return bc_screen * vec3{ p.ndc[0].z, p.ndc[1].z, p.ndc[2].z }; // linear interpolation of the depth
}

} // namespace raster_core

void rasterize(const Triangle &clip, const IShader &shader, TGAImage &framebuffer) {
    raster_core::Prepared prepared = raster_core::prepare(clip, Viewport);
    if (!prepared.valid) return; // backface culling + discarding triangles that cover less than a pixel
    raster_core::Bounds bb = raster_core::pixel_bounds(prepared, framebuffer.width(), framebuffer.height());
#pragma omp parallel for
    for (int x=bb.x0; x<=bb.x1; x++) { // clip the bounding box by the screen
        for (int y=bb.y0; y<=bb.y1; y++) {
            vec3 bc_screen = raster_core::bary_screen(prepared, x, y);
            if (!raster_core::inside(bc_screen)) continue; // negative barycentric coordinate => the pixel is outside the triangle
            double z = raster_core::depth(prepared, bc_screen);
            if (!raster_core::depth_passes(z, zbuffer[x+y*framebuffer.width()])) continue; // discard fragments that are too deep w.r.t the z-buffer
            vec3 bc_clip = raster_core::bary_clip(prepared, bc_screen);
            auto [discard, color] = shader.fragment(bc_clip);
            if (discard) continue;                                 // fragment shader can discard current fragment
            zbuffer[x+y*framebuffer.width()] = z;                  // update the z-buffer
            framebuffer.set(x, y, color);                          // update the framebuffer
        }
    }
}

