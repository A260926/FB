// Conformance checks for the queued rectangle query API of CullingThreadpool.
//
// The cases are registered with the task driver in three groups:
//   - "prefix":    query results must equal a serial TestRect() reference evaluated
//                  on the buffer containing exactly the drawcalls submitted before
//                  the query batch.
//   - "lifetime":  tickets own their results; results stay valid across ring slot
//                  reuse, ClearBuffer/SetBuffer/SetResolution and pool destruction.
//   - "partition": per-bin partitioned execution, cross-bin rectangles, tail bins,
//                  and proof (via timeout-guarded synchronization handshakes, not
//                  timing guesses) that bins make independent progress.
#include "task_support/check.h"

#include "MaskedOcclusionCulling.h"
#include "CullingThreadpool.h"

#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

using M = MaskedOcclusionCulling;
using CR = M::CullingResult;
using Pool = CullingThreadpool;
using Ticket = CullingThreadpool::QueryTicket;
using RQ = CullingThreadpool::RectQuery;

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Test helpers
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// RAII wrapper for the occlusion culling objects
struct MocPtr
{
	M *p;
	MocPtr() { p = M::Create(M::AVX512); }	// auto-detect best supported implementation
	~MocPtr() { M::Destroy(p); }
	operator M *() const { return p; }
	M *operator->() const { return p; }
};

static RQ rq(float xmin, float ymin, float xmax, float ymax, float wmin)
{
	RQ q;
	q.mXmin = xmin; q.mYmin = ymin; q.mXmax = xmax; q.mYmax = ymax; q.mWmin = wmin;
	return q;
}

static CR refTest(M *moc, const RQ &q)
{
	return moc->TestRect(q.mXmin, q.mYmin, q.mXmax, q.mYmax, q.mWmin);
}

// Converts pixel coordinates to NDC for a given resolution (D3D convention: y down)
struct NdcMapper
{
	float hw, hh;
	NdcMapper(float w, float h) : hw(w * 0.5f), hh(h * 0.5f) {}
	float x(float px) const { return px / hw - 1.0f; }
	float y(float py) const { return 1.0f - py / hh; }
	RQ rect(float px0, float py0, float px1, float py1, float w) const
	{
		return rq(x(px0), y(py1), x(px1), y(py0), w);
	}
};

// Front-parallel quads (2 triangles each) with persistent storage. The pool renders
// asynchronously, so the vertex/index buffers must outlive the rendering work.
// Quads are specified in NDC coordinates plus a clip space w (clip = NDC * w).
struct QuadSet
{
	std::vector<float> verts;		// 16 floats per quad
	std::vector<unsigned int> idx;	// 6 indices per quad

	QuadSet() { verts.reserve(16 * 256); idx.reserve(6 * 256); }

	int add(float x0, float y0, float x1, float y1, float w)
	{
		int id = (int)(verts.size() / 16);
		float v[16] = { x0 * w, y0 * w, 0.0f, w, x1 * w, y0 * w, 0.0f, w, x1 * w, y1 * w, 0.0f, w, x0 * w, y1 * w, 0.0f, w };
		for (int k = 0; k < 16; ++k) verts.push_back(v[k]);
		unsigned int ii[6] = { 0, 1, 2, 0, 2, 3 };
		for (int k = 0; k < 6; ++k) idx.push_back(ii[k]);
		return id;
	}
	const float *v(int id) const { return &verts[id * 16]; }
	const unsigned int *i(int id) const { return &idx[id * 6]; }
	void render(M *moc, int id) const { moc->RenderTriangles(v(id), i(id), 2, nullptr, M::BACKFACE_NONE); }
	void render(Pool &pool, int id) const { pool.RenderTriangles(v(id), i(id), 2, M::BACKFACE_NONE); }
};

// Timeout-guarded synchronization handshake: polls pred() until it holds or the
// timeout expires. Used to prove parallel progress without relying on wall-clock
// timing of the work itself.
template <typename F> static bool waitFor(F &&pred, double timeoutSec = 10.0)
{
	std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
	while (!pred())
	{
		if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > timeoutSec)
			return false;
		std::this_thread::yield();
	}
	return true;
}

// Resumes a paused bin if the test case bails out early (keeps the pool destructor
// from blocking on a parked bin)
struct BinGuard
{
	Pool &pool;
	unsigned int bin;
	BinGuard(Pool &p, unsigned int b) : pool(p), bin(b) {}
	~BinGuard() { pool.DebugResumeBin(bin); }
};

static std::vector<CR> refTestAll(M *moc, const std::vector<RQ> &queries)
{
	std::vector<CR> out;
	for (size_t k = 0; k < queries.size(); ++k)
		out.push_back(refTest(moc, queries[k]));
	return out;
}

static bool resultsEqual(const Ticket &t, const std::vector<CR> &expected)
{
	if (t.Results().size() != expected.size())
		return false;
	for (size_t k = 0; k < expected.size(); ++k)
		if (t.Results()[k] != expected[k])
			return false;
	return true;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// prefix group
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// "Draw A, query Q, draw B, query Q again": the two batches must see exactly their
// respective draw prefixes, matching the serial reference.
static int prefix_draw_query_draw_query()
{
	MocPtr moc, ref;
	moc->SetResolution(128, 64); ref->SetResolution(128, 64);
	moc->SetNearClipPlane(0.1f); ref->SetNearClipPlane(0.1f);
	moc->ClearBuffer(); ref->ClearBuffer();

	QuadSet qs;
	int A = qs.add(-0.5f, -0.5f, 0.5f, 0.5f, 2.0f);	// center occluder at w=2
	int B = qs.add(-1.0f, -1.0f, 1.0f, 1.0f, 1.5f);	// nearer fullscreen occluder at w=1.5

	std::vector<RQ> batch;
	batch.push_back(rq(-0.25f, -0.25f, 0.25f, 0.25f, 3.0f));	// behind A
	batch.push_back(rq(-0.25f, -0.25f, 0.25f, 0.25f, 1.0f));	// in front of A and B

	Ticket t1, t2;
	{
		Pool pool(2, 2, 2, 4);
		pool.SetBuffer(moc);
		pool.WakeThreads();

		qs.render(pool, A);
		t1 = pool.EnqueueRectQueries(batch.data(), batch.size());
		qs.render(pool, B);
		t2 = pool.EnqueueRectQueries(batch.data(), batch.size());

		// Serial reference: same prefix sequence on a separate buffer
		qs.render(ref, A);
		std::vector<CR> e1 = refTestAll(ref, batch);
		qs.render(ref, B);
		std::vector<CR> e2 = refTestAll(ref, batch);

		t1.Wait();
		t2.Wait();
		TASK_CHECK(t1.IsReady() && t2.IsReady());
		TASK_CHECK(resultsEqual(t1, e1));
		TASK_CHECK(resultsEqual(t2, e2));

		// Content sanity: the first batch must not see B, the second must
		TASK_CHECK(t1.Results()[0] == M::OCCLUDED);	// w=3 behind A(w=2)
		TASK_CHECK(t1.Results()[1] == M::VISIBLE);	// w=1 in front of A
		TASK_CHECK(t2.Results()[0] == M::OCCLUDED);	// w=3 behind B(w=1.5)
		TASK_CHECK(t2.Results()[1] == M::VISIBLE);	// w=1 in front of B

		// An empty batch completes immediately and does not touch the ring
		Ticket empty = pool.EnqueueRectQueries(nullptr, 0);
		TASK_CHECK(empty.IsReady());
		TASK_CHECK(empty.Results().empty());

		// The immediate TestRect() path is unchanged and matches the reference
		pool.Flush();
		TASK_CHECK(pool.TestRect(-0.25f, -0.25f, 0.25f, 0.25f, 3.0f) == refTest(ref, batch[0]));
		TASK_CHECK(pool.TestRect(-0.25f, -0.25f, 0.25f, 0.25f, 1.0f) == refTest(ref, batch[1]));

		pool.SuspendThreads();
	}
	return 0;
}

// Ring capacity (4 slots) is much smaller than the total number of submitted jobs
// (8 drawcalls + 8 query batches): the submitter only blocks on recyclable slots,
// and every batch still sees exactly its own prefix. Also covers equal-depth
// (GREATER_EQUAL) visibility.
static int prefix_ring_overflow()
{
	MocPtr moc, ref;
	moc->SetResolution(128, 64); ref->SetResolution(128, 64);
	moc->SetNearClipPlane(0.1f); ref->SetNearClipPlane(0.1f);
	moc->ClearBuffer(); ref->ClearBuffer();

	const int N = 8;	// 8 strips of 16 pixels each (subtile aligned for all SIMD widths)
	QuadSet qs;
	std::vector<int> strips;
	for (int k = 0; k < N; ++k)
	{
		float x0 = -1.0f + 0.25f * (float)k;
		strips.push_back(qs.add(x0, -1.0f, x0 + 0.25f, 1.0f, 4.0f + (float)k));
	}

	std::vector<Ticket> tickets;
	std::vector<std::vector<RQ>> batches;
	{
		Pool pool(2, 2, 2, 4);
		pool.SetBuffer(moc);
		pool.WakeThreads();

		for (int k = 0; k < N; ++k)
		{
			qs.render(pool, strips[k]);
			std::vector<RQ> batch;
			for (int j = 0; j <= k; ++j)
			{
				float xc = -1.0f + 0.25f * (float)j + 0.125f;
				batch.push_back(rq(xc - 0.0625f, -0.5f, xc + 0.0625f, 0.5f, 4.0f + (float)j));	// equal depth as strip j
				batch.push_back(rq(xc - 0.0625f, -0.5f, xc + 0.0625f, 0.5f, 100.0f));			// far behind strip j
			}
			batches.push_back(batch);
			tickets.push_back(pool.EnqueueRectQueries(batches.back().data(), batches.back().size()));
		}

		// Serial reference
		std::vector<std::vector<CR>> expected;
		for (int k = 0; k < N; ++k)
		{
			qs.render(ref, strips[k]);
			expected.push_back(refTestAll(ref, batches[k]));
		}

		for (int k = 0; k < N; ++k)
		{
			tickets[k].Wait();
			TASK_CHECK(resultsEqual(tickets[k], expected[k]));
		}
		// Content sanity: equal depth counts as visible, far depth is occluded
		for (int k = 0; k < N; ++k)
			for (size_t i = 0; i < batches[k].size(); ++i)
				TASK_CHECK(tickets[k].Results()[i] == (i % 2 == 0 ? M::VISIBLE : M::OCCLUDED));

		pool.SuspendThreads();
	}
	return 0;
}

// ClearBuffer(), SetBuffer() and SetResolution() execute at the usual Flush
// boundaries: batches queued before them complete against the old state first.
static int prefix_clear_setbuffer_resize()
{
	MocPtr m1, m2, r1, r2;
	m1->SetResolution(128, 64); r1->SetResolution(128, 64);
	m2->SetResolution(128, 64); r2->SetResolution(128, 64);
	m1->SetNearClipPlane(0.1f); r1->SetNearClipPlane(0.1f);
	m2->SetNearClipPlane(0.1f); r2->SetNearClipPlane(0.1f);
	m1->ClearBuffer(); r1->ClearBuffer();
	m2->ClearBuffer(); r2->ClearBuffer();

	QuadSet qs;
	int A = qs.add(-0.5f, -0.5f, 0.5f, 0.5f, 2.0f);	// center occluder
	int B = qs.add(0.0f, 0.0f, 1.0f, 1.0f, 1.5f);		// top-right nearer occluder
	int C = qs.add(-1.0f, -1.0f, -0.2f, -0.2f, 1.2f);	// bottom-left occluder

	RQ qCenter = rq(-0.25f, -0.25f, 0.25f, 0.25f, 3.0f);
	RQ qCorner = rq(0.5f, 0.5f, 0.9f, 0.9f, 3.0f);
	RQ qBL = rq(-0.9f, -0.9f, -0.3f, -0.3f, 3.0f);

	Ticket t1, t2, t3, t4;
	std::vector<CR> e1, e2, e3, e4;
	{
		Pool pool(2, 2, 2, 8);
		pool.SetBuffer(m1);
		pool.WakeThreads();

		// Batch 1: on m1 after drawing A
		qs.render(pool, A);
		std::vector<RQ> b1; b1.push_back(qCenter); b1.push_back(qCorner);
		t1 = pool.EnqueueRectQueries(b1.data(), b1.size());
		qs.render(r1, A);
		e1 = refTestAll(r1, b1);

		// ClearBuffer at a Flush boundary: batch 1 completes against the old contents
		pool.ClearBuffer();
		r1->ClearBuffer();

		// Batch 2: on the cleared m1 after drawing B
		qs.render(pool, B);
		std::vector<RQ> b2; b2.push_back(qCenter); b2.push_back(qCorner);
		t2 = pool.EnqueueRectQueries(b2.data(), b2.size());
		qs.render(r1, B);
		e2 = refTestAll(r1, b2);

		// SetBuffer at a Flush boundary: batch 3 runs against m2
		pool.SetBuffer(m2);
		qs.render(pool, C);
		std::vector<RQ> b3; b3.push_back(qBL); b3.push_back(qCenter);
		t3 = pool.EnqueueRectQueries(b3.data(), b3.size());
		qs.render(r2, C);
		e3 = refTestAll(r2, b3);

		// SetResolution at a Flush boundary: batch 4 runs against the resized m2
		pool.SetResolution(64, 64);
		pool.ClearBuffer();
		r2->SetResolution(64, 64);
		r2->ClearBuffer();
		qs.render(pool, A);
		std::vector<RQ> b4; b4.push_back(qCenter);
		t4 = pool.EnqueueRectQueries(b4.data(), b4.size());
		qs.render(r2, A);
		e4 = refTestAll(r2, b4);

		pool.SuspendThreads();
	}

	t1.Wait(); t2.Wait(); t3.Wait(); t4.Wait();
	TASK_CHECK(resultsEqual(t1, e1));
	TASK_CHECK(resultsEqual(t2, e2));
	TASK_CHECK(resultsEqual(t3, e3));
	TASK_CHECK(resultsEqual(t4, e4));

	// Content sanity
	TASK_CHECK(t1.Results()[0] == M::OCCLUDED);	// behind A
	TASK_CHECK(t1.Results()[1] == M::VISIBLE);	// corner not covered by A
	TASK_CHECK(t2.Results()[0] == M::VISIBLE);	// cleared: nothing covers the center
	TASK_CHECK(t2.Results()[1] == M::OCCLUDED);	// behind B
	TASK_CHECK(t3.Results()[0] == M::OCCLUDED);	// behind C on m2
	TASK_CHECK(t3.Results()[1] == M::VISIBLE);	// center free on m2
	TASK_CHECK(t4.Results()[0] == M::OCCLUDED);	// behind A on the resized m2
	return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// lifetime group
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Completed results stay valid across ring slot reuse, ClearBuffer(), SetBuffer(),
// SetResolution() and destruction of the pool. Tickets are copyable and share state.
static int lifetime_results_survive_ops_and_pool()
{
	MocPtr moc, m2, ref;
	moc->SetResolution(128, 64); ref->SetResolution(128, 64);
	m2->SetResolution(128, 64);
	moc->SetNearClipPlane(0.1f); ref->SetNearClipPlane(0.1f);
	moc->ClearBuffer(); ref->ClearBuffer();
	m2->ClearBuffer();

	QuadSet qs;
	int A = qs.add(-0.5f, -0.5f, 0.5f, 0.5f, 2.0f);
	int B = qs.add(-1.0f, -1.0f, 1.0f, 1.0f, 1.5f);

	std::vector<RQ> b1, b2;
	b1.push_back(rq(-0.25f, -0.25f, 0.25f, 0.25f, 3.0f));
	b1.push_back(rq(-0.25f, -0.25f, 0.25f, 0.25f, 1.0f));
	b2.push_back(rq(-0.9f, -0.9f, 0.9f, 0.9f, 3.0f));
	b2.push_back(rq(0.6f, 0.6f, 0.9f, 0.9f, 1.2f));

	Ticket t1, t1copy, t2;
	std::vector<CR> e1, e2;
	{
		Pool pool(2, 2, 2, 4);
		pool.SetBuffer(moc);
		pool.WakeThreads();

		qs.render(pool, A);
		t1 = pool.EnqueueRectQueries(b1.data(), b1.size());
		t1copy = t1;	// tickets can be copied; copies share the results
		qs.render(pool, B);
		t2 = pool.EnqueueRectQueries(b2.data(), b2.size());

		qs.render(ref, A);
		e1 = refTestAll(ref, b1);
		qs.render(ref, B);
		e2 = refTestAll(ref, b2);

		// Cycle the ring well beyond its capacity (4 slots) to recycle the slots
		// that held t1/t2, then cross all the Flush-boundary state changes.
		for (int k = 0; k < 8; ++k)
		{
			Ticket tk = pool.EnqueueRectQueries(b1.data(), b1.size());
			tk.Wait();
		}
		pool.ClearBuffer();
		pool.SetResolution(64, 64);
		pool.SetBuffer(m2);
		pool.SuspendThreads();
	}	// pool is destroyed here

	// The results must still be valid and correct
	TASK_CHECK(t1.IsReady() && t1copy.IsReady() && t2.IsReady());
	t1.Wait();
	t2.Wait();	// return immediately
	TASK_CHECK(resultsEqual(t1, e1));
	TASK_CHECK(resultsEqual(t2, e2));
	TASK_CHECK(resultsEqual(t1copy, e1));
	TASK_CHECK(&t1.Results() == &t1copy.Results());	// copies share the same storage
	TASK_CHECK(t1.Results()[0] == M::OCCLUDED);
	TASK_CHECK(t1.Results()[1] == M::VISIBLE);
	TASK_CHECK(t2.Results()[0] == M::OCCLUDED);
	TASK_CHECK(t2.Results()[1] == M::VISIBLE);
	return 0;
}

// Queued queries must complete before the pool is released (the release follows the
// usual SuspendThreads() path); no ticket is left waiting forever.
static int lifetime_pool_destruction_completes_queries()
{
	MocPtr moc, ref;
	moc->SetResolution(128, 64); ref->SetResolution(128, 64);
	moc->SetNearClipPlane(0.1f); ref->SetNearClipPlane(0.1f);
	moc->ClearBuffer(); ref->ClearBuffer();

	QuadSet qs;
	int A = qs.add(-0.5f, -0.5f, 0.5f, 0.5f, 2.0f);
	int B = qs.add(-1.0f, -1.0f, 1.0f, 1.0f, 1.5f);

	std::vector<RQ> b1, b2;
	b1.push_back(rq(-0.25f, -0.25f, 0.25f, 0.25f, 3.0f));
	b2.push_back(rq(-0.25f, -0.25f, 0.25f, 0.25f, 3.0f));
	b2.push_back(rq(-0.25f, -0.25f, 0.25f, 0.25f, 1.0f));

	Ticket t1, t2;
	{
		Pool pool(2, 2, 2, 8);
		pool.SetBuffer(moc);
		pool.WakeThreads();
		qs.render(pool, A);
		t1 = pool.EnqueueRectQueries(b1.data(), b1.size());
		qs.render(pool, B);
		t2 = pool.EnqueueRectQueries(b2.data(), b2.size());
		// No Flush and no Wait: the pool is released with queries still in flight
		pool.SuspendThreads();
	}

	qs.render(ref, A);
	std::vector<CR> e1 = refTestAll(ref, b1);
	qs.render(ref, B);
	std::vector<CR> e2 = refTestAll(ref, b2);

	TASK_CHECK(t1.IsReady() && t2.IsReady());
	TASK_CHECK(resultsEqual(t1, e1));
	TASK_CHECK(resultsEqual(t2, e2));
	TASK_CHECK(t1.Results()[0] == M::OCCLUDED);
	TASK_CHECK(t2.Results()[0] == M::OCCLUDED);
	TASK_CHECK(t2.Results()[1] == M::VISIBLE);
	return 0;
}

// Calling Results() before the batch is ready is a clear error (std::logic_error).
static int lifetime_results_before_ready_throws()
{
	MocPtr moc, ref;
	moc->SetResolution(128, 64); ref->SetResolution(128, 64);
	moc->SetNearClipPlane(0.1f); ref->SetNearClipPlane(0.1f);
	moc->ClearBuffer(); ref->ClearBuffer();

	QuadSet qs;
	int A = qs.add(-1.0f, -1.0f, 1.0f, 1.0f, 2.0f);
	std::vector<RQ> batch;
	batch.push_back(rq(-0.5f, -0.5f, 0.5f, 0.5f, 3.0f));

	{
		Pool pool(2, 2, 2, 8);
		pool.SetBuffer(moc);
		pool.WakeThreads();

		// Pause bin 0 for query jobs so the batch cannot complete
		pool.DebugPauseBin(0, false, true);
		BinGuard guard(pool, 0);

		qs.render(pool, A);
		Ticket t = pool.EnqueueRectQueries(batch.data(), batch.size());

		// Handshake: a worker is actually parked on the query job
		TASK_CHECK(waitFor([&] { return pool.DebugIsBinPaused(0); }));
		TASK_CHECK(!t.IsReady());

		bool threw = false;
		try
		{
			(void)t.Results();
		}
		catch (const std::logic_error &)
		{
			threw = true;
		}
		TASK_CHECK(threw);

		// Resume: the batch completes and matches the serial reference
		pool.DebugResumeBin(0);
		t.Wait();
		qs.render(ref, A);
		TASK_CHECK(resultsEqual(t, refTestAll(ref, batch)));
		TASK_CHECK(t.Results()[0] == M::OCCLUDED);

		pool.SuspendThreads();
	}
	return 0;
}

// Default constructed tickets and empty batches are immediately ready; ticket
// copies (copy construction, copy assignment, move) share the same results.
static int lifetime_ticket_copies_and_empty()
{
	// Default constructed ticket: immediately ready, zero results
	Ticket def;
	TASK_CHECK(def.IsReady());
	def.Wait();	// returns immediately
	TASK_CHECK(def.Results().empty());

	MocPtr moc, ref;
	moc->SetResolution(128, 64); ref->SetResolution(128, 64);
	moc->SetNearClipPlane(0.1f); ref->SetNearClipPlane(0.1f);
	moc->ClearBuffer(); ref->ClearBuffer();

	QuadSet qs;
	int A = qs.add(-0.5f, -0.5f, 0.5f, 0.5f, 2.0f);
	std::vector<RQ> batch;
	batch.push_back(rq(-0.25f, -0.25f, 0.25f, 0.25f, 3.0f));

	{
		Pool pool(2, 2, 2, 4);
		pool.SetBuffer(moc);
		pool.WakeThreads();

		// Empty batch: completes immediately without touching the ring
		Ticket empty = pool.EnqueueRectQueries(nullptr, 0);
		TASK_CHECK(empty.IsReady());
		TASK_CHECK(empty.Results().empty());
		Ticket empty2 = pool.EnqueueRectQueries(batch.data(), 0);
		TASK_CHECK(empty2.IsReady());
		TASK_CHECK(empty2.Results().empty());

		qs.render(pool, A);
		Ticket t1 = pool.EnqueueRectQueries(batch.data(), batch.size());
		Ticket t2 = t1;					// copy construction
		Ticket t3; t3 = t1;				// copy assignment
		Ticket t4 = std::move(t2);		// move construction

		t1.Wait();
		qs.render(ref, A);
		std::vector<CR> e = refTestAll(ref, batch);
		TASK_CHECK(resultsEqual(t1, e));
		TASK_CHECK(resultsEqual(t3, e));
		TASK_CHECK(resultsEqual(t4, e));
		TASK_CHECK(&t1.Results() == &t3.Results() && &t1.Results() == &t4.Results());
		TASK_CHECK(t1.Results()[0] == M::OCCLUDED);

		pool.SuspendThreads();
	}
	return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// partition group
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Partitioned execution over a non-uniform bin layout (3x5 bins on 160x96: the last
// column is 96 pixels wide, the last row 32 pixels tall). Covers cross-bin
// rectangles, tail bins, out-of-viewport ranges, equal depth, subtile-sized and
// fractional-pixel rectangles. Every batch is compared against the serial
// reference, and spot-checked against hand-computed expectations.
static int partition_cross_bin_tail_viewport()
{
	MocPtr moc, ref;
	moc->SetResolution(160, 96); ref->SetResolution(160, 96);
	moc->SetNearClipPlane(0.1f); ref->SetNearClipPlane(0.1f);
	moc->ClearBuffer(); ref->ClearBuffer();

	NdcMapper ndc(160.0f, 96.0f);
	QuadSet qs;
	// A: left half (pixels [0,0]-[80,96]) at w=2
	int A = qs.add(ndc.x(0), ndc.y(96), ndc.x(80), ndc.y(0), 2.0f);
	// B: top-right quarter (pixels [80,0]-[160,48]) at w=1.5
	int B = qs.add(ndc.x(80), ndc.y(48), ndc.x(160), ndc.y(0), 1.5f);
	// C: small tail-bin occluder (pixels [96,64]-[128,80]) at w=2.5
	int C = qs.add(ndc.x(96), ndc.y(80), ndc.x(128), ndc.y(64), 2.5f);

	std::vector<RQ> b1, b2, b3;
	// Cross-bin rectangle (spans all three columns and three rows), behind / in front of A
	b1.push_back(ndc.rect(24, 8, 72, 40, 3.0f));	// [0] behind A -> OCCLUDED
	b1.push_back(ndc.rect(24, 8, 72, 40, 1.0f));	// [1] in front of A -> VISIBLE
	// Tail column + tail row rectangle, outside all occluders
	b1.push_back(ndc.rect(100, 60, 150, 90, 3.0f));	// [2] empty -> VISIBLE
	// Rectangle exactly covering bin (1,1), behind A
	b1.push_back(ndc.rect(32, 16, 64, 32, 2.5f));	// [3] behind A -> OCCLUDED
	// Subtile-sized rectangle and a fractional NDC rectangle
	b1.push_back(ndc.rect(40.5f, 10.5f, 41.5f, 11.5f, 3.0f));	// [4] behind A -> OCCLUDED
	b1.push_back(rq(0.13f, -0.71f, 0.44f, 0.29f, 1.0f));		// [5] in front -> VISIBLE

	// Tail column rectangle inside B, behind / in front
	b2.push_back(ndc.rect(90, 10, 150, 40, 3.0f));	// [0] behind B -> OCCLUDED
	b2.push_back(ndc.rect(90, 10, 150, 40, 1.0f));	// [1] in front of B -> VISIBLE
	// Partially and fully out-of-viewport rectangles (clamped by TestRect rules)
	b2.push_back(ndc.rect(150, 10, 200, 30, 3.0f));	// [2] partially offscreen, behind B -> OCCLUDED
	b2.push_back(ndc.rect(170, 10, 190, 30, 3.0f));	// [3] fully offscreen right -> matches reference
	b2.push_back(ndc.rect(10, 100, 30, 120, 3.0f));	// [4] fully offscreen bottom -> matches reference

	// Equal-depth and near-depth rectangles against C in the tail bin. The rects are
	// inset inside C's coverage so the conservative subtile padding of TestRect()
	// cannot reach uncovered subtiles.
	b3.push_back(ndc.rect(100, 68, 124, 76, 2.5f));	// [0] equal depth as C -> VISIBLE
	b3.push_back(ndc.rect(100, 68, 124, 76, 2.6f));	// [1] just behind C -> OCCLUDED
	b3.push_back(ndc.rect(130, 80, 159, 95, 1.0f));	// [2] tail corner, empty -> VISIBLE

	Ticket t1, t2, t3;
	std::vector<CR> e1, e2, e3;
	{
		Pool pool(3, 3, 5, 8);
		pool.SetBuffer(moc);
		pool.WakeThreads();

		qs.render(pool, A);
		t1 = pool.EnqueueRectQueries(b1.data(), b1.size());
		qs.render(pool, B);
		t2 = pool.EnqueueRectQueries(b2.data(), b2.size());
		qs.render(pool, C);
		t3 = pool.EnqueueRectQueries(b3.data(), b3.size());

		qs.render(ref, A);
		e1 = refTestAll(ref, b1);
		qs.render(ref, B);
		e2 = refTestAll(ref, b2);
		qs.render(ref, C);
		e3 = refTestAll(ref, b3);

		pool.SuspendThreads();
	}

	t1.Wait(); t2.Wait(); t3.Wait();
	TASK_CHECK(resultsEqual(t1, e1));
	TASK_CHECK(resultsEqual(t2, e2));
	TASK_CHECK(resultsEqual(t3, e3));

	// Hand-computed content expectations
	TASK_CHECK(t1.Results()[0] == M::OCCLUDED);
	TASK_CHECK(t1.Results()[1] == M::VISIBLE);
	TASK_CHECK(t1.Results()[2] == M::VISIBLE);
	TASK_CHECK(t1.Results()[3] == M::OCCLUDED);
	TASK_CHECK(t1.Results()[4] == M::OCCLUDED);
	TASK_CHECK(t1.Results()[5] == M::VISIBLE);
	TASK_CHECK(t2.Results()[0] == M::OCCLUDED);
	TASK_CHECK(t2.Results()[1] == M::VISIBLE);
	TASK_CHECK(t2.Results()[2] == M::OCCLUDED);
	TASK_CHECK(t3.Results()[0] == M::VISIBLE);	// equal depth is visible
	TASK_CHECK(t3.Results()[1] == M::OCCLUDED);
	TASK_CHECK(t3.Results()[2] == M::VISIBLE);
	return 0;
}

// Parallelism proof with timeout-guarded handshakes: with bin 0 parked on a query
// job, the other bins must still advance - both through the query batch itself and
// through drawcalls submitted after it - while bin 0's own successor work stays
// queued behind the parked query.
static int partition_parallel_bin_progress()
{
	MocPtr moc, ref;
	moc->SetResolution(128, 64); ref->SetResolution(128, 64);
	moc->SetNearClipPlane(0.1f); ref->SetNearClipPlane(0.1f);
	moc->ClearBuffer(); ref->ClearBuffer();

	QuadSet qs;
	int A = qs.add(-1.0f, -1.0f, 1.0f, 1.0f, 2.0f);		// fullscreen, far
	int B = qs.add(-0.5f, -0.5f, 0.5f, 0.5f, 1.5f);		// center, nearer

	std::vector<RQ> Q, Q2;
	Q.push_back(rq(-0.25f, -0.25f, 0.25f, 0.25f, 3.0f));
	Q.push_back(rq(-0.25f, -0.25f, 0.25f, 0.25f, 1.0f));
	Q2.push_back(rq(-0.9f, -0.9f, -0.6f, -0.6f, 3.0f));
	Q2.push_back(rq(0.6f, 0.6f, 0.9f, 0.9f, 1.0f));

	std::vector<CR> e1, e2;
	{
		Pool pool(2, 2, 2, 8);
		pool.SetBuffer(moc);
		pool.WakeThreads();

		// Park bin 0 on query jobs only; its drawcalls still proceed
		pool.DebugPauseBin(0, false, true);
		BinGuard guard(pool, 0);

		qs.render(pool, A);
		Ticket t1 = pool.EnqueueRectQueries(Q.data(), Q.size());
		qs.render(pool, B);
		Ticket t2 = pool.EnqueueRectQueries(Q2.data(), Q2.size());

		// Handshake: bin 0 is parked on the first query batch
		TASK_CHECK(waitFor([&] { return pool.DebugIsBinPaused(0); }));

		// While bin 0 is parked, the other 3 bins complete their parts of t1...
		TASK_CHECK(waitFor([&] { return pool.DebugBinsCompleted(t1) == 3; }));
		// ...and also render the successor drawcall B and complete their parts of t2,
		// proving that successor draws proceed in parallel with bin 0's pending query
		TASK_CHECK(waitFor([&] { return pool.DebugBinsCompleted(t2) == 3; }));

		// The batches themselves are not published while bin 0 has not contributed
		TASK_CHECK(!t1.IsReady());
		TASK_CHECK(!t2.IsReady());

		// Resume: both batches complete and match the serial reference
		pool.DebugResumeBin(0);
		t1.Wait();
		t2.Wait();

		qs.render(ref, A);
		e1 = refTestAll(ref, Q);
		qs.render(ref, B);
		e2 = refTestAll(ref, Q2);
		TASK_CHECK(resultsEqual(t1, e1));
		TASK_CHECK(resultsEqual(t2, e2));

		pool.SuspendThreads();
	}
	return 0;
}

// Wait() on a ticket only waits for its own batch, not for drawcalls submitted
// after it: a ticket becomes (and stays) ready while a successor drawcall is still
// blocked inside the pipeline.
static int partition_wait_not_blocked_by_successor_draws()
{
	MocPtr moc, ref;
	moc->SetResolution(128, 64); ref->SetResolution(128, 64);
	moc->SetNearClipPlane(0.1f); ref->SetNearClipPlane(0.1f);
	moc->ClearBuffer(); ref->ClearBuffer();

	QuadSet qs;
	int D = qs.add(-0.5f, -0.5f, 0.5f, 0.5f, 1.5f);		// center occluder

	std::vector<RQ> Q, Q3;
	Q.push_back(rq(-0.25f, -0.25f, 0.25f, 0.25f, 3.0f));
	Q.push_back(rq(0.6f, 0.6f, 0.9f, 0.9f, 1.0f));
	Q3.push_back(rq(-0.25f, -0.25f, 0.25f, 0.25f, 3.0f));

	std::vector<CR> e1, e3;
	{
		Pool pool(2, 2, 2, 8);
		pool.SetBuffer(moc);
		pool.WakeThreads();

		// Bin 1 pauses on drawcalls only; query jobs pass through
		pool.DebugPauseBin(1, true, false);
		BinGuard guard1(pool, 1);

		// Submit the query batch first: all bins (including bin 1) process it
		Ticket t1 = pool.EnqueueRectQueries(Q.data(), Q.size());

		// Submit a successor drawcall: bin 1 parks on it, the other bins render it
		qs.render(pool, D);
		Ticket t3 = pool.EnqueueRectQueries(Q3.data(), Q3.size());	// queued behind D

		// Handshake: bin 1 is parked on the successor drawcall D
		TASK_CHECK(waitFor([&] { return pool.DebugBinParkCount(1) == 1; }));
		TASK_CHECK(pool.DebugIsBinPaused(1));

		// The ticket for Q is ready even though the successor drawcall D is still
		// blocked inside the pipeline: waiting on a ticket does not wait for later
		// drawcalls. t3, queued behind D, is not ready.
		TASK_CHECK(waitFor([&] { return t1.IsReady(); }));
		t1.Wait();	// returns immediately
		TASK_CHECK(!t3.IsReady());
		TASK_CHECK(pool.DebugIsBinPaused(1));	// D is still blocked

		// t1's prefix contains no drawcalls: compare against the empty reference
		e1 = refTestAll(ref, Q);
		TASK_CHECK(resultsEqual(t1, e1));
		TASK_CHECK(t1.Results()[0] == M::VISIBLE);
		TASK_CHECK(t1.Results()[1] == M::VISIBLE);

		// Release bin 1: D lands and t3 sees exactly the D prefix
		pool.DebugResumeBin(1);
		t3.Wait();
		qs.render(ref, D);
		e3 = refTestAll(ref, Q3);
		TASK_CHECK(resultsEqual(t3, e3));
		TASK_CHECK(t3.Results()[0] == M::OCCLUDED);

		pool.SuspendThreads();
	}
	return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Case registration
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

const struct task_case task_cases[] =
{
	{ "prefix", "draw_query_draw_query", prefix_draw_query_draw_query },
	{ "prefix", "ring_overflow", prefix_ring_overflow },
	{ "prefix", "clear_setbuffer_resize", prefix_clear_setbuffer_resize },
	{ "lifetime", "results_survive_ops_and_pool", lifetime_results_survive_ops_and_pool },
	{ "lifetime", "pool_destruction_completes_queries", lifetime_pool_destruction_completes_queries },
	{ "lifetime", "results_before_ready_throws", lifetime_results_before_ready_throws },
	{ "lifetime", "ticket_copies_and_empty", lifetime_ticket_copies_and_empty },
	{ "partition", "cross_bin_tail_viewport", partition_cross_bin_tail_viewport },
	{ "partition", "parallel_bin_progress", partition_parallel_bin_progress },
	{ "partition", "wait_not_blocked_by_successor_draws", partition_wait_not_blocked_by_successor_draws },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
