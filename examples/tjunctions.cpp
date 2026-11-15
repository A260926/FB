/*
---------------------------------------------------------------------------
Open Asset Import Library (assimp)
---------------------------------------------------------------------------

Demonstrates the AI_CONFIG_PP_TRI_RESOLVE_TJUNCTIONS option: loads
task_support/tjunction.obj, triangulates it with T-junction resolution
enabled and writes the resulting mesh to the OBJ file given as argv[1].
---------------------------------------------------------------------------
*/

#include <assimp/Exporter.hpp>
#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <iostream>

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <output-obj>\n";
        return 1;
    }

    Assimp::Importer importer;
    importer.SetPropertyBool(AI_CONFIG_PP_TRI_RESOLVE_TJUNCTIONS, true);
    importer.SetPropertyFloat(AI_CONFIG_PP_TRI_TJUNCTION_EPSILON,
            AI_CONFIG_PP_TRI_TJUNCTION_EPSILON_DEFAULT);

    const aiScene *scene = importer.ReadFile("task_support/tjunction.obj",
            aiProcess_Triangulate | aiProcess_JoinIdenticalVertices);
    if (nullptr == scene) {
        std::cerr << "import failed: " << importer.GetErrorString() << '\n';
        return 2;
    }

    Assimp::Exporter exporter;
    if (aiReturn_SUCCESS != exporter.Export(scene, "obj", argv[1])) {
        std::cerr << "export failed: " << exporter.GetErrorString() << '\n';
        return 3;
    }

    std::cout << "TJUNCTION_RESOLVE_OK meshes=" << scene->mNumMeshes << '\n';
    return 0;
}
