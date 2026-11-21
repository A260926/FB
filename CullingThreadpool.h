////////////////////////////////////////////////////////////////////////////////
// Copyright 2017 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License.  You may obtain a copy
// of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
// License for the specific language governing permissions and limitations
// under the License.
////////////////////////////////////////////////////////////////////////////////
#pragma once

 /*!
  * \file CullingThreadpool.h
  * \brief Worker threadpool example for threaded masked occlusion culling.
  *
  * This class implements a threadpool for occluder rendering. Calls to CullingThreadpool::RenderTriangle()
  * will immediately return, after adding work items to a queue, and occluder rendering is performed
  * by worker threads as quickly as possible. Occlusion queries are performed directly on the calling 
  * threadand can be performed either synchronosly, by calling Flush() before executing the query, or 
  * asynchronosly, by performing the query without waiting for the worker threads to finish. 
  *
  * Note that this implementation should be considered an example rather than the best threading
  * solution. You may want to integrate threading in your own task system, and it may also be beneficial 
  * to thread the traversal code. Refer to MaskedOcclusionCulling::BinTriangles() and 
  * MaskedOcclusionCulling::RenderTrilist() for functions that can be used to make your own 
  * threaded culling system. 
  */

#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <vector>

#include "MaskedOcclusionCulling.h"

class CullingThreadpool
{
protected:
	static const int TRIS_PER_JOB = 1024; // Maximum number of triangles per job (bigger drawcalls are split), affects memory requirements

	typedef MaskedOcclusionCulling::CullingResult   CullingResult;
	typedef MaskedOcclusionCulling::ClipPlanes      ClipPlanes;
	typedef MaskedOcclusionCulling::BackfaceWinding BackfaceWinding;
	typedef MaskedOcclusionCulling::ScissorRect     ScissorRect;
	typedef MaskedOcclusionCulling::VertexLayout    VertexLayout;
	typedef MaskedOcclusionCulling::TriList         TriList;

	// Shared state for one batch of queued rectangle queries, see EnqueueRectQueries().
	// The state is owned jointly by the QueryTicket(s) held by the application and by
	// the job ring slot referencing it, so results remain valid after the pool (or the
	// occlusion buffer) has moved on. Defined in CullingThreadpool.cpp.
	struct QueryBatch;

	// Small utility class for 4x4 matrices
	struct Matrix4x4
	{
		float mValues[16];
		Matrix4x4() {}
		Matrix4x4(const float *matrix)
		{
			for (int i = 0; i < 16; ++i)
				mValues[i] = matrix[i];
		}
	};

	// Internal utility class for a (mostly) lockless queue for binning & rendering jobs
	struct RenderJobQueue
	{
		struct BinningJob
		{
			const float*        mVerts;
			const unsigned int* mTris;
			unsigned int        nTris;

			const float*        mMatrix;
			ClipPlanes          mClipPlanes;
			BackfaceWinding     mBfWinding;
			const VertexLayout* mVtxLayout;
		};

		struct Job
		{
			volatile unsigned int mBinningJobStartedIdx;
			volatile unsigned int mBinningJobCompletedIdx;
			BinningJob            mBinningJob;
			TriList               *mRenderJobs;

			// If non-null, this slot is a rectangle query batch job rather than a
			// drawcall. The slot keeps the batch alive until all bins have consumed
			// the job and the slot is recycled. Query jobs skip binning and are
			// executed per bin, in ring order, at the render stage.
			std::shared_ptr<QueryBatch> mQueryBatch;
		};

		unsigned int          mNumBins;
		unsigned int          mMaxJobs;

		volatile unsigned int mWritePtr;
		std::atomic_uint      mBinningPtr;
		std::atomic_uint      *mRenderPtrs;
		std::atomic_uint      *mBinMutexes;

		float                 *mTrilistData;
		Job                   *mJobs;

		RenderJobQueue(unsigned int nBins, unsigned int maxJobs);
		~RenderJobQueue();

		unsigned int GetMinRenderPtr() const;
		unsigned int GetBestGlobalQueue() const;
		bool IsPipelineEmpty() const;

		bool CanWrite() const;
		bool CanBin() const;

		Job *GetWriteJob();
		void AdvanceWriteJob();

		Job *GetBinningJob();
		void FinishedBinningJob(Job *job);

		Job *GetRenderJob(int binIdx);
		void AdvanceRenderJob(int binIdx);

		void Reset();
	};

	// Internal utility class for state (matrix / vertex layout)
	template<class T> struct StateData
	{
		unsigned int mMaxJobs;
		unsigned int mCurrentIdx;
		T            *mData;

		StateData(unsigned int maxJobs);
		~StateData();
		void AddData(const T &data);
		const T *GetData() const;
	};

	// Number of worker threads and bins
	unsigned int            mNumThreads;
	unsigned int            mNumBins;
	unsigned int            mMaxJobs;
	unsigned int            mBinsW;
	unsigned int            mBinsH;

	// Threads and control variables
	std::mutex              mSuspendedMutex;
	std::condition_variable mSuspendedCV;
	volatile bool           mKillThreads;
	volatile bool           mSuspendThreads;
	volatile unsigned int   mNumSuspendedThreads;
	std::thread             *mThreads;

	// State variables and command queue
	const float             *mCurrentMatrix;
	StateData<Matrix4x4>    mModelToClipMatrices;
	StateData<VertexLayout> mVertexLayouts;
	RenderJobQueue          *mRenderQueue;

	// Occlusion culling object and related scissor rectangles
	ScissorRect             *mRects;
	MaskedOcclusionCulling  *mMOC;

	// Test hooks for pausing individual bins (used by the conformance checks). All
	// flags are cleared by default, and in that state the hooks are a single atomic
	// load per processed job and do not alter normal scheduling in any way.
	struct BinPauseState
	{
		std::atomic_bool mPauseRequested;  // Set by the calling thread to park the bin
		std::atomic_bool mPauseDraws;      // Pause before drawcall (trilist) jobs
		std::atomic_bool mPauseQueries;    // Pause before query batch jobs
		std::atomic_bool mPaused;          // Set by the worker while parked (handshake)
		std::atomic_uint mParkCount;       // Number of times a worker has parked on this bin
	};
	BinPauseState           *mBinPause;

	void SetupScissors();

	// Processes the part of a query batch job overlapping the given bin, and publishes
	// the combined batch results once every bin has contributed.
	void ProcessQueryBatch(QueryBatch *batch, unsigned int binIdx);
	static void PublishQueryBatch(QueryBatch *batch);

	// Test hook pause point, executed by a worker after claiming a job for a bin.
	void BinPausePoint(unsigned int binIdx, bool isQueryJob);

	static void ThreadRun(CullingThreadpool *threadPool, unsigned int threadId);
	void ThreadMain(unsigned int threadIdx);

public:
	/*!
	 * \brief Creates a new threadpool for masked occlusion culling. This object has a 
	 *        similar API to the MaskedOcclusionCulling class, but performs occluder
	 *        rendering asynchronously on worker threads (similar to how DX/GL works). 
	 *
	 * \param numThreads Number of worker threads to perform occluder rendering. Best 
	 *        balance may be scene/machine dependent, but it's good practice to leave at 
	 *        least one full core (2 threads with hyperthreading) for the main thread.
	 * \param binsW The screen is divided into binsW x binsH rectangular bins for load
	 *        balancing. The number of bins should be atleast equal to the number of
	 *        worker threads.
	 * \param binsH See description for the binsW parameter.
	 * \param maxJobs Maximum number of jobs that may be in flight at any given time. If
	 *        the caller thread generates jobs faster than the worker threads can finish
	 *        them, then the job queue will fill up and the caller thread will stall once
	 *        "maxJobs" items have been queued up. For culling systems interleaving occlusion 
	 *        queries and rendering, this value should be kept quite low to minimize false
	 *        positives (see TestRect()). We've observed that 32 [default] items typically
	 *        works well for our interleaved queries, while also allowing good load-balancing,
	 *        and this is the recommended setting. 
	 */
	CullingThreadpool(unsigned int numThreads, unsigned int binsW, unsigned int binsH, unsigned int maxJobs = 32);
	
	/*!
	 * \brief Destroys the threadpool and terminates all worker threads.
	 */
	~CullingThreadpool();

	/*!
	 * \brief Wakes up culling worker threads from suspended sleep, and puts them in a
	 *        ready state (using an idle spinlock with significantly higher CPU overhead). 
	 *
	 * It may take on the order of 100us to wake up the threads, so this function should
	 * preferably be called slightly ahead of starting occlusion culling work.
	 */
	void WakeThreads();

	/*!
	 * \brief Suspend all culling worker threads to a low CPU overhead sleep state. 
	 *
	 * For performance and latency reasons, the culling work is performed in an active 
	 * processing loop (with no thread sleeping) with high CPU overhead. In a system 
	 * with more worker threads it's important to put the culling worker threads in a 
	 * low overhead sleep state after occlusion culling work has completed.
	 */
	void SuspendThreads();

	/*!
	 * \brief Waits for all outstanding occluder rendering work to complete. Can be used
	 *        to ensure that rendering has completed before performing a TestRect() or 
	 *        TestTriangles() call.
	 */
	void Flush();

	/*
	 * \brief Sets the MaskedOcclusionCulling object (buffer) to be used for rendering and
	 *        testing calls. This method causes a Flush() to ensure that all unfinished 
	 *        rendering is completed.
	 */
	void SetBuffer(MaskedOcclusionCulling *moc);

	/*
	 * \brief Changes the resolution of the occlusion buffer, see MaskedOcclusionCulling::SetResolution().
	 *        This method causes a Flush() to ensure that all unfinished rendering is completed.
	 */
	void SetResolution(unsigned int width, unsigned int height);

	/*
	 * \brief Sets the near clipping plane, see MaskedOcclusionCulling::SetNearClipPlane(). This 
	 *        method causes a Flush() to ensure that all unfinished rendering is completed.
	 */
	void SetNearClipPlane(float nearDist);

	/*
	 * \brief Sets the model to clipspace transform matrix used for the RenderTriangles() and TestTriangles() 
	 *        function calls. The contents of the matrix is copied, and it's safe to modify it without calling
	 *        Flush(). The copy may be costly, which is the reason for passing this parameter as "state".
	 *
	 * \param modelToClipMatrix All vertices will be transformed by the specified model to clipspace matrix. 
	 *        Passing nullptr [default] disables the transform (equivalent to using an identity matrix).
	 */
	void SetMatrix(const float *modelToClipMatrix = nullptr);

	/*
	 * \brief Sets the vertex layout used for the RenderTriangles() and TestTriangles() function calls.
	 *        The vertex layout is copied, and it's safe to modify it without calling Flush(). The copy 
	 *        may be costly, which is the reason for passing this parameter as "state".
	 *
	 * \param vtxLayout A struct specifying the vertex layout (see struct for detailed
	 *        description). For best performance, it is advicable to store position data
	 *        as compactly in memory as possible.
	 */
	void SetVertexLayout(const VertexLayout &vtxLayout = VertexLayout(16, 4, 12));

	/*
	 * \brief Clears the occlusion buffer, see MaskedOcclusionCulling::ClearBuffer(). This method
	 *        causes a Flush() to ensure that all unfinished rendering is completed.
	 */
	void ClearBuffer();

	/*
	 * \brief Asynchronously render occluder triangles, see MaskedOcclusionCulling::RenderTriangles().
	 *
	 * This method puts the drawcall into a command queue, and immediately returns. The rendering is 
	 * performed by the worker threads at the earliest opportunity.
	 *
	 * <B>Important:</B> As rendering is performed asynchronously, the application is not allowed to 
	 * change the contents of the *inVtx or *inTris buffers until after rendering is completed. If 
	 * you wish to use dynamic buffers, the application must perform a Flush() to ensure that rendering 
	 * is finished, or make sure to rotate between more buffers than the maximum number of outstanding
	 * render jobs (see the CullingThreadpool() constructor).
	 */
	void RenderTriangles(const float *inVtx, const unsigned int *inTris, int nTris, BackfaceWinding bfWinding = MaskedOcclusionCulling::BACKFACE_CW, ClipPlanes clipPlaneMask = MaskedOcclusionCulling::CLIP_PLANE_ALL);

	/*
	 * \brief Occlusion query for a rectangle with a given depth, see MaskedOcclusionCulling::TestRect().
	 *
	 * <B>Important:</B> This method is performed on the main thread and does not wait for outstanding 
	 * occluder rendering to be finished. To ensure that all occluder rendering is completed you must 
	 * perform a Flush() prior to calling this function. 
	 *
	 * It is conservatively correct to perform occlusion queries without calling Flush() (it may only 
	 * lead to objects being incorrectly classified as visible), and it can lead to much better performance 
	 * if occlusion queries are used for traversing a BVH or similar data structure. It's possible to 
	 * use "asynchronous" queries during traversal, and removing false positives later, when rendering 
	 * has completed.
	 */
	CullingResult TestRect(float xmin, float ymin, float xmax, float ymax, float wmin);

	/*
	 * \brief Occlusion query for a mesh, see MaskedOcclusionCulling::TestTriangles().
	 *
	 * <B>Important:</B> See the TestRect() method for a brief discussion about asynchronous occlusion 
	 * queries.
	 */
	CullingResult TestTriangles(const float *inVtx, const unsigned int *inTris, int nTris, BackfaceWinding bfWinding = MaskedOcclusionCulling::BACKFACE_CW, ClipPlanes clipPlaneMask = MaskedOcclusionCulling::CLIP_PLANE_ALL);

	/*!
	 * \brief Creates a per-pixel depth buffer from the hierarchical z buffer representation, see
	 *        MaskedOcclusionCulling::ComputePixelDepthBuffer(). This method causes a Flush() to
	 *        ensure that all unfinished rendering is completed.
	 */
	void ComputePixelDepthBuffer(float *depthData, bool flipY);

	/*!
	 * \brief A single rectangle occlusion query, see MaskedOcclusionCulling::TestRect()
	 *        for the exact meaning of the coordinates and the depth test rules. The
	 *        coordinates are finite and must satisfy xmin < xmax, ymin < ymax and
	 *        wmin > 0.
	 */
	struct RectQuery
	{
		float mXmin;	//!< NDC coordinate of the left side of the rectangle
		float mYmin;	//!< NDC coordinate of the bottom side of the rectangle
		float mXmax;	//!< NDC coordinate of the right side of the rectangle
		float mYmax;	//!< NDC coordinate of the top side of the rectangle
		float mWmin;	//!< Clip space W coordinate for the rectangle
	};

	/*!
	 * \brief Owning handle to the results of one EnqueueRectQueries() batch.
	 *
	 * A ticket is a lightweight, copyable handle: copies share the same underlying
	 * batch results. The results are owned by the ticket (not by the threadpool),
	 * so once ready they remain valid after the job ring slots have been recycled,
	 * and after ClearBuffer(), SetBuffer(), SetResolution() or even destruction of
	 * the threadpool itself.
	 *
	 * Tickets are read-only objects: IsReady(), Wait() and Results() may be called
	 * from threads other than the calling (submitting) thread, but note that all
	 * state-changing threadpool calls (including EnqueueRectQueries()) must be
	 * issued from the single calling thread, in order.
	 */
	class QueryTicket
	{
	public:
		/*!
		 * \brief Creates an empty ticket. An empty ticket is immediately ready and
		 *        holds zero results (identical to a ticket from an empty batch).
		 */
		QueryTicket();
		QueryTicket(const QueryTicket &other);
		QueryTicket(QueryTicket &&other) noexcept;
		QueryTicket &operator=(const QueryTicket &other);
		QueryTicket &operator=(QueryTicket &&other) noexcept;
		~QueryTicket();

		/*!
		 * \brief Returns true once all results of this batch have been published.
		 *        Always true for an empty (default constructed) ticket.
		 */
		bool IsReady() const;

		/*!
		 * \brief Blocks until the results of this batch have been published. Waits
		 *        only for this ticket's own batch: it does not wait for drawcalls
		 *        or query batches that were submitted after this batch.
		 */
		void Wait() const;

		/*!
		 * \brief Returns the occlusion query results, one entry per submitted
		 *        RectQuery, in submission order. Each entry is the VISIBLE /
		 *        OCCLUDED / VIEW_CULLED result that MaskedOcclusionCulling::TestRect()
		 *        would have returned if called on the occlusion buffer containing
		 *        exactly the drawcalls submitted before this batch.
		 *
		 * <B>Important:</B> Calling this function before IsReady() returns true is
		 *        an error and throws a std::logic_error exception. Call Wait() (or
		 *        poll IsReady()) first.
		 */
		const std::vector<CullingResult> &Results() const;

	private:
		friend class CullingThreadpool;
		explicit QueryTicket(const std::shared_ptr<QueryBatch> &batch);
		std::shared_ptr<QueryBatch> mBatch;
	};

	/*!
	 * \brief Enqueues a batch of rectangle occlusion queries to be executed by the
	 *        worker threads, and returns a ticket used to retrieve the results.
	 *
	 * The query array is copied at submission time, so the caller may reuse or free
	 * the input memory immediately. An empty batch (numQueries == 0) completes
	 * immediately and does not occupy a job ring slot.
	 *
	 * The batch is inserted into the same bounded job ring as drawcalls: each bin
	 * executes its part of the batch after the drawcalls submitted before the batch
	 * (and before any drawcalls submitted after it), reading only the depth tiles
	 * owned by that bin. The combined result for each rectangle is therefore exactly
	 * the result of calling MaskedOcclusionCulling::TestRect() on the buffer
	 * containing only the drawcalls submitted before the batch; occluders submitted
	 * later never affect it. Submitting a batch never flushes the pipeline and never
	 * waits for outstanding drawcalls to finish; the calling thread only blocks if
	 * the job ring is full, waiting for a recyclable slot (see the maxJobs parameter
	 * of the constructor).
	 *
	 * <B>Important:</B> Like all other state-changing methods of this class, this
	 * function must be called from the single calling thread. The returned ticket
	 * may be read from any thread.
	 *
	 * \param queries Pointer to an array of numQueries rectangle queries, copied at
	 *        submission time. May be nullptr if numQueries is 0.
	 * \param numQueries Number of queries in the batch.
	 * \return A ticket used to poll (IsReady()), wait for (Wait()) and fetch
	 *         (Results()) the batch results.
	 */
	QueryTicket EnqueueRectQueries(const RectQuery *queries, size_t numQueries);

	/*!
	 * \brief Test hook: requests that workers pause before processing jobs of the
	 *        selected types for the given bin. The bin is parked (while holding its
	 *        bin slot) until DebugResumeBin() is called. Other bins keep making
	 *        progress. Disabled by default; when disabled the hooks do not alter
	 *        normal scheduling. Intended for the conformance checks only.
	 */
	void DebugPauseBin(unsigned int binIdx, bool pauseDraws, bool pauseQueries);

	/*!
	 * \brief Test hook: releases a bin previously paused with DebugPauseBin().
	 */
	void DebugResumeBin(unsigned int binIdx);

	/*!
	 * \brief Test hook: returns true once a worker thread is actually parked inside
	 *        the pause requested by DebugPauseBin() (synchronization handshake).
	 */
	bool DebugIsBinPaused(unsigned int binIdx) const;

	/*!
	 * \brief Test hook: returns the number of times a worker has parked on the given
	 *        bin. Used by the conformance checks to detect which job a bin is
	 *        currently parked on.
	 */
	unsigned int DebugBinParkCount(unsigned int binIdx) const;

	/*!
	 * \brief Test hook: returns the number of bins that have completed their part of
	 *        the given ticket's batch. Used by the conformance checks to verify that
	 *        bins make independent progress.
	 */
	unsigned int DebugBinsCompleted(const QueryTicket &ticket) const;
};
