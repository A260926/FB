#include "threads.h"
#include "syncreplay.h"
#include <string.h>
#include <stdio.h>
#include "model-assert.h"
int user_main(int,char**) {
 if(strcmp(sync_replay_format_version(),"CDS-SYNC-1")!=0){MODEL_ASSERT(false);return 1;}
 puts("SYNC_REPLAY_API_OK");return 0;
}
