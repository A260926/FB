//****************************************************************************//
// framecallbacks.h                                                           //
// Per-CalModel frame transaction for deferred animation callbacks.           //
//                                                                            //
// In frame callback mode the five playback APIs (executeAction, blendCycle,  //
// clearCycle, stopAction, stopCycle) only validate and enqueue commands.     //
// A CalModel::update call first submits the queued commands, then advances   //
// the animations and the whole deformation chain, and only dispatches the    //
// frame's animation events once the committed pose/skin is available.        //
//                                                                            //
// This library is free software; you can redistribute it and/or modify it    //
// under the terms of the GNU Lesser General Public License as published by   //
// the Free Software Foundation; either version 2.1 of the License, or (at    //
// your option) any later version.                                            //
//****************************************************************************//

#ifndef CAL_FRAMECALLBACKS_H
#define CAL_FRAMECALLBACKS_H

namespace cal3d {
	class CalModel;
}

#include "cal3d/global.h"
#include "cal3d/animcallback.h"

#include <deque>
#include <list>
#include <vector>

namespace cal3d {
	class CalMixer;
	class CalAnimation;
	class CalCoreAnimation;

	/// Playback commands that can be requested from inside an animation callback.
	enum FrameCommandType
	{
		FRAME_COMMAND_EXECUTE_ACTION = 0,
		FRAME_COMMAND_BLEND_CYCLE,
		FRAME_COMMAND_CLEAR_CYCLE,
		FRAME_COMMAND_STOP_ACTION,
		FRAME_COMMAND_STOP_CYCLE
	};

	/// A validated playback request waiting for the next frame transaction.
	struct FrameCommand
	{
		FrameCommandType type;
		int              id;
		float            p1;   // delayIn / weight / delay
		float            p2;   // delayOut / delay
		float            p3;   // weightTarget
		bool             flag; // autoLock

		FrameCommand()
			: type(FRAME_COMMAND_EXECUTE_ACTION), id(0)
			, p1(0.0f), p2(0.0f), p3(0.0f), flag(false) { }
	};

	/// Interval clock of one callback registration on one animation instance.
	/// A fresh registration identity always gets a fresh, zero-based clock.
	struct FrameCallbackClock
	{
		CalAnimationCallback *callback;
		float                 minInterval;
		unsigned long long    registryId;     // instance-level clock identity
		unsigned long long    registrationId; // core-level registration identity
		double                lastDispatchTime;
		bool                  hasDispatched;
		bool                  matched; // transient: consumed by this snapshot

		FrameCallbackClock()
			: callback(0), minInterval(0.0f), registryId(0), registrationId(0)
			, lastDispatchTime(0.0), hasDispatched(false), matched(false) { }
	};

	/// Persistent, per-instance timing state. Instances are identified by the
	/// transaction and never by a raw animation pointer.
	struct FrameInstanceNode
	{
		CalAnimation                       *animation;
		unsigned long long                 instanceId;
		double                             playSeconds;
		std::list<FrameCallbackClock>      clocks;

		FrameInstanceNode()
			: animation(0), instanceId(0), playSeconds(0.0) { }
	};

	/// One fixed snapshot entry used for the frame's event dispatch. The clock
	/// pointer stays valid for the frame because nodes own their clocks in a
	/// list and the nodes are retained until the frame completes.
	struct FrameDispatchEntry
	{
		CalAnimationCallback *callback;
		float                 minInterval;
		unsigned long long    registryId;
		FrameCallbackClock   *clock;

		FrameDispatchEntry()
			: callback(0), minInterval(0.0f), registryId(0), clock(0) { }
	};

	/// Snapshot of one animation instance alive at the frame start.
	struct FrameAnimationRecord
	{
		CalAnimation                       *animation;
		CalCoreAnimation                   *coreAnimation;
		int                                 kind; // CalAnimation::Type
		unsigned long long                  instanceId;
		bool                                ended;
		float                               sampleTime;
		float                               advance;
		std::vector<FrameDispatchEntry>     entries;

		FrameAnimationRecord()
			: animation(0), coreAnimation(0), kind(0), instanceId(0)
			, ended(false), sampleTime(0.0f), advance(0.0f) { }
	};

	/// Completion deferred until the next committed pose (stop commands).
	struct FramePendingComplete
	{
		struct Entry
		{
			CalAnimationCallback *callback;
			float                 minInterval;

			Entry() : callback(0), minInterval(0.0f) { }
		};

		std::vector<Entry> entries;

		FramePendingComplete() { }
	};

	/**
	  * Owns the frame transaction state of one CalModel.
	  *
	  * The default mixer (CalMixer) and CalModel cooperate with this object.
	  * Custom mixers, direct manipulation of the mixer animation lists,
	  * exceptions thrown from callbacks and model destruction inside a
	  * callback are not supported.
	  */
	class CAL3D_API CalFrameTransaction
	{
	public:
		explicit CalFrameTransaction(CalModel *pModel);
		~CalFrameTransaction();

		/** Enables/disables frame callback mode. Only allowed when no animation
		  * is active, no command is queued and no model update is running.
		  * @return the resulting mode flag (false when the switch was refused). */
		bool setEnabled(bool enabled);
		bool isEnabled() const { return m_enabled; }

		bool isInUpdate() const { return m_inUpdate; }
		bool isDispatching() const { return m_dispatching; }

		/// Validation + FIFO enqueue used by the five mixer playback APIs.
		bool enqueueCommand(const FrameCommand &command);

		/// Frame start: submit the commands queued by the previous frame.
		void beginFrame(float deltaTime);

		/// Snapshot the active animations/registries and advance playback
		/// without firing any callback.
		void advanceAnimation(CalMixer *pMixer, float deltaTime);

		/// Dispatch the frame events after the pose has been committed.
		void dispatchFrame();

		/// Frame end: release per-frame scratch state.
		void endFrame();

		/// Takes ownership of an animation that ended during the silent mixer
		/// advancement so that its completion can still be dispatched and the
		/// object stays alive until the frame completes.
		void adoptEndedAnimation(CalAnimation *animation);

	private:
		FrameInstanceNode *getNode(CalAnimation *animation);
		void syncRegistry(FrameInstanceNode *node, CalCoreAnimation *core,
			FrameAnimationRecord &record);
		void pruneNode(CalAnimation *animation);

		void flushCommands();
		void executeQueuedCommand(const FrameCommand &command);
		void stashCompletion(CalAnimation *animation);

		bool validateCommand(const FrameCommand &command) const;

		CalModel                         *m_pModel;
		bool                              m_enabled;
		bool                              m_inUpdate;
		bool                              m_dispatching;

		std::deque<FrameCommand>          m_queue;
		std::list<FrameInstanceNode>      m_nodes;
		std::vector<FrameAnimationRecord> m_records;
		std::vector<FramePendingComplete> m_pendingCompletes;
		std::vector<CalAnimation *>       m_retainedEnded;

		unsigned long long                m_nextInstanceId;
		unsigned long long                m_nextRegistryId;

		// NonCopyable
		CalFrameTransaction(const CalFrameTransaction &);
		CalFrameTransaction &operator=(const CalFrameTransaction &);
	};
}

#endif

//****************************************************************************//
