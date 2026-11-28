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
//   * Neither the name of ARM Limited nor the names of its contributors may be
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

#include "examples.h"

#include "aarch64/cpu-features-auditor-aarch64.h"
#include "aarch64/decoder-aarch64.h"

using namespace vixl;
using namespace vixl::aarch64;

// Demonstrate CPUFeatureRequirements: an exact description of the features an
// instruction (or a block of instructions) needs, including encodings that can
// be provided by more than one feature.
//
// This example only decodes instruction words. It never executes them.
#ifndef TEST_EXAMPLES
int main() {
  // A few instruction words, as they would appear in memory.
  alignas(4) const uint32_t words[] = {
      0xfd400020,  // ldr d0, [x1]        FP or NEON
      0x1e622820,  // fadd d0, d1, d2      FP
      0x8b020020,  // add x0, x1, x2       nothing
      0x9e670000,  // fmov d0, x0          FP or NEON
      0xd503221f,  // esb                  HINT: behaves as a NOP without RAS
  };

  Decoder decoder;
  CPUFeaturesAuditor auditor(&decoder);

  for (const uint32_t& word : words) {
    const Instruction* instr = reinterpret_cast<const Instruction*>(&word);
    decoder.Decode(instr);

    std::cout << "0x" << std::hex << word << std::dec << "  alternatives: ";
    // GetInstructionRequirements() returns a value, so keep a copy alive
    // while inspecting its alternatives.
    CPUFeatureRequirements requirements =
        auditor.GetInstructionRequirements();
    const std::vector<CPUFeatures>& alternatives =
        requirements.GetAlternatives();
    for (size_t i = 0; i < alternatives.size(); ++i) {
      if (i > 0) std::cout << " | ";
      std::cout << "(" << alternatives[i] << ")";
    }
    std::cout << "\n";
  }

  // The combined requirements of all of the instructions above. Alternatives
  // are combined, not concatenated: for example, the first two instructions
  // together need FP, even though taken alone the first could use NEON.
  const CPUFeatureRequirements& all = auditor.GetSeenRequirements();
  std::cout << "Combined: " << all << "\n";
  std::cout << "Satisfied by { FP }: "
            << (all.IsSatisfiedBy(CPUFeatures(CPUFeatures::kFP)) ? "yes"
                                                                : "no")
            << "\n";
  std::cout << "Satisfied by { NEON }: "
            << (all.IsSatisfiedBy(CPUFeatures(CPUFeatures::kNEON)) ? "yes"
                                                                   : "no")
            << "\n";

  return 0;
}
#endif  // TEST_EXAMPLES
