#include "tgaimage.h"
#include "geometry.h"

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

// Shared rasterization building blocks, used by both the byte-image pipeline
// (rasterize above) and the differentiable pipeline. They encode the original
// rules: integer pixel sampling, area culling (screen-space determinant < 1),
// perspective-corrected barycentric weights, larger z/w is nearer, and
// first-drawn fragments win on equal depth.
struct GlRasterUnit {
    vec4 ndc[3];    // clip coordinates divided by w
    vec2 screen[3]; // viewport-transformed screen coordinates
    vec4 clip[3];   // original clip coordinates (w kept for perspective correction)
    mat<3,3> abc_inv_t;
    double area;
    int bbminx, bbmaxx, bbminy, bbmaxy;
};

GlRasterUnit gl_make_raster_unit(const Triangle &clip);
bool         gl_unit_culled(const GlRasterUnit &u);
bool         gl_evaluate_pixel(const GlRasterUnit &u, const int x, const int y,
                               vec3 &bc_screen, vec3 &bc_clip, double &z);
bool         gl_depth_passes(const double z, const double stored);
double       gl_depth_clear();
