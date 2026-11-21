// Example: queued rectangle occlusion queries with CullingThreadpool.
//
// The scene traversal use case: occluders are still being rasterized in the
// background, but we want to submit a batch of rectangle queries now and pick up
// the results later. Each batch sees exactly the drawcalls submitted before it:
//
//   draw A  ->  enqueue batch 1  ->  draw B  ->  enqueue batch 2
//
// Batch 1 is tested against a buffer containing only A, batch 2 against a buffer
// containing A+B, and both are cross-checked against a serial reference.
#include "MaskedOcclusionCulling.h"
#include "CullingThreadpool.h"

#include <cstdio>
#include <vector>

int main()
{
	using M = MaskedOcclusionCulling;

	M *moc = M::Create(M::AVX512);		// auto-detect best supported implementation
	M *reference = M::Create(M::AVX512);
	moc->SetResolution(256, 128);
	reference->SetResolution(256, 128);
	moc->SetNearClipPlane(0.1f);
	reference->SetNearClipPlane(0.1f);
	moc->ClearBuffer();
	reference->ClearBuffer();

	// Two occluder quads, specified as NDC rectangles scaled by their clip space w
	// (clip = NDC * w): A covers NDC [-0.6,0.6]^2 at w=2, B covers NDC [0.2,1.0]^2,
	// nearer, at w=1.25
	float quadA[] = { -1.2f, -1.2f, 0, 2.0f,  1.2f, -1.2f, 0, 2.0f,  1.2f, 1.2f, 0, 2.0f,  -1.2f, 1.2f, 0, 2.0f };
	float quadB[] = {  0.25f, 0.25f, 0, 1.25f, 1.25f, 0.25f, 0, 1.25f, 1.25f, 1.25f, 0, 1.25f,  0.25f, 1.25f, 0, 1.25f };
	unsigned int indices[] = { 0, 1, 2, 0, 2, 3 };

	// Two rectangle queries (NDC), both behind the occluders (w=3)
	CullingThreadpool::RectQuery queries[2] =
	{
		{ -0.5f, -0.5f, -0.1f, -0.1f, 3.0f },	// fully inside A
		{  0.3f,  0.3f,  0.8f,  0.8f, 3.0f },	// partly inside A, fully inside B
	};

	CullingThreadpool pool(2, 2, 2, 8);
	pool.SetBuffer(moc);
	pool.WakeThreads();

	// Draw A, enqueue the first batch, draw B, enqueue the second batch
	pool.RenderTriangles(quadA, indices, 2, M::BACKFACE_NONE);
	CullingThreadpool::QueryTicket batch1 = pool.EnqueueRectQueries(queries, 2);
	pool.RenderTriangles(quadB, indices, 2, M::BACKFACE_NONE);
	CullingThreadpool::QueryTicket batch2 = pool.EnqueueRectQueries(queries, 2);

	// Serial reference: evaluate both prefixes on a separate buffer
	reference->RenderTriangles(quadA, indices, 2, nullptr, M::BACKFACE_NONE);
	M::CullingResult ref1[2] = { reference->TestRect(queries[0].mXmin, queries[0].mYmin, queries[0].mXmax, queries[0].mYmax, queries[0].mWmin),
								 reference->TestRect(queries[1].mXmin, queries[1].mYmin, queries[1].mXmax, queries[1].mYmax, queries[1].mWmin) };
	reference->RenderTriangles(quadB, indices, 2, nullptr, M::BACKFACE_NONE);
	M::CullingResult ref2[2] = { reference->TestRect(queries[0].mXmin, queries[0].mYmin, queries[0].mXmax, queries[0].mYmax, queries[0].mWmin),
								 reference->TestRect(queries[1].mXmin, queries[1].mYmin, queries[1].mXmax, queries[1].mYmax, queries[1].mWmin) };

	// Pick up the results later; Wait() only waits for its own batch
	batch1.Wait();
	batch2.Wait();

	static const char *names[] = { "VISIBLE", "OCCLUDED", "?", "VIEW_CULLED" };
	bool ok = true;
	for (int i = 0; i < 2; ++i)
	{
		M::CullingResult r1 = batch1.Results()[i];
		M::CullingResult r2 = batch2.Results()[i];
		std::printf("query %d: after A = %s, after A+B = %s\n", i, names[r1 & 3], names[r2 & 3]);
		ok &= r1 == ref1[i] && r2 == ref2[i];
	}

	// Expected contents: query 0 stays occluded (inside A), query 1 is visible
	// after A (it sticks out of A) but occluded once B covers it
	ok &= batch1.Results()[0] == M::OCCLUDED && batch1.Results()[1] == M::VISIBLE;
	ok &= batch2.Results()[0] == M::OCCLUDED && batch2.Results()[1] == M::OCCLUDED;

	pool.SuspendThreads();
	M::Destroy(moc);
	M::Destroy(reference);

	if (!ok)
	{
		std::puts("QUEUED_QUERIES_FAILED");
		return 1;
	}
	std::puts("QUEUED_QUERIES_OK batch1=occluded,visible batch2=occluded,occluded");
	return 0;
}
