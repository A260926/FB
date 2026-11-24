#include "centipede/session_feedback.h"
#include "centipede/runner_result.h"
#include <array>
#include <algorithm>
#include <cstdio>
using namespace fuzztest::internal;
int main(){
 std::array<uint8_t,4096> data{};BlobSequence seq(data.data(),data.size());
 SessionModel model{0,{{0,7,1}}};
 SessionEvent a{SessionEventKind::Begin,1,0,10,0,0};
 SessionEvent b{SessionEventKind::Step,1,1,0,1,7};
 SessionEvent c{SessionEventKind::Finish,1,2,0,1,0};
 if(!BatchResult::WriteInputBegin(seq)||!BatchResult::WriteSessionEvent(a,seq)||!BatchResult::WriteSessionEvent(b,seq)||!BatchResult::WriteSessionEvent(c,seq)||!BatchResult::WriteInputEnd(seq))return 20;
 seq.Reset();BatchResult out;out.ClearAndResize(1);
 if(!out.ReadSessionBatch(seq,model)||seq.read_error()||out.num_outputs_read()!=1)return 21;
 const auto& r=out.results()[0];
 if(r.session_events().size()!=3||!(r.session_events()[0]==a))return 22;
 FeatureVec expected{feature_domains::kUserDomains[14].ConvertToMe(7),feature_domains::kUserDomains[15].ConvertToMe(3)};
 std::sort(expected.begin(),expected.end());if(r.features()!=expected)return 23;
 puts("API_CONSUMER_COMPLETE");return 0;
}
