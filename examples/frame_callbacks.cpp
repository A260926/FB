//****************************************************************************//
// frame_callbacks.cpp (example)                                              //
// Windowless demonstration of switching an action from inside an animation   //
// callback while frame callback mode is enabled. The demo independently      //
// checks the committed pose and the dispatch order and prints                //
// FRAME_CALLBACKS_DEMO_OK on success.                                        //
//                                                                            //
// This library is free software; you can redistribute it and/or modify it    //
// under the terms of the GNU Lesser General Public License as published by   //
// the Free Software Foundation; either version 2.1 of the License, or (at    //
// your option) any later version.                                            //
//****************************************************************************//

#include "cal3d/cal3d.h"
#include "cal3d/coretrack.h"
#include "cal3d/corekeyframe.h"
#include "cal3d/framecallbacks.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace cal3d;

namespace
{
	bool close(float a, float b) { return std::fabs(a - b) < 1e-4f; }

	struct Event
	{
		int       kind; // 0 update, 1 complete
		int       anim;
		float     time;
		float     rootX;
	};

	// Recorder bound to a specific animation id for event attribution.
	struct TaggedRecorder : CalAnimationCallback
	{
		int                 id;
		std::vector<Event> *events;
		TaggedRecorder(int i, std::vector<Event> *e) : id(i), events(e) { }
		virtual void AnimationUpdate(float t, CalModel *m, void *)
		{
			Event e; e.kind = 0; e.anim = id; e.time = t;
			e.rootX = m->getSkeleton()->getBone(0)->getTranslationAbsolute().x;
			events->push_back(e);
		}
		virtual void AnimationComplete(CalModel *m, void *)
		{
			Event e; e.kind = 1; e.anim = id; e.time = -1.0f;
			e.rootX = m->getSkeleton()->getBone(0)->getTranslationAbsolute().x;
			events->push_back(e);
		}
	};

	// On its first update, switches from action A to action B using only the
	// deferred playback APIs.
	struct Switcher : CalAnimationCallback
	{
		CalModel *model;
		bool      fired;
		explicit Switcher(CalModel *m) : model(m), fired(false) { }
		virtual void AnimationUpdate(float, CalModel *, void *)
		{
			if (fired) return;
			fired = true;
			// Both calls only validate and enqueue; they take effect next frame.
			if (!model->getMixer()->stopAction(0)) std::exit(2);
			if (!model->getMixer()->executeAction(1, 0.0f, 0.0f, 1.0f, true))
				std::exit(2);
		}
		virtual void AnimationComplete(CalModel *, void *) { }
	};

	CalCoreAnimation *makeAnim(float x0, float x1)
	{
		CalCoreAnimation *a = new CalCoreAnimation;
		a->setDuration(1.0f);
		CalCoreTrack *tr = new CalCoreTrack;
		tr->setCoreBoneId(0);
		tr->setTranslationRequired(true);
		CalCoreKeyframe *k0 = new CalCoreKeyframe;
		k0->setTime(0.0f);
		k0->setTranslation(CalVector(x0, 0, 0));
		k0->setRotation(CalQuaternion(0, 0, 0, 1));
		tr->addCoreKeyframe(k0);
		CalCoreKeyframe *k1 = new CalCoreKeyframe;
		k1->setTime(1.0f);
		k1->setTranslation(CalVector(x1, 0, 0));
		k1->setRotation(CalQuaternion(0, 0, 0, 1));
		tr->addCoreKeyframe(k1);
		a->addCoreTrack(tr);
		return a;
	}

	int fail(const char *msg)
	{
		std::fprintf(stderr, "FRAME_CALLBACKS_DEMO_FAIL %s\n", msg);
		return 1;
	}
}

int main()
{
	CalCoreModel core("demo");
	CalCoreSkeleton *skel = new CalCoreSkeleton;
	CalCoreBone *root = new CalCoreBone("root");
	root->setParentId(-1);
	root->setTranslation(CalVector(1.0f, 0.0f, 0.0f));
	root->setRotation(CalQuaternion(0, 0, 0, 1));
	// Non-zero inverse bind.
	root->setTranslationBoneSpace(CalVector(-1.0f, 0.0f, 0.0f));
	root->setRotationBoneSpace(CalQuaternion(0, 0, 0, 1));
	root->setCoreSkeleton(skel);
	skel->addCoreBone(root);
	skel->calculateState();
	core.setCoreSkeleton(skel);

	CalCoreAnimation *animA = makeAnim(0.0f, 2.0f);   // id 0
	CalCoreAnimation *animB = makeAnim(10.0f, 12.0f); // id 1
	core.addCoreAnimation(animA);
	core.addCoreAnimation(animB);

	CalModel model(&core);
	if (!model.setFrameCallbacksEnabled(true)) return fail("enable");

	std::vector<Event> events;
	TaggedRecorder recA(0, &events);
	TaggedRecorder recB(1, &events);
	Switcher switcher(&model);
	animA->registerCallback(&recA, 0.0f);
	animA->registerCallback(&switcher, 0.0f);
	animB->registerCallback(&recB, 0.0f);

	if (!model.getMixer()->executeAction(0, 0.0f, 0.0f, 1.0f, true))
		return fail("execute A");

	// Frame 1: A updates at t = 0.25 and queues the switch.
	model.update(0.25f);
	if (events.size() != 1) return fail("frame1 event count");
	if (events[0].kind != 0 || events[0].anim != 0) return fail("frame1 kind");
	if (!close(events[0].time, 0.25f)) return fail("frame1 time");
	// Independent A pose at t=.25: x interpolates 0 -> 2 => 0.5 (root == local).
	if (!close(events[0].rootX, 0.5f)) return fail("frame1 pose");

	// Frame 2: the queued stop(A) + execute(B) are applied first. The deferred
	// completion of A is dispatched after the new pose, then B's first update.
	model.update(0.25f);

	if (events.size() != 3) return fail("frame2 event count");
	if (events[1].kind != 1 || events[1].anim != 0) return fail("A complete order");
	if (events[2].kind != 0 || events[2].anim != 1) return fail("B update order");
	if (!close(events[2].time, 0.25f)) return fail("B local time");
	// Independent B pose at t=.25: x interpolates 10 -> 12 => 10.5.
	if (!close(events[2].rootX, 10.5f)) return fail("B pose");

	// The committed pose observed in the callback equals the post-update pose.
	float engineRoot = model.getSkeleton()->getBone(0)->getTranslationAbsolute().x;
	if (!close(engineRoot, events[2].rootX)) return fail("committed mismatch");

	// A is gone and B is the sole active action; A completed only once.
	if (model.getMixer()->getCurrentOneShotActions().size() != 1)
		return fail("active set size");
	int completesA = 0;
	for (size_t i = 0; i < events.size(); ++i)
		if (events[i].kind == 1 && events[i].anim == 0) ++completesA;
	if (completesA != 1) return fail("A completion count");

	animA->removeCallback(&recA);
	animA->removeCallback(&switcher);
	animB->removeCallback(&recB);

	std::printf("FRAME_CALLBACKS_DEMO_OK\n");
	return 0;
}
