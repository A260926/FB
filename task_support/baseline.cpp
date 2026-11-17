#include "aarch64/decoder-aarch64.h"
#include "aarch64/cpu-features-auditor-aarch64.h"
#include "aarch64/disasm-aarch64.h"
#include <iostream>
using namespace vixl;
using namespace vixl::aarch64;
int main() {
  alignas(4) const uint32_t words[]={0xfd400020,0x1e622820,0x8b020020};
  Decoder decoder;
  CPUFeaturesAuditor auditor(&decoder);
  Disassembler dis;
  decoder.AppendVisitor(&dis);
  const Instruction* ins=reinterpret_cast<const Instruction*>(words);
  decoder.Decode(ins);
  if(auditor.GetInstructionFeatures()!=CPUFeatures(CPUFeatures::kFP,CPUFeatures::kNEON)) return 1;
  std::cout<<"SHARED "<<dis.GetOutput()<<" legacy="<<auditor.GetInstructionFeatures()<<'\n';
  auditor.SetAvailableFeatures(CPUFeatures(CPUFeatures::kFP));
  std::cout<<"POST_HINT available_FP="<<auditor.InstructionIsAvailable()<<'\n';
  if(auditor.InstructionIsAvailable()) return 2;
  decoder.Decode(reinterpret_cast<const Instruction*>(words+1));
  if(auditor.GetInstructionFeatures()!=CPUFeatures(CPUFeatures::kFP)) return 3;
  std::cout<<"SCALAR "<<dis.GetOutput()<<" legacy="<<auditor.GetInstructionFeatures()<<'\n';
  decoder.Decode(reinterpret_cast<const Instruction*>(words+2));
  if(auditor.GetInstructionFeatures()!=CPUFeatures::None()) return 4;
  std::cout<<"INTEGER "<<dis.GetOutput()<<" legacy="<<auditor.GetInstructionFeatures()<<'\n';
  auditor.ResetSeenFeatures();
  if(auditor.GetSeenFeatures()!=CPUFeatures::None()) return 5;
  std::cout<<"BASELINE_OK\n";
}
