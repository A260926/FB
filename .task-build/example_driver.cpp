#include <assimp/Exporter.hpp>
#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <cstdio>
#include <fstream>
#include <sstream>
int main(int argc, char **argv) {
    std::ifstream f(argv[1]);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string json = ss.str();

    Assimp::Importer importer;
    importer.SetPropertyBool(AI_CONFIG_PP_MORPH_FRAME_COMPLETION, true);
    const aiScene *scene = importer.ReadFileFromMemory(json.data(), json.size(), 0, "gltf");
    if (!scene) {
        std::fprintf(stderr, "import failed: %s\n", importer.GetErrorString());
        return 1;
    }
    scene = importer.ApplyPostProcessing(aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace);
    const aiMesh *mesh = scene->mMeshes[0];
    std::printf("targets=%u\n", mesh->mNumAnimMeshes);
    for (unsigned int t = 0; t < mesh->mNumAnimMeshes; ++t) {
        const aiAnimMesh *a = mesh->mAnimMeshes[t];
        std::printf("  %s normals=%d tangents=%d\n", a->mName.C_Str(), (int)a->HasNormals(),
                (int)a->HasTangentsAndBitangents());
    }

    Assimp::ExportProperties props;
    props.SetPropertyBool("GLTF2_TARGET_NORMAL_EXP", true);
    props.SetPropertyBool(AI_CONFIG_GLTF2_TARGET_TANGENT_EXP, true);
    props.SetPropertyBool("GLTF2_TARGETNAMES_EXP", true);
    Assimp::Exporter exporter;
    if (exporter.Export(scene, "gltf2", argv[2], 0u, &props) != aiReturn_SUCCESS) {
        std::fprintf(stderr, "export failed: %s\n", exporter.GetErrorString());
        return 2;
    }
    std::printf("exported %s\n", argv[2]);
    return 0;
}
