#include <algorithm>
#include "our_gl.h"

mat<4,4> ModelView, Viewport, Perspective; // "OpenGL" state matrices
std::vector<double> zbuffer;               // depth buffer

constexpr double depth_clear_value = -1000.; // matches the historical init_zbuffer fill

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
    zbuffer = std::vector(width*height, depth_clear_value);
}

GlRasterUnit gl_make_raster_unit(const Triangle &clip) {
    GlRasterUnit u;
    for (int i = 0; i < 3; i++) {
        u.clip[i]   = clip[i];
        u.ndc[i]    = clip[i]/clip[i].w;                                   // normalized device coordinates
        u.screen[i] = (Viewport*u.ndc[i]).xy();                            // screen coordinates
    }
    mat<3,3> ABC = {{ {u.screen[0].x, u.screen[0].y, 1.},
                      {u.screen[1].x, u.screen[1].y, 1.},
                      {u.screen[2].x, u.screen[2].y, 1.} }};
    u.area      = ABC.det();
    u.abc_inv_t = ABC.invert_transpose();
    auto [bbminx,bbmaxx] = std::minmax({u.screen[0].x, u.screen[1].x, u.screen[2].x});
    auto [bbminy,bbmaxy] = std::minmax({u.screen[0].y, u.screen[1].y, u.screen[2].y});
    u.bbminx = bbminx; u.bbmaxx = bbmaxx;
    u.bbminy = bbminy; u.bbmaxy = bbmaxy;
    return u;
}

bool gl_unit_culled(const GlRasterUnit &u) {
    return u.area < 1; // backface culling + discarding triangles that cover less than a pixel
}

bool gl_evaluate_pixel(const GlRasterUnit &u, const int x, const int y,
                       vec3 &bc_screen, vec3 &bc_clip, double &z) {
    bc_screen = u.abc_inv_t * vec3{static_cast<double>(x), static_cast<double>(y), 1.}; // barycentric coordinates of {x,y}
    bc_clip   = { bc_screen.x/u.clip[0].w, bc_screen.y/u.clip[1].w, bc_screen.z/u.clip[2].w }; // perspective-corrected weights
    bc_clip   = bc_clip / (bc_clip.x + bc_clip.y + bc_clip.z);
    if (bc_screen.x<0 || bc_screen.y<0 || bc_screen.z<0) return false; // outside the triangle
    z = bc_screen * vec3{ u.ndc[0].z, u.ndc[1].z, u.ndc[2].z };        // linear interpolation of the depth
    return true;
}

bool gl_depth_passes(const double z, const double stored) {
    return z > stored; // strict: equal depth keeps the fragment drawn first
}

double gl_depth_clear() {
    return depth_clear_value;
}

void rasterize(const Triangle &clip, const IShader &shader, TGAImage &framebuffer) {
    GlRasterUnit u = gl_make_raster_unit(clip);
    if (gl_unit_culled(u)) return;

#pragma omp parallel for
    for (int x=std::max<int>(u.bbminx, 0); x<=std::min<int>(u.bbmaxx, framebuffer.width()-1); x++) {  // clip the bounding box by the screen
        for (int y=std::max<int>(u.bbminy, 0); y<=std::min<int>(u.bbmaxy, framebuffer.height()-1); y++) {
            vec3 bc_screen, bc_clip;
            double z;
            if (!gl_evaluate_pixel(u, x, y, bc_screen, bc_clip, z)) continue;
            if (!gl_depth_passes(z, zbuffer[x+y*framebuffer.width()])) continue; // discard fragments that are too deep
            auto [discard, color] = shader.fragment(bc_clip);
            if (discard) continue;                                 // fragment shader can discard current fragment
            zbuffer[x+y*framebuffer.width()] = z;                  // update the z-buffer
            framebuffer.set(x, y, color);                          // update the framebuffer
        }
    }
}
