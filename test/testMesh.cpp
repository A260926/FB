#include "TestPrologue.h"
#include <cal3d/coremesh.h>
#include <cal3d/coresubmesh.h>
#include <cal3d/coremorphtarget.h>

FIXTURE(MeshFixture) {
};

TEST_F(MeshFixture, can_replace_base_mesh_with_morph_target) {
    CalCoreSubmesh::Face face(0, 0, 0);

    CalCoreSubmesh::Vertex vertex;
    vertex.position = CalPoint4(1, 1, 1);
    vertex.normal = CalVector4(1, 1, 1);

    CalCoreSubmesh::Influence influence;
    influence.boneId = 0;
    influence.weight = 1.0f;
    influence.lastInfluenceForThisVertex = 1;

    std::vector<CalCoreSubmesh::Influence> influences;
    influences.push_back(influence);

    CalCoreMesh mesh;
    ISTD::shared_ptr<CalCoreSubmesh> submesh = ISTD::make_shared<CalCoreSubmesh>(1, false, 1);
    submesh->addFace(face);
    submesh->addVertex(vertex, CalColor32(), influences);

    VertexOffset aVO;
    aVO.vertexId = 0;
    aVO.position = CalVector4(2, 2, 2);
    aVO.normal = CalVector4(2, 2, 2);

    CalCoreMorphTarget::VertexOffsetArray aVertexOffsets;
    aVertexOffsets.push_back(aVO);
    ISTD::shared_ptr<CalCoreMorphTarget> aTarget = ISTD::make_shared<CalCoreMorphTarget>("A", 1, aVertexOffsets);

    VertexOffset bVO;
    bVO.vertexId = 0;
    bVO.position = CalVector4(3, 3, 3);
    bVO.normal = CalVector4(3, 3, 3);

    CalCoreMorphTarget::VertexOffsetArray bVertexOffsets;
    bVertexOffsets.push_back(bVO);
    ISTD::shared_ptr<CalCoreMorphTarget> bTarget = ISTD::make_shared<CalCoreMorphTarget>("B", 1, bVertexOffsets);

    submesh->addMorphTarget(aTarget);
    submesh->addMorphTarget(bTarget);

    mesh.submeshes.push_back(submesh);
    
    CHECK_EQUAL(CalPoint4(1, 1, 1), submesh->getVectorVertex()[0].position);
    CHECK_EQUAL(CalVector4(1, 1, 1), submesh->getVectorVertex()[0].normal);

    mesh.replaceMeshWithMorphTarget("b"); // testing case insensitivity?
    CHECK_EQUAL(CalPoint4(1, 1, 1), submesh->getVectorVertex()[0].position);
    CHECK_EQUAL(CalVector4(1, 1, 1), submesh->getVectorVertex()[0].normal);

    mesh.replaceMeshWithMorphTarget("B"); // testing case insensitivity?
    CHECK_EQUAL(CalPoint4(4, 4, 4), submesh->getVectorVertex()[0].position);
    CHECK_EQUAL(CalVector4(4, 4, 4), submesh->getVectorVertex()[0].normal);
}

std::vector<CalCoreSubmesh::Influence> makeInfluences(unsigned boneId) {
    std::vector<CalCoreSubmesh::Influence> v;
    v.push_back(CalCoreSubmesh::Influence(boneId, 1.0f, true));
    return v;
}

TEST_F(MeshFixture, split_mesh_by_bone_limit) {
    CalCoreSubmesh::Vertex v;
    CalColor32 c = 0;

    ISTD::shared_ptr<CalCoreSubmesh> submesh = ISTD::make_shared<CalCoreSubmesh>(4, false, 2);
    submesh->addVertex(v, c, makeInfluences(0));
    submesh->addVertex(v, c, makeInfluences(1));
    submesh->addVertex(v, c, makeInfluences(2));
    submesh->addVertex(v, c, makeInfluences(3));
    submesh->addFace(CalCoreSubmesh::Face(0, 1, 2));
    submesh->addFace(CalCoreSubmesh::Face(0, 1, 3));

    CalCoreMesh mesh;
    mesh.submeshes.push_back(submesh);
    mesh.splitSubmeshesToBoneLimit(3);

    CHECK_EQUAL(2u, mesh.submeshes.size());
    auto sm0 = mesh.submeshes[0];
    auto sm1 = mesh.submeshes[1];
    CHECK_EQUAL(1u, sm0->getFaceCount());
    CHECK_EQUAL(1u, sm1->getFaceCount());

    CHECK_EQUAL(4u, sm0->getVertexCount());
    CHECK_EQUAL(4u, sm1->getVertexCount());

    sm0->renumberIndices();
    sm1->renumberIndices();

    CHECK_EQUAL(3u, sm0->getVertexCount());
    CHECK_EQUAL(3u, sm1->getVertexCount());
}
