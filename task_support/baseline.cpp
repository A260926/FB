#include <microgl/math/std_float_math.h>
#define MICROGL_AVOID_BUILTIN_MATH
#include <microgl/canvas.h>
#include <microgl/bitmaps/bitmap.h>
#include <microgl/pixel_coders/RGB888_PACKED_32.h>
#include <microgl/shaders/color_shader.h>
#include <microgl/z_buffer.h>
#include <cstdio>

int main() {
    using S = microgl::shading::color_shader<float, microgl::rgba_t<8,8,8,0>>;
    canvas<bitmap<coder::RGB888_PACKED_32>, CANVAS_OPT_64_BIT_FREE> image(32,32);
    microgl::z_buffer<16> depth(32,32);
    S shader;
    image.clear({0,0,0,255});
    auto draw = [&](float z, microgl::color_t color) {
        S::vertex_attributes a{{-.8f,-.8f,z},color};
        S::vertex_attributes b{{1.4f,-.8f,z},color};
        S::vertex_attributes c{{0,.8f,z},color};
        image.drawTriangle<blendmode::Normal, porterduff::None<>, false,true,true>(
            shader,32,32,a,b,c,255,microtess::triangles::face_culling::none,&depth);
    };
    draw(-.4f,{220,20,10,255});
    draw(.5f,{0,200,0,255});
    microgl::color_t center;
    image.bitmapCanvas().decode(16*32+16,center);
    unsigned covered=0;
    for(int i=0;i<1024;++i) if(depth[i] != depth.max_value) ++covered;
    if(covered<100 || center.r<200 || center.g>30) return 1;
    std::printf("PRODUCTION_TRIANGLES clipped=1 depth_occlusion=1 covered=%u center=%u,%u,%u\n",
                covered,unsigned(center.r),unsigned(center.g),unsigned(center.b));
    return 0;
}
