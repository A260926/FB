#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <cstdio>
int main(int argc, char **argv) {
    Assimp::Importer imp;
    imp.SetPropertyBool(AI_CONFIG_PP_MORPH_FRAME_COMPLETION, true);
    const aiScene *s = imp.ReadFile(argv[1], 0);
    if (!s) {
        std::fprintf(stderr, "ERR %s\n", imp.GetErrorString());
        return 1;
    }
    s = imp.ApplyPostProcessing(aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace);
    const aiMesh *m = s->mMeshes[0];
    std::printf("meshes=%u verts=%u targets=%u\n", s->mNumMeshes, m->mNumVertices, m->mNumAnimMeshes);
    for (unsigned t = 0; t < m->mNumAnimMeshes; ++t) {
        const aiAnimMesh *a = m->mAnimMeshes[t];
        std::printf("target %u name='%s' w=%g N=%d T=%d B=%d\n", t, a->mName.C_Str(), a->mWeight,
                (int)a->HasNormals(), (int)(a->mTangents != nullptr), (int)(a->mBitangents != nullptr));
    }
    return 0;
}
