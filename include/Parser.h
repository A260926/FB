#ifndef PARSER_H
#define PARSER_H

#include "util/Utilities.h"
#include "object/ObjectList.h"
#include "light/LightList.h"
#include "Camera.h"
#include "light/PointLight.h"
#include "light/RectangularLight.h"
#include "object/Sphere.h"
#include "object/Cylinder.h"
#include "object/Triangle.h"

#include <json.hpp>
#include <vector>

using json = nlohmann::json;

class Parser {
    public:

    Parser() = default;
    ~Parser() {
        for (auto* m : materials_) delete m;
    }

    // Main function to parse the scene, includes reading the file
    void parse_scene(const std::string& inputDir, const std::string& jsonName, ObjectList& world, LightList& lights, Camera& cam) {

        // Open the input JSON file
        std::ifstream input_file(inputDir);
        if (!input_file) {
            std::cerr << "Error: Could not open the JSON file: " << inputDir << std::endl;
            throw std::runtime_error("File opening failed.");
        }

        // Read JSON data
        json scene_data;
        input_file >> scene_data;

        // Parse the scene
        parse_scene(scene_data, world, lights, cam);
    }

    private:

    std::vector<Material*> materials_;

    void parse_scene(const json& scene_data, ObjectList& world, LightList& lights, Camera& cam) {
        if (!scene_data.contains("rendermode")) {
            std::cerr << "Error: Missing required field 'rendermode'" << std::endl;
            throw std::runtime_error("Missing required field 'rendermode'.");
        }
        std::string mode = scene_data["rendermode"];
        if (mode == "binary") {
            cam.render_mode = RenderMode::BINARY;
        } else if (mode == "phong") {
            cam.render_mode = RenderMode::PHONG;
        } else if (mode == "path") {
            cam.render_mode = RenderMode::PATH;
        } else {
            std::cerr << "Error: Invalid rendermode '" << mode << "'. Must be 'binary', 'phong', or 'path'." << std::endl;
            throw std::runtime_error("Invalid rendermode.");
        }


        if (scene_data.contains("nbounces")) {
            cam.nbounces = scene_data["nbounces"];
        }

        if (scene_data.contains("RR_PROB")) {
            cam.RR_PROB = scene_data["RR_PROB"];
            if (cam.RR_PROB <= 0.0 || cam.RR_PROB > 1.0) {
                throw std::runtime_error("RR_PROB must be > 0 and <= 1.0, got: " + std::to_string(cam.RR_PROB));
            }
        }

        if (scene_data.contains("useBVH")) {
            if(scene_data["useBVH"]) {
                world.use_bvh = UseBVH::ENABLE;
            } else {
                world.use_bvh = UseBVH::DISABLE;
            }
        }

        // Parse camera
        if (!scene_data.contains("camera")) {
            std::cerr << "Error: Missing required field 'camera'" << std::endl;
            throw std::runtime_error("Missing required field 'camera'.");
        }
        auto camera_data = scene_data["camera"];

        if (!camera_data.contains("width") || !camera_data.contains("height")) {
            std::cerr << "Error: Missing required camera fields 'width' and/or 'height'" << std::endl;
            throw std::runtime_error("Missing required camera fields.");
        }
        if (camera_data["width"].get<int>() <= 0 || camera_data["height"].get<int>() <= 0) {
            std::cerr << "Error: Camera width and height must be positive integers" << std::endl;
            throw std::runtime_error("Invalid camera dimensions.");
        }

        if (!camera_data.contains("fov")) {
            std::cerr << "Error: Missing required camera field 'fov'" << std::endl;
            throw std::runtime_error("Missing required camera field 'fov'.");
        }

        cam.image_width = camera_data["width"];
        cam.image_height = camera_data["height"];
        cam.lookfrom = Point3(camera_data["position"][0], camera_data["position"][1], camera_data["position"][2]);
        cam.lookat = Point3(camera_data["lookAt"][0], camera_data["lookAt"][1], camera_data["lookAt"][2]);
        cam.vup = Vector3(camera_data["upVector"][0], camera_data["upVector"][1], camera_data["upVector"][2]);
        cam.vfov = camera_data["fov"];
        cam.background = Radiance(scene_data["scene"]["backgroundcolor"][0], 
                            scene_data["scene"]["backgroundcolor"][1], 
                            scene_data["scene"]["backgroundcolor"][2]);
        cam.exposure = camera_data["exposure"];

        if(scene_data["scene"].contains("ambient_light")){
            cam.ambient_light = Radiance(scene_data["scene"]["ambient_light"][0], 
                            scene_data["scene"]["ambient_light"][1], 
                            scene_data["scene"]["ambient_light"][2]);
        }

        if (camera_data.contains("samples_per_pixel")) {
            cam.samples_per_pixel = camera_data["samples_per_pixel"];
            if (cam.samples_per_pixel <= 0) {
                throw std::runtime_error("samples_per_pixel must be > 0, got: " + std::to_string(cam.samples_per_pixel));
            }
            if (camera_data.contains("sampling_method")) {
                if (camera_data["sampling_method"] == "random") {
                    cam.sampling_method = AntiAliasing::RANDOM;
                } else if (camera_data["sampling_method"] == "jittered") {
                    cam.sampling_method = AntiAliasing::JITTERED;
                } else if (camera_data["sampling_method"] == "poisson_disk") {
                    cam.sampling_method = AntiAliasing::POISSON;
                }
            }
        }
        if (camera_data.contains("aperture")) {
            cam.aperture = camera_data["aperture"];
        }
        if (camera_data.contains("focus_dist")) {
            cam.focus_dist = camera_data["focus_dist"];
        }

        if (camera_data.contains("allowBRDF")) {
            cam.allow_brdf = camera_data["allowBRDF"];
        }

        if (camera_data.contains("toneMapper")) {
            std::string tone_mapper = camera_data["toneMapper"];
            if (tone_mapper == "linear") {
                cam.tone_mapper = ToneMapper::LINEAR;
            } else if (tone_mapper == "reinhard") {
                cam.tone_mapper = ToneMapper::REINHARD;
            } else if (tone_mapper == "filmic") {
                cam.tone_mapper = ToneMapper::FILMIC;
            } else if (tone_mapper == "luminance") {
                cam.tone_mapper = ToneMapper::LUMINANCE;
            }
        }

        // Parse light sources
        if(cam.render_mode != RenderMode::BINARY) {
            for (const auto& light_data : scene_data["scene"]["lightsources"]) {
                std::string type = light_data["type"];
                if (type == "pointlight") {
                    lights.add(make_shared<PointLight>(
                        Point3(light_data["position"][0], light_data["position"][1], light_data["position"][2]),
                        Radiance(light_data["intensity"][0], light_data["intensity"][1], light_data["intensity"][2])
                    ));
                }  else if (type == "rectlight") {
                    lights.add(make_shared<RectangularLight>(
                        Point3(light_data["position"][0], light_data["position"][1], light_data["position"][2]),
                        Vector3(light_data["u"][0], light_data["u"][1], light_data["u"][2]),
                        Vector3(light_data["v"][0], light_data["v"][1], light_data["v"][2]),
                        light_data["width"],
                        light_data["height"],
                        Radiance(light_data["intensity"][0], light_data["intensity"][1], light_data["intensity"][2]),
                        light_data["num_samples"]
                    ));
                }
            }
        }

        // Parse shapes and materials
        for (const auto& shape_data : scene_data["scene"]["shapes"]) {
            std::string type = shape_data["type"];

            auto mat = new Material();
            materials_.push_back(mat);
            bool excluded = false;
            Vector3 uv0 = Vector3(0,0,0);
            Vector3 uv1 = Vector3(1,0,0);
            Vector3 uv2 = Vector3(0,1,0);
            if(cam.render_mode != RenderMode::BINARY) {

                // Parse material
                auto mat_data = shape_data["material"];
                
                mat->kd = mat_data["kd"];
                mat->ks = mat_data["ks"];
                mat->specularexponent = mat_data["specularexponent"];
                mat->diffusecolor = Radiance(mat_data["diffusecolor"][0], mat_data["diffusecolor"][1], mat_data["diffusecolor"][2]);
                mat->specularcolor = Radiance(mat_data["specularcolor"][0], mat_data["specularcolor"][1], mat_data["specularcolor"][2]);
                mat->is_reflective = mat_data["isreflective"];
                mat->reflectivity = mat_data["reflectivity"];   
                mat->is_refractive = mat_data["isrefractive"];
                mat->refractiveindex = mat_data["refractiveindex"];
                if (mat_data.contains("texture")) {
                    mat->load_texture(mat_data["texture"]);
                    if (mat_data.contains("tile_factor_u")) {
                        mat->tile_factor_u = mat_data["tile_factor_u"];
                    if (mat_data.contains("tile_factor_v")) {
                        mat->tile_factor_v = mat_data["tile_factor_v"];
                        }
                    }
                }

                if (mat_data.contains("has_brdf")) {
                    mat->has_brdf = mat_data["has_brdf"];

                    if (mat->has_brdf) {
                        mat->base_color = Radiance(mat_data["base_color"][0], mat_data["base_color"][1], mat_data["base_color"][2]);
                        mat->roughness = mat_data["roughness"];
                        mat->metalness = mat_data["metalness"];
                    }
                }

                
                if (shape_data.contains("excluded")) {
                    excluded = shape_data["excluded"];
                }

                if (mat_data.contains("uv0") && mat_data.contains("uv1") && mat_data.contains("uv2")) {
                    uv0 = Vector3(mat_data["uv0"][0], mat_data["uv0"][1], 0.0);
                    uv1 = Vector3(mat_data["uv1"][0], mat_data["uv1"][1], 0.0);
                    uv2 = Vector3(mat_data["uv2"][0], mat_data["uv2"][1], 0.0);
                }
            }

            if (type == "sphere") {

                    world.add(make_shared<Sphere>(
                        Point3(shape_data["center"][0], shape_data["center"][1], shape_data["center"][2]),
                        shape_data["radius"],
                        mat,
                        excluded
                    ));
            
            } else if (type == "cylinder") {
                world.add(make_shared<Cylinder>(
                    Point3(shape_data["center"][0], shape_data["center"][1], shape_data["center"][2]),
                    Vector3(shape_data["axis"][0], shape_data["axis"][1], shape_data["axis"][2]),
                    shape_data["radius"],
                    shape_data["height"],
                    mat,
                    excluded
                ));
            } else if (type == "triangle") {
                // customized uv mapping
                world.add(make_shared<Triangle>(
                    Point3(shape_data["v0"][0], shape_data["v0"][1], shape_data["v0"][2]),
                    Point3(shape_data["v1"][0], shape_data["v1"][1], shape_data["v1"][2]),
                    Point3(shape_data["v2"][0], shape_data["v2"][1], shape_data["v2"][2]),
                    mat,
                    uv0, uv1, uv2,
                    excluded
                ));
            } else {
                std::cerr << "Warning: Unknown shape type '" << type << "', skipping." << std::endl;
                delete mat;
                materials_.pop_back();
                continue;
            }
            // mat->print_material(); // Print material properties
        }
    }

};

#endif // PARSER_H