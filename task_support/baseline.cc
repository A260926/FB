#include "threads.h"
#include <mutex>
#include <stdio.h>
#include "model-assert.h"
int user_main(int,char**) {
 std::mutex m;m.lock();m.unlock();
 bool acquired=m.try_lock();MODEL_ASSERT(acquired);if(acquired)m.unlock();
 puts("CDS_NORMAL_MUTEX_OK");return 0;
}
