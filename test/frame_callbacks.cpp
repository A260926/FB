//****************************************************************************//
// frame_callbacks.cpp                                                        //
// Task tests for the per-CalModel frame callback transaction.                //
//                                                                            //
// Registered through task_cases/task_case_count in four groups:              //
// committed_pose, mutation_order, registry_clock, isolation.                 //
// Every numerical expectation is computed independently from the core        //
// keyframes and the inverse-bind skinning formula, then compared both with   //
// what the callbacks observe and with the post-update engine state.          //
//                                                                            //
// This library is free software; you can redistribute it and/or modify it    //
// under the terms of the GNU Lesser General Public License as published by   //
// the Free Software Foundation; either version 2.1 of the License, or (at    //
// your option) any later version.                                            //
//****************************************************************************//

#include "task_support/check.h"

#include "cal3d/cal3d.h"
#include "cal3d/coretrack.h"
#include "cal3d/corekeyframe.h"
#include "cal3d/framecallbacks.h"

#include <cmath>
#include <cstddef>
#include <vector>

using namespace cal3d;

namespace
{
	const float EPS = 1e-4f;

	bool vecClose(const CalVector &a, const CalVector &b)
	{
		return std::fabs(a.x - b.x) < EPS
			&& std::fabs(a.y - b.y) < EPS
			&& std::fabs(a.z - b.z) < EPS;
	}

	bool floatClose(float a, float b)
	{
		return std::fabs(a - b) < EPS;
	}

	// Record a boolean condition reached from inside a void callback. The
	// flag is asserted from the test body with an ordinary TASK_CHECK.
	struct CallbackFlag
	{
		bool ok;
		CallbackFlag() : ok(true) { }
		void check(bool condition) { if (!condition) ok = false; }
	};

	// ---------------------------------------------------------------------
	// Recording callback. It captures, for every event, the originating
	// model, the animation time and the committed skeleton/skin.
	// ---------------------------------------------------------------------
	struct Recorder : CalAnimationCallback
	{
		struct Event
		{
			int         kind;     // 0 = update, 1 = complete
			int         tag;
			float       animTime;
			CalModel   *model;
			CalVector   root;
			CalVector   child;
			CalVector   skin0;
			CalVector   skin1;
			size_t      ordinal;  // global dispatch order within the model
		};

		int                 tag;
		CalSubmesh         *sub;
		std::vector<Event>  events;
		size_t             *orderCounter;

		Recorder(int t, CalSubmesh *s, size_t *counter)
			: tag(t), sub(s), orderCounter(counter) { }

		void snapshot(Event &e, float animTime, CalModel *model)
		{
			e.tag = tag;
			e.animTime = animTime;
			e.model = model;
			e.root = model->getSkeleton()->getBone(0)->getTranslationAbsolute();
			e.child = model->getSkeleton()->getBone(1)->getTranslationAbsolute();
			e.skin0 = model->getPhysique()->calculateVertex(sub, 0);
			e.skin1 = model->getPhysique()->calculateVertex(sub, 1);
			e.ordinal = (*orderCounter)++;
		}

		virtual void AnimationUpdate(float anim_time, CalModel *model, void *)
		{
			Event e;
			e.kind = 0;
			snapshot(e, anim_time, model);
			events.push_back(e);
		}

		virtual void AnimationComplete(CalModel *model, void *)
		{
			Event e;
			e.kind = 1;
			snapshot(e, -1.0f, model);
			events.push_back(e);
		}
	};

	// ---------------------------------------------------------------------
	// Shared fixture: two bones with non-zero inverse bind, three core
	// animations and one weighted two-vertex mesh.
	// ---------------------------------------------------------------------
	struct Scene
	{
		CalCoreModel     *core;
		CalCoreSkeleton  *skel;
		CalCoreAnimation *anim[3];
		CalCoreTrack     *trackRoot[3];
		CalCoreTrack     *trackChild[3];
		CalCoreMesh      *mesh;
		CalCoreSubmesh   *coreSub;
		CalVector         rest0;
		CalVector         rest1;

		Scene()
		{
			core = new CalCoreModel("frame-scene");
			skel = new CalCoreSkeleton;

			CalCoreBone *root = new CalCoreBone("root");
			root->setParentId(-1);
			root->setTranslation(CalVector(2.0f, 0.0f, 0.0f));
			root->setRotation(CalQuaternion(0, 0, 0, 1));
			root->setTranslationBoneSpace(CalVector(-2.0f, 0.0f, 0.0f));
			root->setRotationBoneSpace(CalQuaternion(0, 0, 0, 1));
			root->setCoreSkeleton(skel);
			skel->addCoreBone(root);

			CalCoreBone *child = new CalCoreBone("child");
			child->setParentId(0);
			child->setTranslation(CalVector(0.0f, 3.0f, 0.0f));
			child->setRotation(CalQuaternion(0, 0, 0, 1));
			child->setTranslationBoneSpace(CalVector(0.0f, -3.0f, 0.0f));
			child->setRotationBoneSpace(CalQuaternion(0, 0, 0, 1));
			child->setCoreSkeleton(skel);
			skel->addCoreBone(child);
			root->addChildId(1);

			skel->calculateState();
			core->setCoreSkeleton(skel);

			// anim 0: root x 2 -> 6,  child y 3 -> 9,  duration 2s
			// anim 1: root x 10 -> 14, child y 1 -> 5,  duration 2s
			// anim 2: root x 20 -> 24, child y 7 -> 11, duration 1s
			buildAnim(0, 2.0f, 2.0f, 6.0f, 3.0f, 9.0f);
			buildAnim(1, 2.0f, 10.0f, 14.0f, 1.0f, 5.0f);
			buildAnim(2, 1.0f, 20.0f, 24.0f, 7.0f, 11.0f);

			mesh = new CalCoreMesh;
			coreSub = new CalCoreSubmesh;
			coreSub->reserve(2, 0, 0, 0);

			rest0 = CalVector(3.0f, 1.0f, 0.0f);
			CalCoreSubmesh::Vertex v0;
			v0.position = rest0;
			v0.normal = CalVector(0, 1, 0);
			v0.collapseId = 0;
			v0.faceCollapseCount = 0;
			CalCoreSubmesh::Influence w00; w00.boneId = 0; w00.weight = 0.6f;
			CalCoreSubmesh::Influence w01; w01.boneId = 1; w01.weight = 0.4f;
			v0.vectorInfluence.push_back(w00);
			v0.vectorInfluence.push_back(w01);
			coreSub->setVertex(0, v0);

			rest1 = CalVector(0.0f, 0.0f, 5.0f);
			CalCoreSubmesh::Vertex v1;
			v1.position = rest1;
			v1.normal = CalVector(0, 1, 0);
			v1.collapseId = 1;
			v1.faceCollapseCount = 0;
			CalCoreSubmesh::Influence w11; w11.boneId = 1; w11.weight = 1.0f;
			v1.vectorInfluence.push_back(w11);
			coreSub->setVertex(1, v1);

			mesh->addCoreSubmesh(coreSub);
			core->addCoreMesh(mesh);
		}

		void buildAnim(int id, float dur,
			float rootX0, float rootX1, float childY0, float childY1)
		{
			CalCoreAnimation *a = new CalCoreAnimation;
			a->setDuration(dur);

			CalCoreTrack *tr = new CalCoreTrack;
			tr->setCoreBoneId(0);
			tr->setTranslationRequired(true);
			CalCoreKeyframe *ka = new CalCoreKeyframe;
			ka->setTime(0.0f);
			ka->setTranslation(CalVector(rootX0, 0, 0));
			ka->setRotation(CalQuaternion(0, 0, 0, 1));
			tr->addCoreKeyframe(ka);
			CalCoreKeyframe *kb = new CalCoreKeyframe;
			kb->setTime(dur);
			kb->setTranslation(CalVector(rootX1, 0, 0));
			kb->setRotation(CalQuaternion(0, 0, 0, 1));
			tr->addCoreKeyframe(kb);
			a->addCoreTrack(tr);
			trackRoot[id] = tr;

			CalCoreTrack *tc = new CalCoreTrack;
			tc->setCoreBoneId(1);
			tc->setTranslationRequired(true);
			CalCoreKeyframe *ca = new CalCoreKeyframe;
			ca->setTime(0.0f);
			ca->setTranslation(CalVector(0, childY0, 0));
			ca->setRotation(CalQuaternion(0, 0, 0, 1));
			tc->addCoreKeyframe(ca);
			CalCoreKeyframe *cb = new CalCoreKeyframe;
			cb->setTime(dur);
			cb->setTranslation(CalVector(0, childY1, 0));
			cb->setRotation(CalQuaternion(0, 0, 0, 1));
			tc->addCoreKeyframe(cb);
			a->addCoreTrack(tc);
			trackChild[id] = tc;

			anim[id] = a;
			core->addCoreAnimation(a);
		}

		// Independent local-pose sampling of one core animation at time t.
		void sampleLocal(int id, float t, CalVector &rootLocal,
			CalVector &childLocal) const
		{
			CalQuaternion q;
			trackRoot[id]->getState(t, rootLocal, q);
			trackChild[id]->getState(t, childLocal, q);
		}

		// Independent inverse-bind skinning, mirroring CalPhysique.
		CalVector skinPoint(const CalVector &rest, int boneId,
			const CalVector &absPos, const CalQuaternion &absRot) const
		{
			const CalCoreBone *cb = skel->getCoreBone(boneId);
			CalMatrix rot(cb->getRotationBoneSpace());
			rot *= CalMatrix(absRot);
			CalVector v(rest);
			v *= rot;
			CalVector boneSpace = cb->getTranslationBoneSpace();
			boneSpace *= absRot;
			boneSpace += absPos;
			v += boneSpace;
			return v;
		}
	};

	// A freshly attached model over the shared scene.
	struct ModelBox
	{
		CalModel   *model;
		CalSubmesh *sub;
		size_t      order;

		explicit ModelBox(Scene &scene) : order(0)
		{
			model = new CalModel(scene.core);
			model->attachMesh(0);
			sub = model->getVectorMesh()[0]->getSubmesh(0);
		}
		~ModelBox() { delete model; }
	};

	size_t countEventsFor(const Recorder &rec, CalModel *model, int kind)
	{
		size_t n = 0;
		for (size_t i = 0; i < rec.events.size(); ++i)
		{
			if (rec.events[i].model == model && rec.events[i].kind == kind) ++n;
		}
		return n;
	}
}

// =========================================================================
// committed_pose
// =========================================================================

// Callbacks observe the already-committed skeleton and CPU skin, and those
// values equal an independently computed blended pose and skin.
static int committed_pose_callback_sees_committed_pose()
{
	Scene scene;
	ModelBox box(scene);
	CalModel *model = box.model;
	CalSubmesh *sub = box.sub;

	TASK_CHECK(model->setFrameCallbacksEnabled(true));
	TASK_CHECK(model->getFrameCallbacksEnabled());

	Recorder rec(7, sub, &box.order);
	scene.anim[0]->registerCallback(&rec, 0.0f);

	// Two constant-weight actions on both bones (normalized 50/50 blend).
	model->getMixer()->executeAction(1, 0.0f, 0.0f, 1.0f, true);
	model->getMixer()->executeAction(0, 0.0f, 0.0f, 1.0f, true);

	const float dt = 0.25f;
	model->update(dt);

	// Both actions were alive at the frame start and sampled at t = dt.
	TASK_CHECK(rec.events.size() == 1);
	TASK_CHECK(rec.events[0].kind == 0);
	TASK_CHECK(floatClose(rec.events[0].animTime, dt));

	// Independent reference: average the two local poses at time dt.
	CalVector r0, c0, r1, c1;
	scene.sampleLocal(0, dt, r0, c0);
	scene.sampleLocal(1, dt, r1, c1);
	CalVector refRoot((r0.x + r1.x) * 0.5f, 0.0f, 0.0f);
	CalVector refChildLocal(0.0f, (c0.y + c1.y) * 0.5f, 0.0f);
	CalVector refChild = refChildLocal;
	refChild += refRoot; // identity rotations

	CalQuaternion identity(0, 0, 0, 1);
	CalVector refSkin0 =
		scene.skinPoint(scene.rest0, 0, refRoot, identity) * 0.6f
		+ scene.skinPoint(scene.rest0, 1, refChild, identity) * 0.4f;
	CalVector refSkin1 =
		scene.skinPoint(scene.rest1, 1, refChild, identity);

	// What the callback saw must match the independent reference.
	TASK_CHECK(vecClose(rec.events[0].root, refRoot));
	TASK_CHECK(vecClose(rec.events[0].child, refChild));
	TASK_CHECK(vecClose(rec.events[0].skin0, refSkin0));
	TASK_CHECK(vecClose(rec.events[0].skin1, refSkin1));

	// And the post-update engine state must match both.
	CalVector engRoot = model->getSkeleton()->getBone(0)->getTranslationAbsolute();
	CalVector engChild = model->getSkeleton()->getBone(1)->getTranslationAbsolute();
	CalVector engSkin0 = model->getPhysique()->calculateVertex(sub, 0);
	CalVector engSkin1 = model->getPhysique()->calculateVertex(sub, 1);
	TASK_CHECK(vecClose(engRoot, refRoot));
	TASK_CHECK(vecClose(engChild, refChild));
	TASK_CHECK(vecClose(engSkin0, refSkin0));
	TASK_CHECK(vecClose(engSkin1, refSkin1));
	TASK_CHECK(vecClose(rec.events[0].root, engRoot));
	TASK_CHECK(vecClose(rec.events[0].skin1, engSkin1));

	scene.anim[0]->removeCallback(&rec);
	return 0;
}

// With no queued commands, frame mode produces exactly the same numerical
// pose and skin as the legacy synchronous path over several frames.
static int committed_pose_identical_without_commands()
{
	Scene sceneA;
	Scene sceneB;
	ModelBox frameBox(sceneA);
	ModelBox legacyBox(sceneB);

	TASK_CHECK(frameBox.model->setFrameCallbacksEnabled(true));
	TASK_CHECK(!legacyBox.model->getFrameCallbacksEnabled());

	Recorder recA(1, frameBox.sub, &frameBox.order);
	Recorder recB(2, legacyBox.sub, &legacyBox.order);
	sceneA.anim[0]->registerCallback(&recA, 0.0f);
	sceneB.anim[0]->registerCallback(&recB, 0.0f);

	frameBox.model->getMixer()->executeAction(0, 0, 0, 1, true);
	legacyBox.model->getMixer()->executeAction(0, 0, 0, 1, true);

	const float dts[] = { 0.1f, 0.2f, 0.15f, 0.3f };
	for (size_t f = 0; f < sizeof(dts) / sizeof(dts[0]); ++f)
	{
		frameBox.model->update(dts[f]);
		legacyBox.model->update(dts[f]);

		CalVector fa0 = frameBox.model->getSkeleton()->getBone(0)->getTranslationAbsolute();
		CalVector la0 = legacyBox.model->getSkeleton()->getBone(0)->getTranslationAbsolute();
		CalVector fa1 = frameBox.model->getSkeleton()->getBone(1)->getTranslationAbsolute();
		CalVector la1 = legacyBox.model->getSkeleton()->getBone(1)->getTranslationAbsolute();
		CalVector fs = frameBox.model->getPhysique()->calculateVertex(frameBox.sub, 0);
		CalVector ls = legacyBox.model->getPhysique()->calculateVertex(legacyBox.sub, 0);
		TASK_CHECK(vecClose(fa0, la0));
		TASK_CHECK(vecClose(fa1, la1));
		TASK_CHECK(vecClose(fs, ls));
	}

	// Frame mode dispatches exactly one update per frame for the survivor.
	TASK_CHECK(recA.events.size() == 4);

	sceneA.anim[0]->removeCallback(&recA);
	sceneB.anim[0]->removeCallback(&recB);
	return 0;
}

// =========================================================================
// mutation_order
// =========================================================================

// An action stops itself and a not-yet-dispatched action in the same frame.
// The frame snapshot still completes; stops apply next frame and each
// instance completes exactly once.
static int mutation_stop_self_and_undispatched()
{
	Scene scene;
	ModelBox box(scene);
	CalModel *model = box.model;
	TASK_CHECK(model->setFrameCallbacksEnabled(true));

	Recorder recA(0, box.sub, &box.order);
	Recorder recB(1, box.sub, &box.order);
	scene.anim[0]->registerCallback(&recA, 0.0f);
	scene.anim[1]->registerCallback(&recB, 0.0f);

	struct Stopper : CalAnimationCallback
	{
		CalModel     *model;
		int           self, other;
		CallbackFlag  flag;
		bool          nestedNoop;
		Recorder     *recA;

		Stopper(CalModel *m, int s, int o, Recorder *a)
			: model(m), self(s), other(o), nestedNoop(false), recA(a) { }

		virtual void AnimationUpdate(float, CalModel *, void *)
		{
			flag.check(model->getMixer()->stopAction(self));
			flag.check(model->getMixer()->stopAction(other));

			// A nested update on the same model must be a no-op.
			size_t before = recA->events.size();
			model->update(0.1f);
			nestedNoop = (recA->events.size() == before);
		}
		virtual void AnimationComplete(CalModel *, void *) { }
	} stopper(model, 0, 1, &recA);
	scene.anim[0]->registerCallback(&stopper, 0.0f);

	// Execute B then A so the action list dispatches A before B.
	model->getMixer()->executeAction(1, 0, 0, 1, true);
	model->getMixer()->executeAction(0, 0, 0, 1, true);

	model->update(0.2f);

	TASK_CHECK(stopper.flag.ok);
	TASK_CHECK(stopper.nestedNoop);
	TASK_CHECK(countEventsFor(recA, model, 0) == 1);
	TASK_CHECK(countEventsFor(recB, model, 0) == 1);
	// A (including its stopper) runs before B in dispatch order.
	TASK_CHECK(recA.events[0].ordinal < recB.events[0].ordinal);

	// Frame 2: queued stops take effect; each instance completes once and
	// receives no further update.
	model->update(0.2f);

	TASK_CHECK(countEventsFor(recA, model, 0) == 1);
	TASK_CHECK(countEventsFor(recB, model, 0) == 1);
	TASK_CHECK(countEventsFor(recA, model, 1) == 1);
	TASK_CHECK(countEventsFor(recB, model, 1) == 1);
	TASK_CHECK(model->getMixer()->getCurrentOneShotActions().empty());

	scene.anim[0]->removeCallback(&recA);
	scene.anim[0]->removeCallback(&stopper);
	scene.anim[1]->removeCallback(&recB);
	return 0;
}

// stop then execute of the same id inside a callback yields a brand new
// instance: the old completion never reaches the new instance.
static int mutation_restart_same_id()
{
	Scene scene;
	ModelBox box(scene);
	CalModel *model = box.model;
	TASK_CHECK(model->setFrameCallbacksEnabled(true));

	Recorder rec(3, box.sub, &box.order);
	scene.anim[0]->registerCallback(&rec, 0.0f);

	struct Restarter : CalAnimationCallback
	{
		CalModel     *model;
		CallbackFlag  flag;
		explicit Restarter(CalModel *m) : model(m) { }
		virtual void AnimationUpdate(float, CalModel *, void *)
		{
			flag.check(model->getMixer()->stopAction(0));
			flag.check(model->getMixer()->executeAction(0, 0.0f, 0.0f, 1.0f, true));
		}
		virtual void AnimationComplete(CalModel *, void *) { }
	} restarter(model);
	scene.anim[0]->registerCallback(&restarter, 0.0f);

	model->getMixer()->executeAction(0, 0, 0, 1, true);
	CalAnimation *oldInstance = model->getMixer()->getCurrentOneShotActions().front();

	model->update(0.3f);
	TASK_CHECK(restarter.flag.ok);
	TASK_CHECK(rec.events.size() == 1);
	TASK_CHECK(rec.events[0].kind == 0);
	TASK_CHECK(floatClose(rec.events[0].animTime, 0.3f));

	// Next frame: old instance completion first, then a fresh instance update
	// starting at local time zero.
	model->update(0.1f);

	CalAnimation *newInstance = model->getMixer()->getCurrentOneShotActions().front();
	TASK_CHECK(newInstance != oldInstance); // genuinely new instance
	TASK_CHECK(rec.events.size() == 3);
	TASK_CHECK(rec.events[1].kind == 1);                  // old completion
	TASK_CHECK(rec.events[2].kind == 0);                  // new instance update
	// The new instance was created at frame start and advanced by this frame.
	TASK_CHECK(floatClose(rec.events[2].animTime, 0.1f));
	size_t completes = 0;
	for (size_t i = 0; i < rec.events.size(); ++i)
		if (rec.events[i].kind == 1) ++completes;
	TASK_CHECK(completes == 1); // completion never reached the new instance

	scene.anim[0]->removeCallback(&rec);
	scene.anim[0]->removeCallback(&restarter);
	return 0;
}

// A finite action that reaches its end naturally gets exactly one
// AnimationComplete on its ending frame and no AnimationUpdate that frame.
static int mutation_natural_end_single_complete()
{
	Scene scene;
	ModelBox box(scene);
	CalModel *model = box.model;
	TASK_CHECK(model->setFrameCallbacksEnabled(true));

	Recorder rec(2, box.sub, &box.order);
	scene.anim[0]->registerCallback(&rec, 0.0f); // duration 2s

	model->getMixer()->executeAction(0, 0.0f, 0.0f, 1.0f, false);

	model->update(1.0f); // survives: one update sampled at t = 1
	TASK_CHECK(rec.events.size() == 1);
	TASK_CHECK(rec.events[0].kind == 0);
	TASK_CHECK(floatClose(rec.events[0].animTime, 1.0f));

	model->update(1.0f); // reaches the end at t = 2: completion only
	TASK_CHECK(rec.events.size() == 2);
	TASK_CHECK(rec.events[1].kind == 1);
	TASK_CHECK(model->getMixer()->getCurrentOneShotActions().empty());

	// Further frames produce no more events for the dead instance.
	model->update(0.5f);
	TASK_CHECK(rec.events.size() == 2);

	scene.anim[0]->removeCallback(&rec);
	return 0;
}

// =========================================================================
// registry_clock
// =========================================================================

// min_interval gating on a looping cycle: the first advance always fires,
// later frames fire only when the accumulated playback time reaches the
// interval; one frame spanning multiple loops still fires exactly once and
// no accumulated time is lost.
static int registry_interval_and_loop_wrap()
{
	Scene scene;
	ModelBox box(scene);
	CalModel *model = box.model;
	TASK_CHECK(model->setFrameCallbacksEnabled(true));

	Recorder rec(4, box.sub, &box.order);
	const float interval = 0.3f;
	scene.anim[0]->registerCallback(&rec, interval); // duration 2s cycle

	model->getMixer()->blendCycle(0, 1.0f, 0.0f);

	// Independent expectation maintained with the documented clock rule.
	double clock = 0.0;
	bool fired = false;
	double lastDispatch = 0.0;
	size_t expected = 0;

	const float dts[] = { 0.0f, 0.2f, 0.2f, 0.2f, 0.2f };
	for (size_t f = 0; f < sizeof(dts) / sizeof(dts[0]); ++f)
	{
		size_t before = rec.events.size();
		model->update(dts[f]);
		clock += (double)dts[f];
		bool due = !fired || (clock - lastDispatch + 1e-9 >= (double)interval);
		if (due) { fired = true; lastDispatch = clock; ++expected; }
		TASK_CHECK(rec.events.size() - before == (due ? 1u : 0u));
	}
	TASK_CHECK(rec.events.size() == expected);
	TASK_CHECK(rec.events.size() >= 1); // dt = 0 first frame still dispatched

	// One frame spanning multiple loops (duration 2s, advance 5.5s): exactly
	// one dispatch and the monotonic clock keeps the leftover time.
	size_t beforeBig = rec.events.size();
	model->update(5.5f);
	clock += 5.5;
	TASK_CHECK(rec.events.size() - beforeBig == 1u);

	// The reported time is the wrapped local cycle time; the interval clock
	// continues on the unwrapped, lossless playback seconds.
	float lastAnim = rec.events.back().animTime;
	TASK_CHECK(lastAnim >= 0.0f && lastAnim < 2.0f);

	size_t beforeSmall = rec.events.size();
	model->update(0.05f);
	TASK_CHECK(rec.events.size() == beforeSmall);
	model->update(0.3f);
	TASK_CHECK(rec.events.size() == beforeSmall + 1);

	scene.anim[0]->removeCallback(&rec);
	return 0;
}

// Removing a registration and re-adding the same callback gives it a fresh
// identity and clock: it fires immediately regardless of the old accumulated
// time, and the new interval is honored.
static int registry_remove_readd_fresh_identity()
{
	Scene scene;
	ModelBox box(scene);
	CalModel *model = box.model;
	TASK_CHECK(model->setFrameCallbacksEnabled(true));

	Recorder rec(5, box.sub, &box.order);
	scene.anim[0]->registerCallback(&rec, 0.3f);
	model->getMixer()->blendCycle(0, 1.0f, 0.0f);

	model->update(0.4f); // first dispatch
	TASK_CHECK(rec.events.size() == 1);
	model->update(0.4f); // interval reached again
	TASK_CHECK(rec.events.size() == 2);

	// Remove then re-add with a large interval. The fresh clock fires on its
	// first frame even though the old clock had accumulated 0.8s.
	scene.anim[0]->removeCallback(&rec);
	scene.anim[0]->registerCallback(&rec, 100.0f);

	model->update(0.0f);
	TASK_CHECK(rec.events.size() == 3);

	size_t before = rec.events.size();
	model->update(0.1f);
	model->update(0.1f);
	TASK_CHECK(rec.events.size() == before); // new large interval suppresses

	scene.anim[0]->removeCallback(&rec);
	return 0;
}

// Registry add/remove performed during dispatch only take effect on the next
// frame; the frame's fixed snapshot still runs to completion.
static int registry_dispatch_mutations_apply_next_frame()
{
	Scene scene;
	ModelBox box(scene);
	CalModel *model = box.model;
	TASK_CHECK(model->setFrameCallbacksEnabled(true));

	Recorder rec(8, box.sub, &box.order);
	Recorder lateRec(9, box.sub, &box.order);
	scene.anim[0]->registerCallback(&rec, 0.0f);

	struct SelfRemover : CalAnimationCallback
	{
		CalCoreAnimation *core;
		CalAnimationCallback *late;
		int                  fired;
		SelfRemover(CalCoreAnimation *c, CalAnimationCallback *l)
			: core(c), late(l), fired(0) { }
		virtual void AnimationUpdate(float, CalModel *, void *)
		{
			++fired;
			// Mutate the registry while this frame is being dispatched.
			core->removeCallback(this);
			core->registerCallback(late, 0.0f);
		}
		virtual void AnimationComplete(CalModel *, void *) { }
	} remover(scene.anim[0], &lateRec);
	scene.anim[0]->registerCallback(&remover, 0.0f);

	model->getMixer()->blendCycle(0, 1.0f, 0.0f);

	// Frame 1: the fixed snapshot contains rec and remover; the late
	// registration must not fire yet and the removed callback still does.
	model->update(0.1f);
	TASK_CHECK(rec.events.size() == 1);
	TASK_CHECK(remover.fired == 1);
	TASK_CHECK(lateRec.events.size() == 0);

	// Frame 2: remover is gone, the late registration now participates.
	model->update(0.1f);
	TASK_CHECK(rec.events.size() == 2);
	TASK_CHECK(remover.fired == 1); // never called again
	TASK_CHECK(lateRec.events.size() == 1);

	// Frame 3: both surviving registrations keep firing once per frame.
	model->update(0.1f);
	TASK_CHECK(rec.events.size() == 3);
	TASK_CHECK(lateRec.events.size() == 2);

	scene.anim[0]->removeCallback(&rec);
	scene.anim[0]->removeCallback(&lateRec);
	return 0;
}

// =========================================================================
// isolation
// =========================================================================

// Two models sharing one core animation keep independent interval clocks.
static int isolation_two_models_independent_clocks()
{
	Scene scene;
	ModelBox boxA(scene);
	ModelBox boxB(scene);
	CalModel *modelA = boxA.model;
	CalModel *modelB = boxB.model;
	TASK_CHECK(modelA->setFrameCallbacksEnabled(true));
	TASK_CHECK(modelB->setFrameCallbacksEnabled(true));

	Recorder rec(9, boxA.sub, &boxA.order);
	scene.anim[0]->registerCallback(&rec, 0.3f);

	modelA->getMixer()->blendCycle(0, 1.0f, 0.0f);
	modelB->getMixer()->blendCycle(0, 1.0f, 0.0f);

	// A advances by two small steps that never reach the interval after the
	// mandatory first dispatch; B advances by two large steps that do.
	modelA->update(0.1f); // first dispatch for A
	modelA->update(0.1f); // 0.1 < 0.3: suppressed
	modelB->update(0.4f); // first dispatch for B
	modelB->update(0.4f); // 0.4 >= 0.3: second dispatch for B

	TASK_CHECK(countEventsFor(rec, modelA, 0) == 1);
	TASK_CHECK(countEventsFor(rec, modelB, 0) == 2);
	TASK_CHECK(countEventsFor(rec, modelA, 1) == 0);
	TASK_CHECK(countEventsFor(rec, modelB, 1) == 0);

	// A stop requested on A does not touch B's active cycle.
	TASK_CHECK(modelA->getMixer()->stopCycle(0));
	modelA->update(0.1f);
	TASK_CHECK(modelA->getMixer()->getCurrentCycleActions().empty());
	TASK_CHECK(!modelB->getMixer()->getCurrentCycleActions().empty());

	scene.anim[0]->removeCallback(&rec);
	return 0;
}

// Updating a different model from inside a callback is allowed, while a
// same-model nested update is ignored; validation rejects bad requests and
// the mode toggle respects its guards.
static int isolation_cross_model_nested_validation_toggle()
{
	Scene scene;
	ModelBox boxA(scene);
	ModelBox boxB(scene);
	CalModel *modelA = boxA.model;
	CalModel *modelB = boxB.model;

	// Toggle guard: cannot enable while an animation is active.
	modelA->getMixer()->executeAction(0, 0, 0, 1, false);
	TASK_CHECK(!modelA->setFrameCallbacksEnabled(true));
	TASK_CHECK(!modelA->getFrameCallbacksEnabled());
	TASK_CHECK(!modelA->setFrameCallbacksEnabled(false)); // already off

	// Drain the finite (non-locked) animation, then enable on a quiet model.
	modelA->update(2.5f);
	TASK_CHECK(modelA->getMixer()->getCurrentOneShotActions().empty());
	TASK_CHECK(modelA->setFrameCallbacksEnabled(true));
	TASK_CHECK(modelB->setFrameCallbacksEnabled(true));

	Recorder recA(6, boxA.sub, &boxA.order);
	scene.anim[0]->registerCallback(&recA, 0.0f);

	// This callback lives on anim 2, which only model B runs, so updating A
	// from it exercises the "other model is fine" path without re-entrancy.
	struct CrossUpdater : CalAnimationCallback
	{
		CalModel     *other;
		int           crossUpdates;
		CallbackFlag  flag;
		explicit CrossUpdater(CalModel *o) : other(o), crossUpdates(0) { }
		virtual void AnimationUpdate(float, CalModel *, void *)
		{
			other->update(0.1f); // different model: allowed
			++crossUpdates;
			flag.check(crossUpdates > 0);
		}
		virtual void AnimationComplete(CalModel *, void *) { }
	} cross(modelA);
	scene.anim[2]->registerCallback(&cross, 0.0f);

	// Same-model nested update must be ignored entirely.
	struct Nested : CalAnimationCallback
	{
		CalModel *model;
		size_t    seen;
		explicit Nested(CalModel *m) : model(m), seen(0) { }
		virtual void AnimationUpdate(float, CalModel *, void *)
		{
			++seen;
			model->update(0.25f); // nested same-model update: no-op
		}
		virtual void AnimationComplete(CalModel *, void *) { }
	} nested(modelA);
	scene.anim[0]->registerCallback(&nested, 0.0f);

	modelA->getMixer()->executeAction(0, 0, 0, 1, true);
	size_t before = recA.events.size();
	modelA->update(0.2f);
	TASK_CHECK(recA.events.size() == before + 1);
	TASK_CHECK(nested.seen == 1);

	// B runs anim 2; its callback drives an update of the other model.
	modelB->getMixer()->executeAction(2, 0, 0, 1, true);
	modelB->update(0.1f);
	TASK_CHECK(cross.flag.ok);
	TASK_CHECK(cross.crossUpdates == 1);

	// Parameter/id validation: bad requests are rejected and never queued.
	CalMixer *mixer = modelA->getMixer();
	TASK_CHECK(!mixer->executeAction(999, 0, 0, 1, true));
	TASK_CHECK(!mixer->stopAction(-7));
	TASK_CHECK(!mixer->blendCycle(0, -1.0f, 0.0f));
	TASK_CHECK(!mixer->blendCycle(0, NAN, 0.0f));
	TASK_CHECK(!mixer->blendCycle(0, 1.0f, INFINITY));
	TASK_CHECK(!mixer->clearCycle(0, -0.5f));

	// A valid request is accepted and only applies at the next frame.
	TASK_CHECK(mixer->blendCycle(1, 1.0f, 0.0f));
	TASK_CHECK(modelA->getMixer()->getCurrentCycleActions().empty());
	modelA->update(0.1f);
	TASK_CHECK(!modelA->getMixer()->getCurrentCycleActions().empty());

	// Cannot disable while animations remain active: the call is refused and
	// the mode stays enabled (the setter returns the resulting mode).
	TASK_CHECK(modelA->getFrameCallbacksEnabled());
	modelA->setFrameCallbacksEnabled(false);
	TASK_CHECK(modelA->getFrameCallbacksEnabled());

	scene.anim[0]->removeCallback(&recA);
	scene.anim[0]->removeCallback(&nested);
	scene.anim[2]->removeCallback(&cross);
	return 0;
}

// =========================================================================
// Registration
// =========================================================================

extern const struct task_case task_cases[] = {
	{ "committed_pose", "callback_sees_committed_pose",
		committed_pose_callback_sees_committed_pose },
	{ "committed_pose", "identical_without_commands",
		committed_pose_identical_without_commands },
	{ "mutation_order", "stop_self_and_undispatched",
		mutation_stop_self_and_undispatched },
	{ "mutation_order", "restart_same_id",
		mutation_restart_same_id },
	{ "mutation_order", "natural_end_single_complete",
		mutation_natural_end_single_complete },
	{ "registry_clock", "interval_and_loop_wrap",
		registry_interval_and_loop_wrap },
	{ "registry_clock", "remove_readd_fresh_identity",
		registry_remove_readd_fresh_identity },
	{ "registry_clock", "dispatch_mutations_apply_next_frame",
		registry_dispatch_mutations_apply_next_frame },
	{ "isolation", "two_models_independent_clocks",
		isolation_two_models_independent_clocks },
	{ "isolation", "cross_model_nested_validation_toggle",
		isolation_cross_model_nested_validation_toggle },
};

extern const size_t task_case_count =
	sizeof(task_cases) / sizeof(task_cases[0]);
