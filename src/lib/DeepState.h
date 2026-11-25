/*
 * Copyright (c) 2019 Trail of Bits, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_INCLUDE_DEEPSTATE_DEEPSTATE_PRIVATE_H_
#define SRC_INCLUDE_DEEPSTATE_DEEPSTATE_PRIVATE_H_

#include <deepstate/DeepState.h>

DEEPSTATE_BEGIN_EXTERN_C


/* Function that allocates the shared memory for the current test run. Platform 
 * specific function. */
extern void DeepState_AllocCurrentTestRun(void);

/* Run saved take over cases. Platform specific function. */
extern void DeepState_RunSavedTakeOverCases(jmp_buf env, struct DeepState_TestInfo *test);

/* Run take over. Platform specific function. */
extern int DeepState_TakeOver(void);

/* Drop any buffered DeepState stream content without logging it.  Used by the
 * isolated replay children so inherited formatting state is not attributed to
 * a child's log.  Implemented in Stream.c. */
extern void DeepState_ResetStreams(void);

/* Index into `DeepState_GeneratedAllocs`; reset per replay child so inherited
 * test allocations are not attributed to the child.  Defined in DeepState.c. */
extern uint32_t DeepState_GeneratedAllocsIndex;

/* Private single-case child execution path used by `DeepState_ReplayBatch`.
 * Must be called in a fresh forked child (or any process that may discard all
 * prior DeepState state).  It allocates private current-run state, resets the
 * input cursor, initializes the input from exactly `data`/`size`, disables
 * random generation and the framework's automatic output_test_dir saving, and
 * runs the test through `DeepState_Begin` plus the usual
 * `DeepState_RunTestNoFork` Pass/Fail/Abandon handling.  On success it stores
 * the actual test result and the actual `DeepState_InputIndex` consumption and
 * returns 0; it returns -1 only if private state could not be allocated. */
extern int DeepState_RunReplayChild(struct DeepState_TestInfo *test,
                                    const uint8_t *data, size_t size,
                                    enum DeepState_TestRunResult *result,
                                    size_t *consumed);


DEEPSTATE_END_EXTERN_C

#endif /* SRC_INCLUDE_DEEPSTATE_DEEPSTATE_PRIVATE_H_ */