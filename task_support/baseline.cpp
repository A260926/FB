#include "centipede/runner_result.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>
using namespace fuzztest::internal;
#define REQUIRE(x) do{if(!(x)){std::fprintf(stderr,"baseline failed: %s\n",#x);return 1;}}while(0)
int main(){
 int fds[2];REQUIRE(pipe(fds)==0);pid_t child=fork();REQUIRE(child>=0);
 if(child==0){close(fds[0]);std::array<uint8_t,4096> bytes{};BlobSequence out(bytes.data(),bytes.size());
  feature_t f1[]={7,9},f2[]={11};ExecutionResult::Stats stats;stats.peak_rss_mb=17;
  uint8_t a[]={1,2},b[]={3,4};ExecutionMetadata meta;
  bool ok=meta.AppendCmpEntry({a,2},{b,2}) && BatchResult::WriteInputBegin(out) && BatchResult::WriteOneFeatureVec(f1,2,out) && BatchResult::WriteStats(stats,out) && BatchResult::WriteInputEnd(out) && BatchResult::WriteInputBegin(out) && BatchResult::WriteMetadata(meta,out) && BatchResult::WriteOneFeatureVec(f2,1,out) && BatchResult::WriteInputEnd(out);
  size_t off=0;while(ok && off<bytes.size()){ssize_t n=write(fds[1],bytes.data()+off,bytes.size()-off);if(n<=0){ok=false;break;}off+=n;}close(fds[1]);_exit(ok?0:2);
 }
 close(fds[1]);std::array<uint8_t,4096> bytes{};size_t off=0;
 while(off<bytes.size()){ssize_t n=read(fds[0],bytes.data()+off,bytes.size()-off);REQUIRE(n>0);off+=n;}close(fds[0]);int status=0;REQUIRE(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
 BlobSequence in(bytes.data(),bytes.size());BatchResult batch;batch.ClearAndResize(2);REQUIRE(batch.Read(in));REQUIRE(batch.num_outputs_read()==2);
 REQUIRE(batch.results()[0].features()==FeatureVec({7,9}));REQUIRE(batch.results()[0].stats().peak_rss_mb==17);REQUIRE(batch.results()[1].features()==FeatureVec({11}));
 REQUIRE(batch.results()[1].metadata().cmp_data==ByteArray({2,1,2,3,4}));
 puts("RUNNER_BATCH inputs=2 features=3 metadata_bytes=5 rss=17");puts("CHILD_PRODUCER parent_reader=1 real_blob_sequence=1");puts("BASELINE_COMPLETE");return 0;
}
