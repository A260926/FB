#!/bin/bash
set -euo pipefail
c++ -std=c++17 -Icodec/api/wels -Itask_support task_support/api_consumer.cpp libopenh264.a -pthread -o .task-build/api_consumer
.task-build/api_consumer
test -s examples/scaling_lists.cpp
c++ -std=c++17 -Icodec/api/wels -I. examples/scaling_lists.cpp libopenh264.a -pthread -o .task-build/scaling_lists
rm -f .task-build/scaling-example.264
.task-build/scaling_lists .task-build/scaling-example.264
test -s .task-build/scaling-example.264
c++ -std=c++17 -Icodec/api/wels -Icodec/common/inc -Icodec/encoder/core/inc -Icodec/decoder/core/inc -I. test/encoder/EncUT_ScalingLists_task.cpp task_support/driver.c libopenh264.a -pthread -o .task-build/target
