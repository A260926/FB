// Fixed normal-use public API consumer; no verifier implementation.
#include "flatbuffers/flatbuffers.h"
#include "proof_generated.h"
#include <iostream>
int main(){
 flatbuffers::FlatBufferBuilder b;
 auto leaf=Proof::CreateNode(b,9);Proof::FinishNodeBuffer(b,Proof::CreateNode(b,7,leaf,leaf));
 flatbuffers::Verifier::Options opts;opts.reuse_tables=true;opts.max_tables=2;
 flatbuffers::Verifier v(b.GetBufferPointer(),b.GetSize(),opts);
 if(!Proof::VerifyNodeBuffer(v))return 71;
 const auto report=v.GetVerificationReport();
 if(report.error!=flatbuffers::VerificationError::none||report.committed_tables!=2||report.cache_hits<1)return 72;
 if(!Proof::VerifyNodeBuffer(v))return 73;
 const auto again=v.GetVerificationReport();
 if(again.committed_tables!=2||again.cache_hits!=report.cache_hits)return 74;
 std::cout<<"PROOF_API_BOUND expanded=2 reused=1 root_reset=1\n";
}
