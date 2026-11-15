// Tests for TinyScene's CPU morph + skin deformation path.
//
// The production code is included through task_support/scene_api.hpp, exactly as it
// is compiled for the real target. Each case builds procedural meshes and skeletons,
// computes the expected vertices / normals independently (morph first, then four
// weight linear skinning) and checks the scene's per-node output, the untouched source
// data, stable instance identity, occlusion and a real TLAS render.
//
// Cases are registered in the "stages", "instances" and "render" groups expected by
// task_support/driver.c.

#include "task_support/scene_api.hpp"
#include "task_support/check.h"

#include <cstdio>
#include <cstdlib>

using namespace tinyscene;
using namespace tinybvh;

// ---------------------------------------------------------------------------
// Small matrix helpers, deliberately independent of the scene implementation.
// ---------------------------------------------------------------------------
static ts_mat4 MatTranslate( const ts_vec3 t ) { ts_mat4 m; m[3] = t.x, m[7] = t.y, m[11] = t.z; return m; }
static ts_mat4 MatScale( const ts_vec3 s ) { ts_mat4 m; m[0] = s.x, m[5] = s.y, m[10] = s.z; return m; }
static ts_mat4 MatRotY( const float a )
{
	ts_mat4 m; const float c = cosf( a ), s = sinf( a );
	m[0] = c, m[2] = -s, m[8] = s, m[10] = c;
	return m;
}
static ts_vec3 XformPoint( const ts_vec3& v, const ts_mat4& T )
{
	const ts_vec3 r( T[0] * v.x + T[1] * v.y + T[2] * v.z + T[3],
		T[4] * v.x + T[5] * v.y + T[6] * v.z + T[7],
		T[8] * v.x + T[9] * v.y + T[10] * v.z + T[11] );
	const float w = T[12] * v.x + T[13] * v.y + T[14] * v.z + T[15];
	return w == 1.0f ? r : r * (1.0f / w);
}
static ts_vec3 XformVector( const ts_vec3& v, const ts_mat4& T )
{
	return ts_vec3( T[0] * v.x + T[1] * v.y + T[2] * v.z, T[4] * v.x + T[5] * v.y + T[6] * v.z,
		T[8] * v.x + T[9] * v.y + T[10] * v.z );
}
static ts_mat4 Inv( const ts_mat4& m );
static ts_mat4 Inverse( const ts_mat4& m ) { return Inv( m ); }
static ts_mat4 NormalMatrix( const ts_mat4& m )
{
	const ts_mat4 inv = Inverse( m );
	ts_mat4 t = inv;
	for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) t.cell[i * 4 + j] = inv.cell[j * 4 + i];
	return t;
}
static bool Near( const float a, const float b, const float eps = 2e-3f ) { return fabsf( a - b ) <= eps; }
static bool NearV( const ts_vec3& a, const ts_vec3& b, const float eps = 2e-3f )
{
	return Near( a.x, b.x, eps ) && Near( a.y, b.y, eps ) && Near( a.z, b.z, eps );
}

// The test is compiled against TinyScene's declarations only, so the scene's own
// math helpers are not visible here. These local versions keep the reference
// implementation independent of the code under test.
static ts_vec3 Cross( const ts_vec3& a, const ts_vec3& b )
{
	return ts_vec3( a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x );
}
static float Dot( const ts_vec3& a, const ts_vec3& b ) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static ts_vec3 Norm( const ts_vec3& a )
{
	const float l = sqrtf( Dot( a, a ) );
	return l == 0.0f ? ts_vec3( 0 ) : a * (1.0f / l);
}
static ts_mat4 Mul( const ts_mat4& a, const ts_mat4& b )
{
	ts_mat4 r;
	for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++)
		r.cell[i * 4 + j] = a.cell[i * 4 + 0] * b.cell[0 * 4 + j] + a.cell[i * 4 + 1] * b.cell[1 * 4 + j] +
			a.cell[i * 4 + 2] * b.cell[2 * 4 + j] + a.cell[i * 4 + 3] * b.cell[3 * 4 + j];
	return r;
}
static float FMax( const float a, const float b ) { return a > b ? a : b; }
static float FMin( const float a, const float b ) { return a < b ? a : b; }

// 4x4 inverse (cofactor expansion); only used on the deliberately invertible
// matrices in this test.
static ts_mat4 Inv( const ts_mat4& m )
{
	const float* M = m.cell;
	float inv[16];
	inv[0] = M[5]*M[10]*M[15] - M[5]*M[11]*M[14] - M[9]*M[6]*M[15] + M[9]*M[7]*M[14] + M[13]*M[6]*M[11] - M[13]*M[7]*M[10];
	inv[4] = -M[4]*M[10]*M[15] + M[4]*M[11]*M[14] + M[8]*M[6]*M[15] - M[8]*M[7]*M[14] - M[12]*M[6]*M[11] + M[12]*M[7]*M[10];
	inv[8] = M[4]*M[9]*M[15] - M[4]*M[11]*M[13] - M[8]*M[5]*M[15] + M[8]*M[7]*M[13] + M[12]*M[5]*M[11] - M[12]*M[7]*M[9];
	inv[12] = -M[4]*M[9]*M[14] + M[4]*M[10]*M[13] + M[8]*M[5]*M[14] - M[8]*M[6]*M[13] - M[12]*M[5]*M[10] + M[12]*M[6]*M[9];
	inv[1] = -M[1]*M[10]*M[15] + M[1]*M[11]*M[14] + M[9]*M[2]*M[15] - M[9]*M[3]*M[14] - M[13]*M[2]*M[11] + M[13]*M[3]*M[10];
	inv[5] = M[0]*M[10]*M[15] - M[0]*M[11]*M[14] - M[8]*M[2]*M[15] + M[8]*M[3]*M[14] + M[12]*M[2]*M[11] - M[12]*M[3]*M[10];
	inv[9] = -M[0]*M[9]*M[15] + M[0]*M[11]*M[13] + M[8]*M[1]*M[15] - M[8]*M[3]*M[13] - M[12]*M[1]*M[11] + M[12]*M[3]*M[9];
	inv[13] = M[0]*M[9]*M[14] - M[0]*M[10]*M[13] - M[8]*M[1]*M[14] + M[8]*M[2]*M[13] + M[12]*M[1]*M[10] - M[12]*M[2]*M[9];
	inv[2] = M[1]*M[6]*M[15] - M[1]*M[7]*M[14] - M[5]*M[2]*M[15] + M[5]*M[3]*M[14] + M[13]*M[2]*M[7] - M[13]*M[3]*M[6];
	inv[6] = -M[0]*M[6]*M[15] + M[0]*M[7]*M[14] + M[4]*M[2]*M[15] - M[4]*M[3]*M[14] - M[12]*M[2]*M[7] + M[12]*M[3]*M[6];
	inv[10] = M[0]*M[5]*M[15] - M[0]*M[7]*M[13] - M[4]*M[1]*M[15] + M[4]*M[3]*M[13] + M[12]*M[1]*M[7] - M[12]*M[3]*M[5];
	inv[14] = -M[0]*M[5]*M[14] + M[0]*M[6]*M[13] + M[4]*M[1]*M[14] - M[4]*M[2]*M[13] - M[12]*M[1]*M[6] + M[12]*M[2]*M[5];
	inv[3] = -M[1]*M[6]*M[11] + M[1]*M[7]*M[10] + M[5]*M[2]*M[11] - M[5]*M[3]*M[10] - M[9]*M[2]*M[7] + M[9]*M[3]*M[6];
	inv[7] = M[0]*M[6]*M[11] - M[0]*M[7]*M[10] - M[4]*M[2]*M[11] + M[4]*M[3]*M[10] + M[8]*M[2]*M[7] - M[8]*M[3]*M[6];
	inv[11] = -M[0]*M[5]*M[11] + M[0]*M[7]*M[9] + M[4]*M[1]*M[11] - M[4]*M[3]*M[9] - M[8]*M[1]*M[7] + M[8]*M[3]*M[5];
	inv[15] = M[0]*M[5]*M[10] - M[0]*M[6]*M[9] - M[4]*M[1]*M[10] + M[4]*M[2]*M[9] + M[8]*M[1]*M[6] - M[8]*M[2]*M[5];
	const float det = M[0]*inv[0] + M[1]*inv[4] + M[2]*inv[8] + M[3]*inv[12];
	ts_mat4 r;
	if (det != 0.0f) { const float invDet = 1.0f / det; for (int i = 0; i < 16; i++) r.cell[i] = inv[i] * invDet; }
	return r;
}
static ts_mat4 ScaleMat( const float f, const ts_mat4& m )
{
	ts_mat4 r;
	for (int i = 0; i < 16; i++) r.cell[i] = m.cell[i] * f;
	return r;
}

// Ray / triangle intersection (Möller–Trumbore). Returns true and fills t, u, v when
// the ray hits the triangle within [0, tMax). Used only by the independent reference.
static bool RayTri( const ts_vec3& O, const ts_vec3& D, const ts_vec3& a, const ts_vec3& b,
	const ts_vec3& c, const float tMax, float& t, float& u, float& v )
{
	const ts_vec3 e1 = b - a, e2 = c - a;
	const ts_vec3 pv = Cross( D, e2 );
	const float det = Dot( e1, pv );
	if ( fabsf( det ) < 1e-9f ) return false;
	const float invDet = 1.0f / det;
	const ts_vec3 tv = O - a;
	u = Dot( tv, pv ) * invDet;
	if ( u < 0.0f || u > 1.0f ) return false;
	const ts_vec3 qv = Cross( tv, e1 );
	v = Dot( D, qv ) * invDet;
	if ( v < 0.0f || u + v > 1.0f ) return false;
	t = Dot( e2, qv ) * invDet;
	return t > 1e-4f && t < tMax;
}

// ---------------------------------------------------------------------------
// Independent reference deformation: morph first, then four-weight skinning.
// ---------------------------------------------------------------------------
struct RefMesh
{
	::std::vector<ts_vec3> basePos;		// source positions, 3 for each triangle
	::std::vector<ts_vec3> baseNrm;		// source vertex normals, 3 for each triangle
	::std::vector<Mesh::Pose> poses;	// morph poses; [0] is the base pose
	::std::vector<ts_uint4> joints;
	::std::vector<ts_vec4> weights;
};

struct RefPose
{
	::std::vector<ts_vec3> pos;
	::std::vector<ts_vec3> nrm;
};

static RefMesh CaptureMesh( const int meshId )
{
	const Mesh* m = Scene::meshPool[meshId];
	RefMesh r;
	for (int t = 0; t < (int)m->triangles.size(); t++)
	{
		const FatTri& tri = m->triangles[t];
		r.basePos.push_back( tri.vertex0 );
		r.basePos.push_back( tri.vertex1 );
		r.basePos.push_back( tri.vertex2 );
		r.baseNrm.push_back( tri.vN0 );
		r.baseNrm.push_back( tri.vN1 );
		r.baseNrm.push_back( tri.vN2 );
	}
	r.poses = m->poses;
	r.joints = m->joints;
	r.weights = m->weights;
	return r;
}

static RefPose ReferenceDeform( const RefMesh& mesh, const ::std::vector<float>& morphWeights,
	const ::std::vector<ts_mat4>* jointMat )
{
	RefPose out;
	const int vertCount = (int)mesh.basePos.size();
	out.pos.resize( vertCount );
	out.nrm.resize( vertCount );
	const int poseCount = (int)mesh.poses.size();
	for (int i = 0; i < vertCount; i++)
	{
		ts_vec3 p = poseCount > 0 ? mesh.poses[0].positions[i] : mesh.basePos[i];
		ts_vec3 n = poseCount > 0 ? mesh.poses[0].normals[i] : mesh.baseNrm[i];
		for (int w = 0; w < (int)morphWeights.size() && w + 1 < poseCount; w++)
		{
			p = p + morphWeights[w] * mesh.poses[w + 1].positions[i];
			n = n + morphWeights[w] * mesh.poses[w + 1].normals[i];
		}
		if (jointMat && i < (int)mesh.weights.size())
		{
			const ts_uint4 j4 = mesh.joints[i];
			const ts_vec4 w4 = mesh.weights[i];
			ts_mat4 S = ScaleMat( w4.x, (*jointMat)[j4.x] );
			S += ScaleMat( w4.y, (*jointMat)[j4.y] );
			S += ScaleMat( w4.z, (*jointMat)[j4.z] );
			S += ScaleMat( w4.w, (*jointMat)[j4.w] );
			p = XformPoint( p, S );
			n = XformVector( n, NormalMatrix( S ) );
		}
		out.pos[i] = p;
		out.nrm[i] = Norm( n );
	}
	return out;
}

static ts_vec3 RefGeometricNormal( const RefPose& pose, const int tri )
{
	return Norm( Cross( pose.pos[tri * 3 + 1] - pose.pos[tri * 3 + 0],
		pose.pos[tri * 3 + 2] - pose.pos[tri * 3 + 0] ) );
}

static ::std::vector<ts_mat4> ReferenceJointMatrices( const Skin* skin, const ts_mat4& meshWorld )
{
	const ts_mat4 invMesh = Inverse( meshWorld );
	::std::vector<ts_mat4> out;
	for (int j = 0; j < (int)skin->joints.size(); j++)
	{
		const ts_mat4& ibm = j < (int)skin->inverseBindMatrices.size() ? skin->inverseBindMatrices[j] : ts_mat4();
		out.push_back( Mul( Mul( invMesh, Scene::nodePool[skin->joints[j]]->combinedTransform ), ibm ) );
	}
	return out;
}

// ---------------------------------------------------------------------------
// Scene construction.
// ---------------------------------------------------------------------------

// Cases run in one process against the static Scene, so start each case from an empty
// instance set. Meshes / nodes from earlier cases remain allocated but are unreachable
// (rootNodes is cleared), which keeps node and mesh ids stable and non-reused.
static void BeginCase()
{
	Scene::rootNodes.clear();
	Scene::instPool.clear();
	Scene::instNodeId.clear();
	Scene::blasList.clear();
}

static int AddRoot( Node* node )
{
	const int id = Scene::AddNode( node );
	Scene::AddInstance( id );
	return id;
}

// A 3x3-vertex grid in the XZ plane, 2x2 cells, split into 8 triangles. The winding
// gives every triangle an upward (+Y) normal, and the source triangles get their own
// vertex normals and UVs.
static const int kGridTris = 8;
static const int kGridVerts = kGridTris * 3;

static int BuildGridMesh( const int matId, const float size = 1.0f )
{
	const int meshId = Scene::AddMesh( 0 );
	ts_vec3 g[3][3];
	for (int z = 0; z < 3; z++) for (int x = 0; x < 3; x++)
		g[z][x] = ts_vec3( (x - 1) * size, 0.0f, (z - 1) * size );
	for (int z = 0; z < 2; z++) for (int x = 0; x < 2; x++)
	{
		Scene::AddTriToMesh( meshId, g[z][x], g[z + 1][x], g[z][x + 1], matId );
		Scene::AddTriToMesh( meshId, g[z][x + 1], g[z + 1][x], g[z + 1][x + 1], matId );
	}
	Mesh* m = Scene::meshPool[meshId];
	for (int t = 0; t < (int)m->triangles.size(); t++)
	{
		FatTri& tri = m->triangles[t];
		const ts_vec3 N = Norm( Cross( tri.vertex1 - tri.vertex0, tri.vertex2 - tri.vertex0 ) );
		tri.vN0 = tri.vN1 = tri.vN2 = N;
		tri.Nx = N.x, tri.Ny = N.y, tri.Nz = N.z;
		tri.u0 = 0.0f, tri.v0 = 0.0f;
		tri.u1 = 1.0f, tri.v1 = 0.0f;
		tri.u2 = 0.0f, tri.v2 = 1.0f;
		tri.material = matId;
	}
	m->BuildMaterialList();
	return meshId;
}

// Two morph targets: a +X wedge and an X-dependent +Y / +Z bulge. The second target
// optionally carries normal deltas.
static void InstallMorphTargets( const int meshId, const bool withNormals = true )
{
	Mesh* m = Scene::meshPool[meshId];
	Mesh::Pose base, target1, target2;
	for (int i = 0; i < (int)m->vertices.size(); i++)
	{
		const ts_vec3 p = ts_vec3( m->vertices[i] );
		base.positions.push_back( p );
		base.normals.push_back( ts_vec3( 0, 1, 0 ) );
		target1.positions.push_back( ts_vec3( 0.5f, 0, 0 ) );
		target1.normals.push_back( ts_vec3( 0, 0, 0 ) );
		target2.positions.push_back( ts_vec3( 0.0f, 0.25f + 0.1f * p.x, 0.05f * p.z ) );
		target2.normals.push_back( withNormals ? ts_vec3( 0.2f * p.x, 0.0f, 0.0f ) : ts_vec3( 0, 0, 0 ) );
	}
	m->poses = { base, target1, target2 };
}

// Bind every vertex of a mesh to four joints with non-negative weights summing to 1.
static void BindWeights( const int meshId, const int jointCount )
{
	Mesh* m = Scene::meshPool[meshId];
	m->joints.clear();
	m->weights.clear();
	for (int i = 0; i < (int)m->vertices.size(); i++)
	{
		const int a = i % jointCount, b = (i + 1) % jointCount;
		const int c = (i + 2) % jointCount, d = (i + 3) % jointCount;
		m->joints.push_back( ts_uint4( a, b, c, d ) );
		switch (i % 4)
		{
		case 0: m->weights.push_back( ts_vec4( 0.4f, 0.3f, 0.2f, 0.1f ) ); break;
		case 1: m->weights.push_back( ts_vec4( 1.0f, 0.0f, 0.0f, 0.0f ) ); break;
		case 2: m->weights.push_back( ts_vec4( 0.25f, 0.25f, 0.25f, 0.25f ) ); break;
		default: m->weights.push_back( ts_vec4( 0.0f, 0.5f, 0.5f, 0.0f ) ); break;
		}
	}
}

static int AddSkin( const ::std::vector<int>& jointIds, const ::std::vector<ts_mat4>& inverseBind )
{
	Skin* skin = new Skin();
	skin->joints = jointIds;
	skin->inverseBindMatrices = inverseBind;
	skin->jointMat.resize( jointIds.size() );
	const int id = (int)Scene::skins.size();
	Scene::skins.push_back( skin );
	return id;
}

// Compare a node's current output against the independently computed reference.
static bool NodeMatchesReference( const int nodeId, const RefPose& ref, const int triCount )
{
	for (int t = 0; t < triCount; t++)
	{
		const FatTri* tri = Scene::GetNodeTriangle( nodeId, t );
		if (!tri) return false;
		if (!NearV( tri->vertex0, ref.pos[t * 3 + 0] )) return false;
		if (!NearV( tri->vertex1, ref.pos[t * 3 + 1] )) return false;
		if (!NearV( tri->vertex2, ref.pos[t * 3 + 2] )) return false;
		if (!NearV( tri->vN0, ref.nrm[t * 3 + 0] )) return false;
		if (!NearV( tri->vN1, ref.nrm[t * 3 + 1] )) return false;
		if (!NearV( tri->vN2, ref.nrm[t * 3 + 2] )) return false;
		if (!NearV( ts_vec3( tri->Nx, tri->Ny, tri->Nz ), RefGeometricNormal( ref, t ) )) return false;
	}
	return true;
}

// ===========================================================================
// Group: stages
// ===========================================================================

// Morph and skin on a single chain: non-identity inverse bind matrices, a joint as a
// child of another joint, and negative morph weights.
static int CaseMorphThenSkin()
{
	BeginCase();
	const int mat = Scene::AddMaterial( ts_vec3( 0.7f, 0.6f, 0.5f ), "stage_mat" );
	const int meshId = BuildGridMesh( mat );
	InstallMorphTargets( meshId );

	const int rootJoint = AddRoot( new Node() );
	Scene::nodePool[rootJoint]->localTransform = MatTranslate( ts_vec3( 0.0f, 0.5f, 0.0f ) );
	const int childJoint = Scene::AddNode( new Node() );
	Scene::nodePool[childJoint]->localTransform = MatTranslate( ts_vec3( 0.4f, 0.0f, 0.0f ) );
	Scene::AddChildNode( rootJoint, childJoint );
	// A second root so the mesh node is part of the graph.
	const int meshRoot = AddRoot( new Node() );
	Scene::nodePool[meshRoot]->localTransform = ts_mat4();

	ts_mat4 ibm[4];
	ibm[0] = MatTranslate( ts_vec3( -0.1f, 0.2f, 0.05f ) );
	ibm[1] = Mul( MatRotY( 0.3f ), MatScale( ts_vec3( 1.1f, 0.9f, 1.2f ) ) );
	ibm[2] = Mul( MatTranslate( ts_vec3( 0.15f, -0.1f, 0.0f ) ), MatRotY( -0.25f ) );
	ibm[3] = MatScale( ts_vec3( 0.9f, 1.2f, 0.8f ) );
	const int skinId = AddSkin( { rootJoint, childJoint, rootJoint, childJoint }, { ibm[0], ibm[1], ibm[2], ibm[3] } );
	BindWeights( meshId, 2 ); // four indices, two distinct joints

	Node* meshNode = new Node( meshId, Mul( Mul( MatTranslate( ts_vec3( 1.0f, 2.0f, -1.0f ) ),
		MatRotY( 0.4f ) ), MatScale( ts_vec3( 1.3f, 0.8f, 1.1f ) ) ) );
	meshNode->skinID = skinId;
	meshNode->weights = { 0.35f, -0.2f };
	const int meshNodeId = Scene::AddNode( meshNode );
	Scene::AddChildNode( meshRoot, meshNodeId );

	Scene::UpdateSceneGraph( 0.0f );

	const ts_mat4 world = Scene::nodePool[meshNodeId]->combinedTransform;
	::std::vector<ts_mat4> jm = ReferenceJointMatrices( Scene::skins[skinId], world );
	const RefMesh refMesh = CaptureMesh( meshId );
	const RefPose ref = ReferenceDeform( refMesh, meshNode->weights, &jm );

	TASK_CHECK( NodeMatchesReference( meshNodeId, ref, kGridTris ) );

	// Source binding data is untouched.
	const Mesh* m = Scene::meshPool[meshId];
	TASK_CHECK( (int)m->vertices.size() == kGridVerts );
	TASK_CHECK( (int)m->triangles.size() == kGridTris );
	for (int i = 0; i < kGridVerts; i++)
		if (!NearV( ts_vec3( m->vertices[i] ), refMesh.basePos[i] )) return 1;
	TASK_CHECK( (int)m->poses.size() == 3 );
	TASK_CHECK( (int)m->joints.size() == kGridVerts );
	TASK_CHECK( (int)m->weights.size() == kGridVerts );
	// Per-node shading data keeps shared material / UV information.
	const FatTri* tri = Scene::GetNodeTriangle( meshNodeId, 0 );
	TASK_CHECK( tri != nullptr );
	TASK_CHECK( tri->material == (uint32_t)mat );
	TASK_CHECK( Near( tri->u1, m->triangles[0].u1 ) && Near( tri->v2, m->triangles[0].v2 ) );
	// A deformed node must not use the source BLAS slot.
	int inst = -1;
	for (unsigned i = 0; i < (unsigned)Scene::instPool.size(); i++)
		if (Scene::NodeIdFromInstance( i ) == meshNodeId) inst = (int)i;
	TASK_CHECK( inst >= 0 );
	TASK_CHECK( Scene::instPool[inst].blasIdx != (unsigned)meshId );
	return 0;
}

// A joint can live under a different root than the skinned mesh; the result must not
// depend on the order in which roots / children are visited.
static int CaseCrossRootJointOrder()
{
	BeginCase();
	const int mat = Scene::AddMaterial( ts_vec3( 0.4f, 0.8f, 0.6f ), "order_mat" );
	const int meshId = BuildGridMesh( mat );
	InstallMorphTargets( meshId );

	const int rootA = AddRoot( new Node() );
	Scene::nodePool[rootA]->localTransform = MatTranslate( ts_vec3( 0.0f, 1.0f, 0.0f ) );
	const int jointId = Scene::AddNode( new Node() );
	Scene::nodePool[jointId]->localTransform = Mul( MatTranslate( ts_vec3( 0.25f, 0.0f, 0.1f ) ), MatRotY( 0.6f ) );
	Scene::AddChildNode( rootA, jointId );

	const int rootB = AddRoot( new Node() );
	Scene::nodePool[rootB]->localTransform = MatTranslate( ts_vec3( -0.5f, 0.0f, 0.5f ) );

	ts_mat4 ibm[2] = { MatTranslate( ts_vec3( 0.05f, -0.1f, 0.0f ) ),
		Mul( MatScale( ts_vec3( 1.2f, 0.85f, 1.0f ) ), MatRotY( -0.35f ) ) };
	const int skinId = AddSkin( { jointId, jointId }, { ibm[0], ibm[1] } );
	BindWeights( meshId, 2 );

	Node* meshNode = new Node( meshId, Mul( MatTranslate( ts_vec3( 0.75f, 0.4f, 0.0f ) ), MatScale( ts_vec3( 1.1f, 1.2f, 0.9f ) ) ) );
	meshNode->skinID = skinId;
	meshNode->weights = { 0.6f, 0.15f };
	const int meshNodeId = Scene::AddNode( meshNode );
	Scene::AddChildNode( rootB, meshNodeId );
	// Give each root more than one child so child order can be swapped as well.
	const int rootBChild2 = Scene::AddNode( new Node( meshId, MatTranslate( ts_vec3( 5.0f, 0.0f, 0.0f ) ) ) );
	Scene::AddChildNode( rootB, rootBChild2 );

	// Roots registered in reverse declaration order.
	Scene::rootNodes.clear();
	Scene::rootNodes.push_back( rootB );
	Scene::rootNodes.push_back( rootA );
	Scene::UpdateSceneGraph( 0.0f );

	const ts_mat4 world = Scene::nodePool[meshNodeId]->combinedTransform;
	::std::vector<ts_mat4> jm = ReferenceJointMatrices( Scene::skins[skinId], world );
	const RefMesh refMesh = CaptureMesh( meshId );
	const RefPose ref = ReferenceDeform( refMesh, meshNode->weights, &jm );
	TASK_CHECK( NodeMatchesReference( meshNodeId, ref, kGridTris ) );
	const ts_mat4 worldRef = world;

	// Swap root order: transforms (and therefore the pose) must be identical.
	Scene::rootNodes.clear();
	Scene::rootNodes.push_back( rootA );
	Scene::rootNodes.push_back( rootB );
	Scene::UpdateSceneGraph( 0.0f );
	TASK_CHECK( NodeMatchesReference( meshNodeId, ref, kGridTris ) );
	for (int i = 0; i < 16; i++) TASK_CHECK( Near( Scene::nodePool[meshNodeId]->combinedTransform[i], worldRef[i] ) );

	// Swap the children of rootB (and of the joint's root) as well: the pose of the
	// mesh node must not depend on the child traversal order either.
	::std::vector<int> childB = Scene::nodePool[rootB]->childIdx;
	Scene::nodePool[rootB]->childIdx.clear();
	for (int i = (int)childB.size() - 1; i >= 0; i--) Scene::nodePool[rootB]->childIdx.push_back( childB[i] );
	Scene::rootNodes.clear();
	Scene::rootNodes.push_back( rootB );
	Scene::rootNodes.push_back( rootA );
	Scene::UpdateSceneGraph( 0.0f );
	TASK_CHECK( NodeMatchesReference( meshNodeId, ref, kGridTris ) );
	for (int i = 0; i < 16; i++) TASK_CHECK( Near( Scene::nodePool[meshNodeId]->combinedTransform[i], worldRef[i] ) );
	return 0;
}

// Negative morph weights combined with a second target, before skinning.
static int CaseNegativeMorph()
{
	BeginCase();
	const int mat = Scene::AddMaterial( ts_vec3( 0.9f, 0.5f, 0.2f ), "neg_morph_mat" );
	const int meshId = BuildGridMesh( mat );
	InstallMorphTargets( meshId );

	const int jA = AddRoot( new Node() );
	Scene::nodePool[jA]->localTransform = MatTranslate( ts_vec3( 0.2f, 0.0f, 0.0f ) );
	const int jB = Scene::AddNode( new Node() );
	Scene::nodePool[jB]->localTransform = MatTranslate( ts_vec3( 0.0f, 0.3f, 0.0f ) );
	Scene::AddChildNode( jA, jB );
	ts_mat4 ibm[2] = { MatRotY( 0.2f ), MatTranslate( ts_vec3( -0.1f, 0.05f, 0.1f ) ) };
	const int skinId = AddSkin( { jA, jB }, { ibm[0], ibm[1] } );
	BindWeights( meshId, 2 );

	const int meshRoot = AddRoot( new Node() );
	Node* meshNode = new Node( meshId, MatTranslate( ts_vec3( 0.0f, 0.5f, 0.0f ) ) );
	meshNode->skinID = skinId;
	meshNode->weights = { -0.75f, 0.4f };
	const int meshNodeId = Scene::AddNode( meshNode );
	Scene::AddChildNode( meshRoot, meshNodeId );

	Scene::UpdateSceneGraph( 0.0f );
	const ts_mat4 world = Scene::nodePool[meshNodeId]->combinedTransform;
	::std::vector<ts_mat4> jm = ReferenceJointMatrices( Scene::skins[skinId], world );
	const RefMesh refMesh = CaptureMesh( meshId );
	const RefPose ref = ReferenceDeform( refMesh, meshNode->weights, &jm );
	TASK_CHECK( NodeMatchesReference( meshNodeId, ref, kGridTris ) );
	// The negative weight must actually move the source geometry (a non-trivial pose).
	TASK_CHECK( !NearV( ref.pos[0], refMesh.basePos[0] ) );
	return 0;
}

// Three consecutive animated frames, then a return to the rest pose. Nothing may
// accumulate and the rest pose must match the untouched source geometry.
static int CaseThreeFramesThenRest()
{
	BeginCase();
	const int mat = Scene::AddMaterial( ts_vec3( 0.3f, 0.4f, 0.9f ), "frames_mat" );
	const int meshId = BuildGridMesh( mat );
	InstallMorphTargets( meshId );

	const int jointId = AddRoot( new Node() );
	Scene::nodePool[jointId]->localTransform = MatTranslate( ts_vec3( 0.1f, 0.0f, 0.0f ) );
	const int meshRoot = AddRoot( new Node() );
	Node* meshNode = new Node( meshId, ts_mat4() );
	meshNode->weights.resize( 2, 0.0f );
	const int meshNodeId = Scene::AddNode( meshNode );
	Scene::AddChildNode( meshRoot, meshNodeId );

	const RefMesh refMesh = CaptureMesh( meshId );
	const RefPose rest = ReferenceDeform( refMesh, meshNode->weights, nullptr );

	for (int frame = 0; frame < 3; frame++)
	{
		meshNode->weights = { 0.25f + 0.2f * frame, -0.1f * frame + 0.05f };
		Scene::nodePool[jointId]->localTransform = MatTranslate( ts_vec3( 0.1f + 0.15f * frame, 0.2f * frame, 0.0f ) );
		Scene::nodePool[jointId]->transformed = true;
		Scene::UpdateSceneGraph( 0.0f );
		const RefPose frameRef = ReferenceDeform( refMesh, meshNode->weights, nullptr );
		TASK_CHECK( NodeMatchesReference( meshNodeId, frameRef, kGridTris ) );
	}
	// Back to rest: exactly the source geometry, not an accumulated pose.
	meshNode->weights = { 0.0f, 0.0f };
	Scene::nodePool[jointId]->localTransform = MatTranslate( ts_vec3( 0.1f, 0.0f, 0.0f ) );
	Scene::UpdateSceneGraph( 0.0f );
	TASK_CHECK( NodeMatchesReference( meshNodeId, rest, kGridTris ) );
	// The node is rigid again, so it shares the source BLAS once more.
	{
		int inst = -1;
		for (unsigned i = 0; i < (unsigned)Scene::instPool.size(); i++)
			if (Scene::NodeIdFromInstance( i ) == meshNodeId) inst = (int)i;
		TASK_CHECK( inst >= 0 );
		TASK_CHECK( Scene::instPool[inst].blasIdx == (unsigned)meshId );
		// A ray from above hits the rest-position source triangle.
		Ray ray( bvhvec3( -0.9f, 9.0f, -0.99f ), bvhvec3( 0, -1, 0 ) );
		Scene::tlas->Intersect( ray );
		TASK_CHECK( ray.hit.t < 1000.0f );
		TASK_CHECK( Scene::NodeIdFromInstance( (unsigned)ray.hit.inst ) == meshNodeId );
	}

	const Mesh* m = Scene::meshPool[meshId];
	for (int i = 0; i < kGridVerts; i++) if (!NearV( ts_vec3( m->vertices[i] ), refMesh.basePos[i] )) return 1;
	TASK_CHECK( (int)m->triangles.size() == kGridTris );
	return 0;
}

// ===========================================================================
// Group: instances
// ===========================================================================

// Four nodes share one mesh: two with different skins, one morph-only, one fully
// rigid. Every node keeps its own output and the deformers do not overwrite the
// first instance.
static int CaseSharedMeshDistinctSkins()
{
	BeginCase();
	const int mat = Scene::AddMaterial( ts_vec3( 0.8f, 0.7f, 0.4f ), "shared_mat" );
	const int meshId = BuildGridMesh( mat );
	InstallMorphTargets( meshId );

	// Skeleton 1.
	const int j1 = AddRoot( new Node() );
	Scene::nodePool[j1]->localTransform = MatTranslate( ts_vec3( 0.3f, 0.0f, 0.0f ) );
	const int j2 = Scene::AddNode( new Node() );
	Scene::nodePool[j2]->localTransform = MatTranslate( ts_vec3( 0.0f, 0.4f, 0.0f ) );
	Scene::AddChildNode( j1, j2 );
	ts_mat4 ibm1[2] = { MatRotY( 0.15f ), MatTranslate( ts_vec3( 0.1f, 0.0f, 0.0f ) ) };
	const int skin1 = AddSkin( { j1, j2 }, { ibm1[0], ibm1[1] } );

	// Skeleton 2 with different joints under another root.
	const int root2 = AddRoot( new Node() );
	Scene::nodePool[root2]->localTransform = MatTranslate( ts_vec3( -1.0f, 0.0f, 0.5f ) );
	const int j3 = Scene::AddNode( new Node() );
	Scene::nodePool[j3]->localTransform = Mul( MatTranslate( ts_vec3( 0.2f, 0.2f, 0.0f ) ), MatRotY( 0.5f ) );
	Scene::AddChildNode( root2, j3 );
	const int j4 = Scene::AddNode( new Node() );
	Scene::nodePool[j4]->localTransform = MatScale( ts_vec3( 1.1f, 0.9f, 1.0f ) );
	Scene::AddChildNode( j3, j4 );
	ts_mat4 ibm2[2] = { MatTranslate( ts_vec3( -0.05f, 0.1f, 0.0f ) ), MatRotY( -0.4f ) };
	const int skin2 = AddSkin( { j3, j4 }, { ibm2[0], ibm2[1] } );

	// The source mesh's joint indices refer to skin-local joint slots; both skins have
	// two joints, so the same binding data is valid for either node.
	BindWeights( meshId, 2 );

	const int meshRoot = AddRoot( new Node() );
	Node* nodeA = new Node( meshId, MatTranslate( ts_vec3( 0.0f, 1.0f, 0.0f ) ) );
	nodeA->skinID = skin1;
	nodeA->weights = { 0.5f, 0.25f };
	const int nodeAId = Scene::AddNode( nodeA );
	Scene::AddChildNode( meshRoot, nodeAId );
	Node* nodeB = new Node( meshId, Mul( MatTranslate( ts_vec3( 2.0f, 0.0f, 0.0f ) ), MatScale( ts_vec3( 1.2f ) ) ) );
	nodeB->skinID = skin2;
	nodeB->weights = { -0.3f, 0.6f };
	const int nodeBId = Scene::AddNode( nodeB );
	Scene::AddChildNode( meshRoot, nodeBId );
	Node* nodeC = new Node( meshId, MatTranslate( ts_vec3( 4.0f, 0.0f, 0.0f ) ) );
	nodeC->weights = { 0.1f, 0.2f };
	const int nodeCId = Scene::AddNode( nodeC );
	Scene::AddChildNode( meshRoot, nodeCId );
	Node* nodeD = new Node( meshId, MatTranslate( ts_vec3( 6.0f, 0.0f, 0.0f ) ) );
	const int nodeDId = Scene::AddNode( nodeD );
	Scene::AddChildNode( meshRoot, nodeDId );

	Scene::UpdateSceneGraph( 0.0f );

	const RefMesh refMesh = CaptureMesh( meshId );
	::std::vector<ts_mat4> jmA = ReferenceJointMatrices( Scene::skins[skin1], Scene::nodePool[nodeAId]->combinedTransform );
	::std::vector<ts_mat4> jmB = ReferenceJointMatrices( Scene::skins[skin2], Scene::nodePool[nodeBId]->combinedTransform );
	const RefPose refA = ReferenceDeform( refMesh, nodeA->weights, &jmA );
	const RefPose refB = ReferenceDeform( refMesh, nodeB->weights, &jmB );
	const RefPose refC = ReferenceDeform( refMesh, nodeC->weights, nullptr );

	TASK_CHECK( NodeMatchesReference( nodeAId, refA, kGridTris ) );
	TASK_CHECK( NodeMatchesReference( nodeBId, refB, kGridTris ) );
	TASK_CHECK( NodeMatchesReference( nodeCId, refC, kGridTris ) );
	// Rigid node D reports the untouched source triangle.
	const FatTri* d0 = Scene::GetNodeTriangle( nodeDId, 0 );
	TASK_CHECK( d0 != nullptr );
	TASK_CHECK( NearV( d0->vertex0, refMesh.basePos[0] ) );
	// A and B must differ from each other and from the rigid source.
	TASK_CHECK( !NearV( Scene::GetNodeTriangle( nodeAId, 0 )->vertex0, Scene::GetNodeTriangle( nodeBId, 0 )->vertex0 ) );
	TASK_CHECK( !NearV( Scene::GetNodeTriangle( nodeAId, 0 )->vertex0, refMesh.basePos[0] ) );
	TASK_CHECK( !NearV( Scene::GetNodeTriangle( nodeBId, 0 )->vertex0, refMesh.basePos[0] ) );
	// Invalid arguments.
	TASK_CHECK( Scene::GetNodeTriangle( nodeAId, kGridTris ) == nullptr );
	TASK_CHECK( Scene::GetNodeTriangle( nodeAId, -1 ) == nullptr );
	TASK_CHECK( Scene::GetNodeTriangle( -1, 0 ) == nullptr );
	TASK_CHECK( Scene::GetNodeTriangle( (int)Scene::nodePool.size(), 0 ) == nullptr );
	// Shared material / UVs survive deformation.
	TASK_CHECK( Scene::GetNodeTriangle( nodeAId, 2 )->material == (uint32_t)mat );
	TASK_CHECK( Near( Scene::GetNodeTriangle( nodeAId, 2 )->u0, refMesh.poses.size() ? Scene::meshPool[meshId]->triangles[2].u0 : 0.0f ) );
	return 0;
}

// TLAS hit instance indices resolve back to stable node ids, including for deformed
// instances, and GetNodeTriangle agrees with the triangle actually intersected.
static int CaseInstanceIdentity()
{
	BeginCase();
	const int mat = Scene::AddMaterial( ts_vec3( 0.5f, 0.5f, 0.9f ), "id_mat" );
	const int meshA = BuildGridMesh( mat );
	const int meshB = BuildGridMesh( mat, 0.7f );
	InstallMorphTargets( meshB );

	const int j = AddRoot( new Node() );
	Scene::nodePool[j]->localTransform = MatTranslate( ts_vec3( 0.0f, 0.2f, 0.0f ) );
	const int skin = AddSkin( { j, j }, { ts_mat4(), MatRotY( 0.1f ) } );
	BindWeights( meshB, 2 );

	const int meshRoot = AddRoot( new Node() );
	Node* nodeA = new Node( meshA, MatTranslate( ts_vec3( 0.0f, 0.0f, 2.0f ) ) );
	const int nodeAId = Scene::AddNode( nodeA );
	Scene::AddChildNode( meshRoot, nodeAId );
	Node* nodeB = new Node( meshB, MatTranslate( ts_vec3( 2.0f, 0.0f, 2.0f ) ) );
	nodeB->skinID = skin;
	nodeB->weights = { 0.4f, 0.1f };
	const int nodeBId = Scene::AddNode( nodeB );
	Scene::AddChildNode( meshRoot, nodeBId );

	Scene::UpdateSceneGraph( 0.0f );

	TASK_CHECK( Scene::instPool.size() == Scene::instNodeId.size() );
	bool sawA = false, sawB = false;
	for (unsigned i = 0; i < (unsigned)Scene::instPool.size(); i++)
	{
		const int nodeId = Scene::NodeIdFromInstance( i );
		TASK_CHECK( nodeId >= 0 && nodeId < (int)Scene::nodePool.size() );
		TASK_CHECK( Scene::nodePool[nodeId] != nullptr );
		// The instance's BLAS index is either its source mesh slot or a per-node
		// deformation slot, never a raw mesh id used as a triangle source.
		TASK_CHECK( Scene::instPool[i].blasIdx >= 0 );
		if (nodeId == nodeAId) sawA = true;
		if (nodeId == nodeBId) sawB = true;
	}
	TASK_CHECK( sawA && sawB );
	// Node B's source mesh triangles are still the untouched source (B has its own copy).
	const Mesh* srcMesh = Scene::meshPool[meshB];
	for (int t = 0; t < kGridTris; t++)
	{
		TASK_CHECK( Near( srcMesh->triangles[t].vertex0.x, ts_vec3( srcMesh->vertices[t * 3 + 0] ).x ) );
	}
	TASK_CHECK( Scene::NodeIdFromInstance( (unsigned)Scene::instPool.size() ) == -1 );
	TASK_CHECK( Scene::NodeIdFromInstance( 0xffffu ) == -1 );

	// Shoot a ray from above at the *current* world position of a triangle of each
	// node (deformed geometry may have moved). The hit identity must resolve to that
	// node, and the triangle it reports must match the hit point.
	auto probeNode = [&]( const int nodeId ) -> bool {
		const FatTri* tri = Scene::GetNodeTriangle( nodeId, 0 );
		if (!tri) return false;
		int inst = -1;
		for (unsigned i = 0; i < (unsigned)Scene::instPool.size(); i++)
			if (Scene::NodeIdFromInstance( i ) == nodeId) inst = (int)i;
		if (inst < 0) return false;
		const ts_mat4& M = Scene::instPool[inst].transform;
		const ts_vec3 world0 = XformPoint( tri->vertex0, M );
		Ray ray( bvhvec3( world0.x, 5.0f, world0.z ), bvhvec3( 0, -1, 0 ) );
		Scene::tlas->Intersect( ray );
		if (ray.hit.t >= 1000.0f) return false;
		if (Scene::NodeIdFromInstance( (unsigned)ray.hit.inst ) != nodeId) return false;
		const FatTri* hitTri = Scene::GetNodeTriangle( nodeId, (int)ray.hit.prim );
		if (!hitTri) return false;
		// A ray from above must hit an upward-facing triangle of the reference grid.
		return Near( ts_vec3( hitTri->Nx, hitTri->Ny, hitTri->Nz ).y, 1.0f, 5e-2f );
	};
	TASK_CHECK( probeNode( nodeAId ) );
	TASK_CHECK( probeNode( nodeBId ) );
	return 0;
}

// Rigid instances keep sharing the source BLAS; a deformed sibling gets its own and
// leaves the shared source untouched.
static int CaseRigidSharedBLAS()
{
	BeginCase();
	const int mat = Scene::AddMaterial( ts_vec3( 0.6f, 0.9f, 0.6f ), "rigid_mat" );
	const int meshId = BuildGridMesh( mat );
	InstallMorphTargets( meshId );

	const int j = AddRoot( new Node() );
	Scene::nodePool[j]->localTransform = MatTranslate( ts_vec3( 0.0f, 0.3f, 0.0f ) );
	const int skin = AddSkin( { j, j }, { ts_mat4(), MatRotY( 0.2f ) } );
	BindWeights( meshId, 2 );

	const int meshRoot = AddRoot( new Node() );
	const int rigid1Id = Scene::AddNode( new Node( meshId, MatTranslate( ts_vec3( -3.0f, 0.0f, 0.0f ) ) ) );
	Scene::AddChildNode( meshRoot, rigid1Id );
	const int rigid2Id = Scene::AddNode( new Node( meshId, MatTranslate( ts_vec3( -6.0f, 0.0f, 0.0f ) ) ) );
	Scene::AddChildNode( meshRoot, rigid2Id );
	Node* deformer = new Node( meshId, MatTranslate( ts_vec3( 3.0f, 0.0f, 0.0f ) ) );
	deformer->skinID = skin;
	deformer->weights = { 0.3f, 0.0f };
	const int deformerId = Scene::AddNode( deformer );
	Scene::AddChildNode( meshRoot, deformerId );

	// Snapshot the source geometry before any deformation happens.
	::std::vector<ts_vec3> srcBefore;
	for (const FatTri& t : Scene::meshPool[meshId]->triangles)
	{
		srcBefore.push_back( t.vertex0 );
		srcBefore.push_back( t.vertex1 );
		srcBefore.push_back( t.vertex2 );
	}

	Scene::UpdateSceneGraph( 0.0f );

	tinybvh::BVHBase* sourceBLAS = Scene::meshPool[meshId]->ActiveBLAS();
	TASK_CHECK( sourceBLAS != nullptr );
	int rigidInstances = 0, deformInstance = -1;
	for (unsigned i = 0; i < (unsigned)Scene::instPool.size(); i++)
	{
		const int nodeId = Scene::NodeIdFromInstance( i );
		if (nodeId == rigid1Id || nodeId == rigid2Id)
		{
			TASK_CHECK( Scene::instPool[i].blasIdx == (unsigned)meshId );
			TASK_CHECK( Scene::blasList[Scene::instPool[i].blasIdx] == sourceBLAS );
			rigidInstances++;
		}
		if (nodeId == deformerId) deformInstance = (int)i;
	}
	TASK_CHECK( rigidInstances == 2 );
	TASK_CHECK( deformInstance >= 0 );
	const int deformSlot = Scene::instPool[deformInstance].blasIdx;
	TASK_CHECK( deformSlot != meshId );
	TASK_CHECK( Scene::blasList[deformSlot] != sourceBLAS );
	TASK_CHECK( Scene::blasList[deformSlot] != nullptr );
	// The source mesh's own geometry and BLAS are unchanged by deformation.
	const Mesh* src = Scene::meshPool[meshId];
	for (int t = 0; t < (int)src->triangles.size(); t++)
	{
		TASK_CHECK( NearV( src->triangles[t].vertex0, srcBefore[t * 3 + 0] ) );
		TASK_CHECK( NearV( src->triangles[t].vertex1, srcBefore[t * 3 + 1] ) );
		TASK_CHECK( NearV( src->triangles[t].vertex2, srcBefore[t * 3 + 2] ) );
	}
	// Rendered positions of the rigid and deformed nodes differ (no cross-overwrite).
	Ray rigidRay( bvhvec3( -3.0f, 9.0f, 0.0f ), bvhvec3( 0, -1, 0 ) );
	Scene::tlas->Intersect( rigidRay );
	TASK_CHECK( rigidRay.hit.t < 1000.0f );
	TASK_CHECK( Scene::NodeIdFromInstance( (unsigned)rigidRay.hit.inst ) == rigid1Id );
	return 0;
}

// ===========================================================================
// Group: render
// ===========================================================================

// A morph slides a triangle along X. Primary and shadow rays must follow the current
// pose: the rest-position hit must disappear after the move, the new position must be
// occluded, and no stale geometry may remain in the BLAS.
static int CaseOcclusionFollowsPose()
{
	BeginCase();
	const int mat = Scene::AddMaterial( ts_vec3( 1.0f ), "render_mat" );
	const int meshId = Scene::AddMesh( 0 );
	Scene::AddTriToMesh( meshId, ts_vec3( -1, -1, 0 ), ts_vec3( 1, -1, 0 ), ts_vec3( 0, 1, 0 ), mat );
	Mesh* m = Scene::meshPool[meshId];
	m->triangles[0].vN0 = m->triangles[0].vN1 = m->triangles[0].vN2 = ts_vec3( 0, 0, -1 );
	m->triangles[0].Nx = 0, m->triangles[0].Ny = 0, m->triangles[0].Nz = -1;
	m->BuildMaterialList();
	Mesh::Pose base, target;
	for (int i = 0; i < (int)m->vertices.size(); i++)
	{
		base.positions.push_back( ts_vec3( m->vertices[i] ) );
		base.normals.push_back( ts_vec3( 0, 0, -1 ) );
		target.positions.push_back( ts_vec3( 8.0f, 0, 0 ) );
		target.normals.push_back( ts_vec3( 0, 0, 0 ) );
	}
	m->poses = { base, target };

	Node* node = new Node( meshId, ts_mat4() );
	node->weights = { 0.0f };
	const int nodeId = AddRoot( node );

	auto primaryHit = [&]( const float x ) -> bool {
		Ray r( bvhvec3( x, 0.0f, 5.0f ), bvhvec3( 0, 0, -1 ) );
		Scene::tlas->Intersect( r );
		return r.hit.t < 1000.0f;
	};
	auto shadowBlocked = [&]( const float x ) -> bool {
		Ray r( bvhvec3( x, 0.0f, 5.0f ), bvhvec3( 0, 0, -1 ), 10.0f );
		return Scene::tlas->IsOccluded( r );
	};

	Scene::UpdateSceneGraph( 0.0f );
	TASK_CHECK( primaryHit( 0.0f ) );
	TASK_CHECK( !primaryHit( 6.0f ) );
	TASK_CHECK( shadowBlocked( 0.0f ) );
	TASK_CHECK( !shadowBlocked( 6.0f ) );

	node->weights = { 1.0f };
	Scene::UpdateSceneGraph( 0.0f );
	TASK_CHECK( !primaryHit( 0.0f ) );
	TASK_CHECK( primaryHit( 8.0f ) );
	TASK_CHECK( !shadowBlocked( 0.0f ) );
	TASK_CHECK( shadowBlocked( 8.0f ) );

	// No stale hit at the old position.
	Ray stale( bvhvec3( 0.0f, 0.0f, 5.0f ), bvhvec3( 0, 0, -1 ) );
	Scene::tlas->Intersect( stale );
	TASK_CHECK( stale.hit.t > 1000.0f );
	// New hit resolves to the node's triangle, at the moved position.
	Ray fresh( bvhvec3( 8.0f, 0.0f, 5.0f ), bvhvec3( 0, 0, -1 ) );
	Scene::tlas->Intersect( fresh );
	TASK_CHECK( fresh.hit.t < 1000.0f );
	TASK_CHECK( Scene::NodeIdFromInstance( (unsigned)fresh.hit.inst ) == nodeId );
	const FatTri* tri = Scene::GetNodeTriangle( nodeId, (int)fresh.hit.prim );
	TASK_CHECK( tri != nullptr );
	TASK_CHECK( Near( tri->vertex0.x, 7.0f ) && Near( tri->vertex1.x, 9.0f ) );
	TASK_CHECK( Near( tri->vertex2.x, 8.0f ) );
	return 0;
}

// Render a 48 x 36 P3 PPM with real primary rays through the TLAS and compare it
// pixel by pixel with a reference computed from independently deformed geometry.
// A rigid sibling provides the control: it stays at its source position.
static int CaseRenderPPM()
{
	BeginCase();
	const int mat = Scene::AddMaterial( ts_vec3( 0.85f, 0.45f, 0.3f ), "ppm_mat" );
	const int meshId = BuildGridMesh( mat );
	InstallMorphTargets( meshId );

	// Deformed mesh node, translated to z = 4.
	Node* meshNode = new Node( meshId, MatTranslate( ts_vec3( 0.0f, 0.0f, 4.0f ) ) );
	meshNode->weights = { 0.2f, 0.5f };
	const int meshNodeId = AddRoot( meshNode );
	// Rigid control at z = 4 but x = -12, far enough not to overlap the projected mesh.
	Node* rigidNode = new Node( meshId, MatTranslate( ts_vec3( -12.0f, 0.0f, 4.0f ) ) );
	const int rigidNodeId = AddRoot( rigidNode );
	Scene::UpdateSceneGraph( 0.0f );

	const RefMesh refMesh = CaptureMesh( meshId );
	const RefPose ref = ReferenceDeform( refMesh, meshNode->weights, nullptr );
	const ts_mat4 world = Scene::nodePool[meshNodeId]->combinedTransform;

	const int W = 48, H = 36;
	const float halfH = 1.6f, aspect = (float)W / (float)H;
	::std::vector<unsigned char> image( W * H * 3 ), reference( W * H * 3 );
	const ts_vec3 L = Norm( ts_vec3( 0.4f, 0.8f, 0.6f ) );
	const ts_vec3 albedo = Scene::materials[mat]->color.value;

	auto shade = [&]( const ts_vec3& iN, ts_vec3& c ) {
		const float diff = FMax( 0.25f, Dot( iN, L ) );
		c = albedo * diff;
	};

	int differing = 0, hits = 0;
	for (int py = 0; py < H; py++) for (int px = 0; px < W; px++)
	{
		const float u = (2.0f * (px + 0.5f) / W - 1.0f) * halfH * aspect;
		const float v = (1.0f - 2.0f * (py + 0.5f) / H) * halfH;
		// Camera looks straight down at the grid (which lives around z = 4).
		Ray ray( bvhvec3( u, 9.0f, 4.0f + v ), bvhvec3( 0, -1, 0 ) );
		Scene::tlas->Intersect( ray );
		unsigned char rgb[3] = { 20, 24, 32 };
		if (ray.hit.t < 1000.0f)
		{
			const int nodeId = Scene::NodeIdFromInstance( (unsigned)ray.hit.inst );
			TASK_CHECK( nodeId == meshNodeId || nodeId == rigidNodeId );
			const FatTri* tri = Scene::GetNodeTriangle( nodeId, (int)ray.hit.prim );
			TASK_CHECK( tri != nullptr );
			TASK_CHECK( tri->Ny > 0.9f ); // upward-facing hit
			const float bu = ray.hit.u, bv = ray.hit.v, bw = 1.0f - bu - bv;
			ts_vec3 iN = bu * tri->vN1 + bv * tri->vN2 + bw * tri->vN0;
			iN = Norm( XformVector( iN, Scene::instPool[ray.hit.inst].transform ) );
			ts_vec3 c;
			shade( iN, c );
			rgb[0] = (unsigned char)FMin( 255.0f, c.x * 255.0f );
			rgb[1] = (unsigned char)FMin( 255.0f, c.y * 255.0f );
			rgb[2] = (unsigned char)FMin( 255.0f, c.z * 255.0f );
			hits++;
		}
		const int idx = (py * W + px) * 3;
		image[idx + 0] = rgb[0], image[idx + 1] = rgb[1], image[idx + 2] = rgb[2];

		// Independent reference: Möller–Trumbore against the deformed triangles.
		bool hit = false;
		float bestT = 1e30f, bestU = 0, bestV = 0;
		int bestTri = -1;
		const ts_vec3 O( u, 9.0f, 4.0f + v ), D( 0, -1, 0 );
		for (int t = 0; t < kGridTris; t++)
		{
			float tt, uu, vv;
			if (!RayTri( O, D, XformPoint( ref.pos[t * 3 + 0], world ),
				XformPoint( ref.pos[t * 3 + 1], world ), XformPoint( ref.pos[t * 3 + 2], world ),
				bestT, tt, uu, vv )) continue;
			hit = true, bestT = tt, bestU = uu, bestV = vv, bestTri = t;
		}
		unsigned char rr[3] = { 20, 24, 32 };
		if (hit)
		{
			ts_vec3 iN = bestU * ref.nrm[bestTri * 3 + 1] + bestV * ref.nrm[bestTri * 3 + 2]
				+ (1.0f - bestU - bestV) * ref.nrm[bestTri * 3 + 0];
			iN = Norm( XformVector( iN, world ) );
			ts_vec3 c;
			shade( iN, c );
			rr[0] = (unsigned char)FMin( 255.0f, c.x * 255.0f );
			rr[1] = (unsigned char)FMin( 255.0f, c.y * 255.0f );
			rr[2] = (unsigned char)FMin( 255.0f, c.z * 255.0f );
		}
		reference[idx + 0] = rr[0], reference[idx + 1] = rr[1], reference[idx + 2] = rr[2];
		// Per-channel tolerance of 2 handles float rounding; silhouettes must still match.
		bool equal = true;
		for (int k = 0; k < 3; k++) if (abs( (int)image[idx + k] - (int)reference[idx + k] ) > 2) equal = false;
		if (!equal) differing++;
	}
	// The frame must contain both covered and background pixels; otherwise the
	// comparison would be vacuous.
	TASK_CHECK( hits > 0 && hits < W * H );
	TASK_CHECK( differing == 0 );

	// Finite shadow rays along the same directions must agree with the primary hits:
	// blocked where the deformed mesh is hit, clear where the background is visible.
	for (int py = 0; py < H; py += 6) for (int px = 0; px < W; px += 6)
	{
		const float u = (2.0f * (px + 0.5f) / W - 1.0f) * halfH * aspect;
		const float v = (1.0f - 2.0f * (py + 0.5f) / H) * halfH;
		const int idx = (py * W + px) * 3;
		const bool covered = !(image[idx + 0] == 20 && image[idx + 1] == 24 && image[idx + 2] == 32);
		Ray shadow( bvhvec3( u, 9.0f, 4.0f + v ), bvhvec3( 0, -1, 0 ), 20.0f );
		TASK_CHECK( Scene::tlas->IsOccluded( shadow ) == covered );
	}
	// A finite shadow ray that stops before the mesh must not report occlusion.
	Ray shortRay( bvhvec3( 0.0f, 9.0f, 4.0f ), bvhvec3( 0, -1, 0 ), 1.0f );
	TASK_CHECK( !Scene::tlas->IsOccluded( shortRay ) );

	// The rigid control still hits at its untouched source position.
	Ray control( bvhvec3( -12.0f, 9.0f, 4.0f ), bvhvec3( 0, -1, 0 ) );
	Scene::tlas->Intersect( control );
	TASK_CHECK( control.hit.t < 1000.0f );
	TASK_CHECK( Scene::NodeIdFromInstance( (unsigned)control.hit.inst ) == rigidNodeId );
	{
		const FatTri* tri = Scene::GetNodeTriangle( rigidNodeId, (int)control.hit.prim );
		TASK_CHECK( tri != nullptr );
		TASK_CHECK( NearV( tri->vertex0, refMesh.basePos[(int)control.hit.prim * 3 + 0] ) );
	}

	// Write a compact P3 PPM with the production image.
	FILE* f = fopen( "tiny_scene_deform_render.ppm", "wb" );
	TASK_CHECK( f != nullptr );
	TASK_CHECK( fprintf( f, "P3\n%d %d\n255\n", W, H ) > 0 );
	for (int y = 0; y < H; y++)
	{
		for (int x = 0; x < W; x++)
		{
			const int idx = (y * W + x) * 3;
			fprintf( f, "%d %d %d ", (int)image[idx + 0], (int)image[idx + 1], (int)image[idx + 2] );
		}
		fprintf( f, "\n" );
	}
	fclose( f );
	return 0;
}

// ---------------------------------------------------------------------------
// Case registration
// ---------------------------------------------------------------------------
extern const task_case task_cases[] =
{
	{ "stages", "morph_then_skin", CaseMorphThenSkin },
	{ "stages", "cross_root_order", CaseCrossRootJointOrder },
	{ "stages", "negative_morph", CaseNegativeMorph },
	{ "stages", "three_frames_rest", CaseThreeFramesThenRest },
	{ "instances", "shared_mesh_skins", CaseSharedMeshDistinctSkins },
	{ "instances", "instance_identity", CaseInstanceIdentity },
	{ "instances", "rigid_shared_blas", CaseRigidSharedBLAS },
	{ "render", "occlusion_follows_pose", CaseOcclusionFollowsPose },
	{ "render", "render_ppm", CaseRenderPPM },
};
extern const size_t task_case_count = sizeof( task_cases ) / sizeof( task_cases[0] );
