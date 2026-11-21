#include <iostream>
#include <tuple>
#ifndef RASTERIZER_HEADLESS
#include <SDL2/SDL.h>
#endif

#include "math.h"
#include "model.h"

using namespace gfx;

constexpr auto W = 1920 / 2, H = 1080 / 2;
constexpr auto kMoveSpeed = 5.;
constexpr auto kRotSpeed = 0.0008;

const Vec kLightDir({0, 0, -1, 0});
const Vec kLightColor({1, 1, 1});

auto Barycentric(const Vec& a, const Vec& b, const Vec& c, const Vec& p) {
    Vec v0 = b - a, v1 = c - a, v2 = p - a;
    auto d00 = dot(v0, v0), d01 = dot(v0, v1), d11 = dot(v1, v1);
    auto d20 = dot(v2, v0), d21 = dot(v2, v1), denom = d00 * d11 - d01 * d01;
    auto v = (d11 * d20 - d01 * d21) / denom;
    auto w = (d00 * d21 - d01 * d20) / denom;
    return std::make_tuple(1.0 - v - w, v, w);
}

auto VertexShader(const auto& projection_mat, const auto& model_view_mat,
            const auto& normal_mat, const Vec& v, const Vec& n) {
    return std::make_tuple(
        projection_mat * model_view_mat * v,
        normalize(xyz(normal_mat * n)),
        normalize(xyz(normal_mat * -kLightDir)),
        normalize(-xyz(model_view_mat * v))
    );
}

auto FragmentShader(Vec n, Vec l, Vec v) -> Vec {
    Vec r = normalize(2.0 * dot(n, l) * n - l);
    Vec diffuse = kLightColor * std::max(0.f, dot(n, l));
    Vec specular = Vec({1, 1, 1}) * std::pow(std::max(0.f, dot(r, v)), 8);
    return 0.4 * diffuse + 0.4 * specular + 0.1 * kLightColor;
}

void Rasterize(const auto& projection_mat, const auto& model_view_mat, const auto& normal_mat,
                const Vec& v0, const Vec& v1, const Vec& v2,
                const Vec& n0, const Vec& n1, const Vec& n2,
                auto *pixels, auto *depth) {

    auto [v0_, n0_, l0_, p0_] = VertexShader(projection_mat, model_view_mat, normal_mat, v0, n0);
    auto [v1_, n1_, l1_, p1_] = VertexShader(projection_mat, model_view_mat, normal_mat, v1, n1);
    auto [v2_, n2_, l2_, p2_] = VertexShader(projection_mat, model_view_mat, normal_mat, v2, n2);

    // Discard triangles behind camera
    if (v0_[2] < 0 || v1_[2] < 0 || v2_[2] < 0) return;

    v0_ /= v0_[3]; v1_ /= v1_[3]; v2_ /= v2_[3];

    // Backface culling
    if (cross(v1_ - v0_, v2_ - v0_)[2] < 0) return;

    // Window to screen coordinates
    v0_[0] = (v0_[0] + 1) * W / 2; v0_[1] = (v0_[1] + 1) * H / 2;
    v1_[0] = (v1_[0] + 1) * W / 2; v1_[1] = (v1_[1] + 1) * H / 2;
    v2_[0] = (v2_[0] + 1) * W / 2; v2_[1] = (v2_[1] + 1) * H / 2;

    // Compute triangle bounding box
    auto y_min = std::max(0, (int)std::min({v0_[1], v1_[1], v2_[1]}));
    auto y_max = std::min(H - 1, 1 + (int)std::max({v0_[1], v1_[1], v2_[1]}));
    auto x_min = std::max(0, (int)std::min({v0_[0], v1_[0], v2_[0]}));
    auto x_max = std::min(W - 1, 1 + (int)std::max({v0_[0], v1_[0], v2_[0]}));

    #pragma omp parallel for collapse(2)
    for (auto y = y_min; y < y_max; y++) {
        for (auto x = x_min; x < x_max; x++) {
            auto [u, v, w] = Barycentric(xy(v0_), xy(v1_), xy(v2_), {(float)x, (float)y, 0, 0});
            if (u < 0 || v < 0 || w < 0) continue;

            // Z-buffer check
            Vec s = v0_ * u + v1_ * v + v2_ * w;
            if (isnan(s[2]) || s[2] > depth[y * W + x]) continue;
            depth[y * W + x] = s[2];

            Vec n = n0_ * u + n1_ * v + n2_ * w;
            Vec l = l0_ * u + l1_ * v + l2_ * w;
            Vec p = p0_ * u + p1_ * v + p2_ * w;

            Vec color = FragmentShader(n, l, p);
            auto r = (int)std::clamp(color[0] * 255, 0.f, 255.f);
            auto g = (int)std::clamp(color[1] * 255, 0.f, 255.f);
            auto b = (int)std::clamp(color[2] * 255, 0.f, 255.f);
            pixels[W * H - (y * W + x) - 1] = 0xff << 24 | r << 16 | g << 8 | b;
        }
    }
}

void Render(const auto& model, const auto& projection_mat, const auto& model_view_mat,
            const auto& normal_mat, auto *pixels, auto *depth) {
    for (const auto& face : model.get_faces()) {
        Rasterize(projection_mat, model_view_mat, normal_mat,
                    face[0]->pos, face[1]->pos, face[2]->pos,
                    face[0]->normal, face[1]->normal, face[2]->normal,
                    pixels, depth);
    }
}

auto ComputeModelMatrix(const auto& model) {
    // Normalize model vertices to box offset along -Z axis
    const auto [min, max] = model.ComputeBoundingBox();

    constexpr auto kBox = 100.;
    const auto scale = kBox / std::max({max[0] - min[0], max[1] - min[1], max[2] - min[2]});

    Mat4 model_matrix;
    model_matrix.values[std::slice(0, 3, 5)] = scale;
    model_matrix.values[std::slice(3, 3, 4)] = scale * (-min - (max - min) / 2);
    model_matrix(2, 3) += -kBox - kBox / 2;
    model_matrix(3, 3) = 1;

    Mat4 model_inv_matrix(model_matrix);
    model_inv_matrix.values[std::slice(0, 3, 5)] = 1 / scale;
    model_inv_matrix.values[std::slice(3, 3, 4)] = Vec(model_matrix.values[std::slice(3, 3, 4)]) / -scale;

    return std::make_tuple(model_matrix, model_inv_matrix);
}

#ifndef RASTERIZER_HEADLESS
int main(int argc, char **argv) {

    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <model.obj>" << std::endl;
        return 1;
    }

    Model model(argv[1]);

    if (!model.is_loaded()) {
        std::cerr << "Failed to load model: " << argv[1] << std::endl;
        return 1;
    }

    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* window = SDL_CreateWindow("rasterizer", SDL_WINDOWPOS_UNDEFINED,
                                SDL_WINDOWPOS_UNDEFINED, W, H, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, 0);
    SDL_Texture* framebuffer = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING, W, H);

    SDL_SetRelativeMouseMode(SDL_TRUE);
    SDL_ShowCursor(SDL_DISABLE);

    // https://stackoverflow.com/q/46182845
    constexpr auto r = 1., l = -1., t = (double)H / W, b = -t;
    constexpr auto f = 200., n = 1.;
    const Mat4 projection_mat({
        2 * n / (r - l), 0, (r + l) / (r - l), 0,
        0, 2 * n / (t - b), (t + b) / (t - b), 0,
        0, 0, -(f + n) / (f - n), -2 * f * n / (f - n),
        0, 0, -1, 0
    });

    auto [model_mat, model_inv_mat] = ComputeModelMatrix(model);

    auto camera_orientation = Quaternion::FromEuler(0, 0, 0);
    Vec camera_position({0, 0, 0, 1});

    unsigned pixels[W * H];
    double depth[W * H];

    while (true) {

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) return 0;
            if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_q || e.key.keysym.sym == SDLK_ESCAPE)) return 0;
            // Handle camera rotation
            if (e.type == SDL_MOUSEMOTION) {
                auto dx = e.motion.xrel;
                auto dy = e.motion.yrel;
                // Yaw global, pitch local to avoid unintended rolling
                camera_orientation = Quaternion::FromEuler(0, dx * kRotSpeed, 0)
                                    * camera_orientation
                                    * Quaternion::FromEuler(-dy * kRotSpeed, 0, 0);
            }
        }

        // Handle camera translation
        auto camera_matrix = camera_orientation.AsMatrix();
        auto state = SDL_GetKeyboardState(nullptr);

        Vec forward = camera_matrix * Vec({0, 0, -1, 0}); forward[1] = 0; forward = kMoveSpeed * normalize(forward);
        Vec right = camera_matrix * Vec({1, 0, 0, 0}); right[1] = 0; right = kMoveSpeed * normalize(right);
        camera_position += forward * (state[SDL_SCANCODE_W] - state[SDL_SCANCODE_S]);
        camera_position += right * (state[SDL_SCANCODE_A] - state[SDL_SCANCODE_D]);
        camera_position += Vec({0, kMoveSpeed, 0, 0}) * (state[SDL_SCANCODE_SPACE] - state[SDL_SCANCODE_LSHIFT]);

        // Setup model matrix and rasterize
        Mat4 camera_pos_mat;
        camera_pos_mat.values[std::slice(3, 3, 4)] = camera_position;
        camera_pos_mat.values[std::slice(0, 4, 5)] = 1;

        auto view_mat = camera_matrix.Transpose();
        view_mat.values[std::slice(3, 3, 4)] = view_mat * -camera_position;

        const auto model_view_mat = view_mat * model_mat;
        const auto normal_mat = (camera_matrix * camera_pos_mat * model_inv_mat).Transpose();

        std::fill_n(pixels, W * H, 0);
        std::fill_n(depth, W * H, 1e99);

        Render(model, projection_mat, model_view_mat, normal_mat, pixels, depth);

        SDL_UpdateTexture(framebuffer, NULL, pixels, W * sizeof(unsigned));
        SDL_RenderCopy(renderer, framebuffer, NULL, NULL);
        SDL_RenderPresent(renderer);
    }
}

#endif // RASTERIZER_HEADLESS
