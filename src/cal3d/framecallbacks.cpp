//****************************************************************************//
// framecallbacks.cpp                                                         //
// Per-CalModel frame transaction for deferred animation callbacks.           //
//                                                                            //
// This library is free software; you can redistribute it and/or modify it    //
// under the terms of the GNU Lesser General Public License as published by   //
// the Free Software Foundation; either version 2.1 of the License, or (at    //
// your option) any later version.                                            //
//****************************************************************************//

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "cal3d/framecallbacks.h"

#include "cal3d/animation.h"
#include "cal3d/animation_action.h"
#include "cal3d/animation_cycle.h"
#include "cal3d/coreanimation.h"
#include "cal3d/coremodel.h"
#include "cal3d/model.h"
#include "cal3d/mixer.h"

#include <cmath>
#include <cstddef>

using namespace cal3d;

namespace
{
	bool finiteNonNegative(float value)
	{
		return std::isfinite(value) && value >= 0.0f;
	}
}

/*****************************************************************************/
/** Constructs the frame transaction of a model.
  *****************************************************************************/

CalFrameTransaction::CalFrameTransaction(CalModel *pModel)
  : m_pModel(pModel)
  , m_enabled(false)
  , m_inUpdate(false)
  , m_dispatching(false)
  , m_nextInstanceId(1)
  , m_nextRegistryId(1)
{
	assert(pModel);
}

/*****************************************************************************/
/** Destructs the frame transaction.
  *****************************************************************************/

CalFrameTransaction::~CalFrameTransaction()
{
	// Animations retained for an in-flight frame would normally be released
	// in endFrame; release any survivor here as well.
	for (size_t i = 0; i < m_retainedEnded.size(); ++i)
	{
		delete m_retainedEnded[i];
	}
	m_retainedEnded.clear();
}

/*****************************************************************************/
/** Returns the persistent timing node of an animation instance, creating it
  * (with a fresh instance identity) on first use. Nodes live in a list so
  * their address stays stable while entries reference them.
  *****************************************************************************/

FrameInstanceNode *CalFrameTransaction::getNode(CalAnimation *animation)
{
	std::list<FrameInstanceNode>::iterator it;
	for (it = m_nodes.begin(); it != m_nodes.end(); ++it)
	{
		if (it->animation == animation) return &(*it);
	}

	FrameInstanceNode node;
	node.animation = animation;
	node.instanceId = m_nextInstanceId++;
	node.playSeconds = 0.0;
	m_nodes.push_back(node);
	return &m_nodes.back();
}

/*****************************************************************************/
/** Fixes the registry snapshot of one animation instance. Clocks of
  * registrations that disappeared are dropped; a later re-registration of the
  * same callback therefore starts from a fresh, zero-based clock instead of
  * inheriting the old timing. Surviving registrations keep their identity and
  * accumulated interval timing.
  *****************************************************************************/

void CalFrameTransaction::syncRegistry(FrameInstanceNode *node,
	CalCoreAnimation *core, FrameAnimationRecord &record)
{
	// Every clock starts unmatched for this snapshot.
	std::list<FrameCallbackClock>::iterator clockIt;
	for (clockIt = node->clocks.begin(); clockIt != node->clocks.end(); ++clockIt)
	{
		clockIt->matched = false;
	}

	std::vector<CalCoreAnimation::CallbackRecord> &list = core->getCallbackList();
	record.entries.clear();
	record.entries.reserve(list.size());

	for (size_t i = 0; i < list.size(); ++i)
	{
		CalAnimationCallback *callback = list[i].callback;
		float minInterval = list[i].min_interval;
		unsigned long long registrationId = list[i].registration_id;

		// A surviving registration keeps its clock (and its timing); the
		// core-level registration id distinguishes a remove/re-add even when
		// the same callback object is re-registered.
		FrameCallbackClock *clock = 0;
		for (clockIt = node->clocks.begin(); clockIt != node->clocks.end(); ++clockIt)
		{
			if (!clockIt->matched && clockIt->registrationId == registrationId)
			{
				clock = &(*clockIt);
				break;
			}
		}

		if (clock == 0)
		{
			// New registration (or a removed-then-readded one): fresh instance
			// identity and fresh, zero-based timing.
			FrameCallbackClock fresh;
			fresh.callback = callback;
			fresh.minInterval = minInterval;
			fresh.registryId = m_nextRegistryId++;
			fresh.registrationId = registrationId;
			fresh.lastDispatchTime = 0.0;
			fresh.hasDispatched = false;
			fresh.matched = true;
			node->clocks.push_back(fresh);
			clock = &node->clocks.back();
		}
		else
		{
			clock->matched = true;
			clock->callback = callback;
			clock->minInterval = minInterval;
		}

		FrameDispatchEntry entry;
		entry.callback = callback;
		entry.minInterval = minInterval;
		entry.registryId = clock->registryId;
		entry.clock = clock;
		record.entries.push_back(entry);
	}

	// Drop clocks of registrations no longer present.
	std::list<FrameCallbackClock>::iterator nextIt;
	for (clockIt = node->clocks.begin(); clockIt != node->clocks.end(); clockIt = nextIt)
	{
		nextIt = clockIt;
		++nextIt;
		if (!clockIt->matched) node->clocks.erase(clockIt);
	}
}

/*****************************************************************************/
/** Removes the timing node of an animation instance.
  *****************************************************************************/

void CalFrameTransaction::pruneNode(CalAnimation *animation)
{
	std::list<FrameInstanceNode>::iterator it;
	for (it = m_nodes.begin(); it != m_nodes.end(); ++it)
	{
		if (it->animation == animation)
		{
			m_nodes.erase(it);
			return;
		}
	}
}

/*****************************************************************************/
/** Validates a playback request. Invalid ids, negative or non-finite weights
  * and delays are rejected and never queued.
  *****************************************************************************/

bool CalFrameTransaction::validateCommand(const FrameCommand &command) const
{
	CalCoreModel *coreModel = m_pModel->getCoreModel();
	if (coreModel == 0) return false;

	int count = coreModel->getCoreAnimationCount();
	if (command.id < 0 || command.id >= count) return false;
	if (coreModel->getCoreAnimation(command.id) == 0) return false;

	switch (command.type)
	{
	case FRAME_COMMAND_EXECUTE_ACTION:
		return finiteNonNegative(command.p1)   // delayIn
			&& finiteNonNegative(command.p2)   // delayOut
			&& finiteNonNegative(command.p3);  // weightTarget
	case FRAME_COMMAND_BLEND_CYCLE:
		return finiteNonNegative(command.p1)  // weight
			&& finiteNonNegative(command.p2); // delay
	case FRAME_COMMAND_CLEAR_CYCLE:
		return finiteNonNegative(command.p1); // delay
	case FRAME_COMMAND_STOP_ACTION:
	case FRAME_COMMAND_STOP_CYCLE:
		return true;
	}
	return false;
}

/*****************************************************************************/
/** Validates and enqueues a playback request in FIFO order.
  *****************************************************************************/

bool CalFrameTransaction::enqueueCommand(const FrameCommand &command)
{
	if (!m_enabled) return false;
	if (!validateCommand(command)) return false;

	m_queue.push_back(command);
	return true;
}

/*****************************************************************************/
/** Saves the registrations of an animation about to be stopped, so that its
  * completion is dispatched after the next committed pose.
  *****************************************************************************/

void CalFrameTransaction::stashCompletion(CalAnimation *animation)
{
	FramePendingComplete pending;

	CalCoreAnimation *core = animation->getCoreAnimation();
	std::vector<CalCoreAnimation::CallbackRecord> &list = core->getCallbackList();
	pending.entries.reserve(list.size());
	for (size_t i = 0; i < list.size(); ++i)
	{
		FramePendingComplete::Entry entry;
		entry.callback = list[i].callback;
		entry.minInterval = list[i].min_interval;
		pending.entries.push_back(entry);
	}

	m_pendingCompletes.push_back(pending);
}

/*****************************************************************************/
/** Takes ownership of an animation removed from the mixer lists during this
  * frame, keeping it alive until its events have been dispatched.
  *****************************************************************************/

void CalFrameTransaction::adoptEndedAnimation(CalAnimation *animation)
{
	if (animation == 0) return;
	m_retainedEnded.push_back(animation);
}

/*****************************************************************************/
/** Runs one queued playback command against the mixer. Commands target the
  * active animation through the core animation id, never through a stored raw
  * pointer.
  *****************************************************************************/

void CalFrameTransaction::executeQueuedCommand(const FrameCommand &command)
{
	CalMixer *mixer = m_pModel->getMixer();
	if (mixer == 0) return;

	CalCoreModel *coreModel = m_pModel->getCoreModel();
	CalCoreAnimation *core = coreModel->getCoreAnimation(command.id);
	if (core == 0) return;

	mixer->m_submittingCommands = true;

	switch (command.type)
	{
	case FRAME_COMMAND_EXECUTE_ACTION:
		mixer->executeAction(command.id, command.p1, command.p2,
			command.p3, command.flag);
		break;
	case FRAME_COMMAND_BLEND_CYCLE:
		mixer->blendCycle(command.id, command.p1, command.p2);
		break;
	case FRAME_COMMAND_CLEAR_CYCLE:
		mixer->clearCycle(command.id, command.p1);
		break;
	case FRAME_COMMAND_STOP_ACTION:
	{
		std::list<CalAnimationAction *> actions = mixer->getCurrentOneShotActions();
		std::list<CalAnimationAction *>::iterator it;
		CalAnimationAction *target = 0;
		for (it = actions.begin(); it != actions.end(); ++it)
		{
			if ((*it)->getCoreAnimation() == core) { target = *it; break; }
		}
		if (target != 0)
		{
			// Completion is deferred to the dispatch after the next pose.
			stashCompletion(target);
			mixer->stopAction(command.id);
			// The instance is gone: drop its timing node and keep the object
			// alive only until the deferred completion has been dispatched.
			pruneNode(target);
			adoptEndedAnimation(target);
		}
		break;
	}
	case FRAME_COMMAND_STOP_CYCLE:
	{
		std::list<CalAnimationCycle *> cycles = mixer->getCurrentCycleActions();
		std::list<CalAnimationCycle *>::iterator it;
		CalAnimationCycle *target = 0;
		for (it = cycles.begin(); it != cycles.end(); ++it)
		{
			if ((*it)->getCoreAnimation() == core) { target = *it; break; }
		}
		if (target != 0)
		{
			stashCompletion(target);
			mixer->stopCycle(command.id);
			// Detach the removed cycle from the mixer's id table.
			if (command.id >= 0
				&& command.id < (int)mixer->m_vectorAnimation.size()
				&& mixer->m_vectorAnimation[command.id] == target)
			{
				mixer->m_vectorAnimation[command.id] = 0;
			}
			pruneNode(target);
			adoptEndedAnimation(target);
		}
		break;
	}
	}

	mixer->m_submittingCommands = false;
}

/*****************************************************************************/
/** Submits the commands requested by the previous frame, in FIFO order.
  *****************************************************************************/

void CalFrameTransaction::flushCommands()
{
	while (!m_queue.empty())
	{
		FrameCommand command = m_queue.front();
		m_queue.pop_front();
		executeQueuedCommand(command);
	}
}

/*****************************************************************************/
/** Toggles frame callback mode. The switch is refused while animations are
  * active, commands are queued or an update is running.
  *****************************************************************************/

bool CalFrameTransaction::setEnabled(bool enabled)
{
	if (enabled == m_enabled) return m_enabled;
	if (m_inUpdate) return m_enabled;

	// Frame mode is only defined for the default CalMixer. A custom mixer is
	// an unsupported configuration for this transaction (getMixer() returns
	// null in that case).
	CalMixer *mixer = m_pModel->getMixer();
	if (mixer == 0) return m_enabled;

	if (enabled)
	{
		if (mixer->hasActiveAnimations()) return m_enabled;
		if (!m_queue.empty()) return m_enabled;

		m_enabled = true;
	}
	else
	{
		if (!m_queue.empty() || !m_pendingCompletes.empty()) return m_enabled;
		if (mixer->hasActiveAnimations()) return m_enabled;

		m_enabled = false;
		m_nodes.clear();
		m_records.clear();
	}

	return m_enabled;
}

/*****************************************************************************/
/** Frame start: open the transaction and submit last frame's commands.
  *****************************************************************************/

void CalFrameTransaction::beginFrame(float /*deltaTime*/)
{
	m_inUpdate = true;
	m_dispatching = false;
	flushCommands();
}

/*****************************************************************************/
/** Snapshots the active animations and registries, then advances playback
  * through the regular mixer code (which stays silent in frame mode) and
  * derives the per-instance timing used for interval gating.
  *****************************************************************************/

void CalFrameTransaction::advanceAnimation(CalMixer *mixer, float deltaTime)
{
	m_records.clear();

	const float globalDurationPre = mixer->getAnimationDuration();
	const float globalTimeFactor = mixer->getTimeFactor();

	// Snapshot actions first, then cycles, each in frame-start list order.
	std::list<CalAnimationAction *> actions = mixer->getCurrentOneShotActions();
	std::list<CalAnimationAction *>::iterator itAction;
	for (itAction = actions.begin(); itAction != actions.end(); ++itAction)
	{
		CalAnimation *animation = *itAction;
		FrameInstanceNode *node = getNode(animation);

		FrameAnimationRecord record;
		record.animation = animation;
		record.coreAnimation = animation->getCoreAnimation();
		record.kind = CalAnimation::TYPE_ACTION;
		record.instanceId = node->instanceId;
		record.sampleTime = animation->getTime();
		syncRegistry(node, record.coreAnimation, record);
		m_records.push_back(record);
	}

	std::list<CalAnimationCycle *> cycles = mixer->getCurrentCycleActions();
	std::list<CalAnimationCycle *>::iterator itCycle;
	for (itCycle = cycles.begin(); itCycle != cycles.end(); ++itCycle)
	{
		CalAnimation *animation = *itCycle;
		FrameInstanceNode *node = getNode(animation);

		FrameAnimationRecord record;
		record.animation = animation;
		record.coreAnimation = animation->getCoreAnimation();
		record.kind = CalAnimation::TYPE_CYCLE;
		record.instanceId = node->instanceId;
		record.sampleTime = animation->getTime();
		syncRegistry(node, record.coreAnimation, record);
		m_records.push_back(record);
	}

	// Regular playback advancement; no callback fires while frame mode is on.
	// Animations that end are adopted by this transaction instead of deleted.
	mixer->updateAnimation(deltaTime);

	// Post-process the snapshot: distinguish surviving and ended instances and
	// accumulate the nonnegative playback seconds of every surviving instance.
	for (size_t i = 0; i < m_records.size(); ++i)
	{
		FrameAnimationRecord &record = m_records[i];

		bool retained = false;
		for (size_t k = 0; k < m_retainedEnded.size(); ++k)
		{
			if (m_retainedEnded[k] == record.animation) { retained = true; break; }
		}
		if (retained)
		{
			// The instance ended during the silent advancement. It gets a
			// completion only, and its timing node is dropped so that a later
			// instance of the same core animation starts with a fresh clock.
			record.ended = true;
			pruneNode(record.animation);
			continue;
		}

		FrameInstanceNode *node = getNode(record.animation);

		if (record.kind == CalAnimation::TYPE_ACTION)
		{
			float nowTime = record.animation->getTime();
			float advance = nowTime - record.sampleTime;
			if (advance < 0.0f) advance = 0.0f;

			record.advance = advance;
			record.sampleTime = nowTime;
			node->playSeconds += advance;
		}
		else
		{
			CalAnimationCycle *cycle = (CalAnimationCycle *)record.animation;
			float duration = record.coreAnimation->getDuration();
			float advance = 0.0f;
			float localTime = 0.0f;

			if (cycle->getState() == CalAnimation::STATE_ASYNC)
			{
				// Async cycles own their time and time factor.
				advance = deltaTime * cycle->getTimeFactor();
				localTime = cycle->getTime();
			}
			else if (globalDurationPre > 0.0f && duration > 0.0f)
			{
				// Synced cycles: reproduce exactly the local time used by
				// CalMixer::updateSkeleton, which reads the post-update global
				// animation time/duration.
				float postDuration = mixer->getAnimationDuration();
				float postTime = mixer->getAnimationTime();
				if (postDuration > 0.0f)
					localTime = postTime * duration / postDuration;

				// Interval clock accumulates this instance's mapped playback
				// seconds for the frame; it never loses time across loops.
				float mapped = duration / globalDurationPre;
				advance = deltaTime * globalTimeFactor * mapped;
			}

			if (advance < 0.0f) advance = 0.0f;
			record.advance = advance;
			record.sampleTime = localTime;
			node->playSeconds += advance;
		}
	}
}

/*****************************************************************************/
/** Dispatches the frame's events. Stopped animations complete first (their
  * pose was committed this frame); then the snapshot runs actions first and
  * cycles second. Surviving instances get at most one Update per registration,
  * ended instances get exactly one Complete and no Update.
  *****************************************************************************/

void CalFrameTransaction::dispatchFrame()
{
	m_dispatching = true;
	void *userData = m_pModel->getUserData();

	// Completions caused by stop commands of the previous frame.
	for (size_t p = 0; p < m_pendingCompletes.size(); ++p)
	{
		FramePendingComplete &pending = m_pendingCompletes[p];
		for (size_t i = 0; i < pending.entries.size(); ++i)
		{
			pending.entries[i].callback->AnimationComplete(m_pModel, userData);
		}
	}
	m_pendingCompletes.clear();

	for (size_t r = 0; r < m_records.size(); ++r)
	{
		FrameAnimationRecord &record = m_records[r];

		if (record.ended)
		{
			for (size_t i = 0; i < record.entries.size(); ++i)
			{
				record.entries[i].callback->AnimationComplete(m_pModel, userData);
			}
			continue;
		}

		FrameInstanceNode *node = getNode(record.animation);
		for (size_t i = 0; i < record.entries.size(); ++i)
		{
			FrameCallbackClock *clock = record.entries[i].clock;
			if (clock == 0) continue;

			bool due = !clock->hasDispatched
				|| (node->playSeconds - clock->lastDispatchTime + 1e-9
					>= (double)clock->minInterval);
			if (!due) continue;

			clock->callback->AnimationUpdate(record.sampleTime, m_pModel, userData);
			clock->hasDispatched = true;
			clock->lastDispatchTime = node->playSeconds;
		}
	}

	m_dispatching = false;
}

/*****************************************************************************/
/** Frame end: release per-frame scratch state and the animations removed
  * during the frame. Commands queued during the dispatch are kept for the
  * next frame.
  *****************************************************************************/

void CalFrameTransaction::endFrame()
{
	for (size_t i = 0; i < m_retainedEnded.size(); ++i)
	{
		pruneNode(m_retainedEnded[i]);
		delete m_retainedEnded[i];
	}
	m_retainedEnded.clear();

	m_records.clear();
	m_dispatching = false;
	m_inUpdate = false;
}

//****************************************************************************//
