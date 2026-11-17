// Copyright 2024, VIXL authors
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of Arm Limited nor the names of its contributors may be
//     used to endorse or promote products derived from this software without
//     specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// Demonstrate exact CPU feature requirements.
//
// Unlike the historical `CPUFeatures` queries, `CPUFeatureRequirements` keeps
// every alternative that would be sufficient for an instruction, so a code
// block can be analysed once and then queried against several hypothetical
// hardware configurations later.

#include <stdint.h>
#include <stdio.h>

#include <sstream>
#include <string>

#include "aarch64/cpu-features-auditor-aarch64.h"
#include "aarch64/decoder-aarch64.h"
#include "aarch64/disasm-aarch64.h"

#ifndef TEST_EXAMPLES

using namespace vixl;
using namespace vixl::aarch64;

namespace {

std::string Describe(const CPUFeatures& features) {
  std::ostringstream stream;
  stream << features;
  return stream.str();
}

void PrintRequirements(const char* label,
                       const CPUFeatureRequirements& requirements) {
  printf("%s:", label);
  for (const CPUFeatures& alternative : requirements.GetAlternatives()) {
    printf(" (%s)", Describe(alternative).c_str());
  }
  printf("\n");
}

}  // namespace

int main() {
  // `ldr d0, [x1]` can be provided by either FP or NEON. `fadd d0, d1, d2`
  // requires FP. Together, the sequence requires FP.
  const uint32_t code[] = {
      0xfd400020,  // ldr d0, [x1]
      0x1e622820,  // fadd d0, d1, d2
  };

  // The auditor is constructed without an `available` hint. The exact
  // requirements do not depend on it in any case.
  CPUFeaturesAuditor auditor;
  Decoder decoder;
  decoder.AppendVisitor(&auditor);
  for (uint32_t word : code) {
    const Instruction* instr = reinterpret_cast<const Instruction*>(&word);
    decoder.Decode(instr);
    PrintRequirements("instruction", auditor.GetInstructionRequirements());
  }
  PrintRequirements("sequence", auditor.GetSeenRequirements());

  // The saved value is independent of the auditor's later state.
  CPUFeatureRequirements saved = auditor.GetSeenRequirements();

  // Query the requirement against several hardware configurations.
  const CPUFeatures configurations[] = {
      CPUFeatures::None(),
      CPUFeatures(CPUFeatures::kFP),
      CPUFeatures(CPUFeatures::kNEON),
      CPUFeatures(CPUFeatures::kFP, CPUFeatures::kNEON),
  };
  for (const CPUFeatures& available : configurations) {
    std::string name = Describe(available);
    if (name.empty()) name = "none";
    printf("available [%s]: satisfied=%d\n",
           name.c_str(),
           saved.IsSatisfiedBy(available));
  }

  // The PrintDisassembler can print the minimal supplement required by an
  // instruction, given the available features.
  Decoder print_decoder;
  CPUFeaturesAuditor print_auditor(&print_decoder);
  PrintDisassembler disasm(stdout);
  disasm.SetExactCPUFeatures(true);
  disasm.RegisterCPUFeaturesAuditor(&print_auditor);
  print_decoder.AppendVisitor(&disasm);
  for (uint32_t word : code) {
    const Instruction* instr = reinterpret_cast<const Instruction*>(&word);
    print_decoder.Decode(instr);
  }
  return 0;
}

#endif  // TEST_EXAMPLES
