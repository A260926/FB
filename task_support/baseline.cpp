#include "flatbuffers/flatbuffers.h"
#include "proof_generated.h"
#include "legacy_generated.h"
#include <cstring>
#include <iostream>
#include <vector>
int main(){
 flatbuffers::FlatBufferBuilder b;
 auto leaf=Proof::CreateNode(b,9);
 auto root=Proof::CreateNode(b,7,leaf,leaf);
 Proof::FinishNodeBuffer(b,root);
 std::vector<uint8_t> before(b.GetBufferPointer(),b.GetBufferPointer()+b.GetSize());
 flatbuffers::Verifier v(b.GetBufferPointer(),b.GetSize());
 flatbuffers::SizeVerifier sv(b.GetBufferPointer(),b.GetSize());
 if(!Proof::VerifyNodeBuffer(v)||!Proof::VerifyNodeBuffer(sv))return 1;
 auto n=Proof::GetNode(b.GetBufferPointer());
 if(!n->left()||n->left()!=n->right()||n->left()->value()!=9||!sv.GetComputedSize())return 2;
 flatbuffers::FlatBufferBuilder inner;Proof::FinishNodeBuffer(inner,Proof::CreateNode(inner,11));
 flatbuffers::FlatBufferBuilder outer;
 auto bytes=outer.CreateVector(inner.GetBufferPointer(),inner.GetSize());
 Proof::FinishNodeBuffer(outer,Proof::CreateNode(outer,12,0,0,bytes));
 flatbuffers::Verifier nv(outer.GetBufferPointer(),outer.GetSize());
 if(!Proof::VerifyNodeBuffer(nv))return 3;
 flatbuffers::FlatBufferBuilder old;Legacy::FinishItemBuffer(old,Legacy::CreateItem(old,17));
 flatbuffers::Verifier ov(old.GetBufferPointer(),old.GetSize());
 if(!Legacy::VerifyItemBuffer(ov)||Legacy::GetItem(old.GetBufferPointer())->value()!=17)return 4;
 if(std::memcmp(before.data(),b.GetBufferPointer(),before.size()))return 5;
 std::cout<<"BASELINE_FLATBUFFERS shared=1 nested=1 size=1 legacy=1 unchanged=1\n";
}
