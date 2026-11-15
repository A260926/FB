#ifndef PHONG_SHADER_H
#define PHONG_SHADER_H

#include "our_gl.h"
#include "model.h"

// The original Phong drawing path of main.cpp, factored out unchanged so the
// projective baking example can render a baked atlas through the same
// IShader-based material pipeline.
struct PhongShader : IShader {
    const Model &model;
    vec4 l;              // light direction in eye coordinates
    vec2  varying_uv[3]; // triangle uv coordinates, written by the vertex shader, read by the fragment shader
    vec4 varying_nrm[3]; // normal per vertex to be interpolated by the fragment shader
    vec4 tri[3];         // triangle in view coordinates

    PhongShader(const vec3 light, const Model &m) : model(m) {
        l = normalized((ModelView*vec4{light.x, light.y, light.z, 0.})); // transform the light vector to view coordinates
    }

    virtual vec4 vertex(const int face, const int vert) {
        varying_uv[vert]  = model.uv(face, vert);
        varying_nrm[vert] = ModelView.invert_transpose() * model.normal(face, vert);
        vec4 gl_Position = ModelView * model.vert(face, vert);
        tri[vert] = gl_Position;
        return Perspective * gl_Position;                         // in clip coordinates
    }

    virtual std::pair<bool,TGAColor> fragment(const vec3 bar) const {
        mat<2,4> E = { tri[1]-tri[0], tri[2]-tri[0] };
        mat<2,2> U = { varying_uv[1]-varying_uv[0], varying_uv[2]-varying_uv[0] };
        mat<2,4> T = U.invert() * E;
        mat<4,4> D = {normalized(T[0]),  // tangent vector
                      normalized(T[1]),  // bitangent vector
                      normalized(varying_nrm[0]*bar[0] + varying_nrm[1]*bar[1] + varying_nrm[2]*bar[2]), // interpolated normal
                      {0,0,0,1}}; // Darboux frame
        vec2 uv = varying_uv[0] * bar[0] + varying_uv[1] * bar[1] + varying_uv[2] * bar[2];
        vec4 n = normalized(D.transpose() * model.normal(uv));
        vec4 r = normalized(n * (n * l)*2 - l);                   // reflected light direction
        double ambient  = .4;                                     // ambient light intensity
        double diffuse  = 1.*std::max(0., n * l);                 // diffuse light intensity
        double specular = (.5+2.*sample2D(model.specular(), uv)[0]/255.) * std::pow(std::max(r.z, 0.), 35);  // specular intensity, note that the camera lies on the z-axis (in eye coordinates), therefore simple r.z, since (0,0,1)*(r.x, r.y, r.z) = r.z
        TGAColor gl_FragColor = sample2D(model.diffuse(), uv);
        for (int channel : {0,1,2})
            gl_FragColor[channel] = std::min<int>(255, gl_FragColor[channel]*(ambient + diffuse + specular));
        return {false, gl_FragColor};                             // do not discard the fragment
    }
};

#endif // PHONG_SHADER_H
