// Copyright 2018, VIXL authors
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

#ifndef VIXL_AARCH64_CPU_FEATURES_AUDITOR_AARCH64_H_
#define VIXL_AARCH64_CPU_FEATURES_AUDITOR_AARCH64_H_

#include <functional>
#include <iostream>
#include <unordered_map>

#include "../cpu-features.h"

#include "decoder-aarch64.h"
#include "decoder-visitor-map-aarch64.h"

namespace vixl {
namespace aarch64 {

// This visitor records the CPU features that each decoded instruction requires.
// It provides:
//  - the set of CPU features required by the most recently decoded instruction,
//  - a cumulative set of encountered CPU features,
//  - an optional list of 'available' CPU features.
//
// Primarily, this allows the Disassembler and Simulator to share the same CPU
// features logic. However, it can be used standalone to scan code blocks for
// CPU features.
class CPUFeaturesAuditor : public DecoderVisitor {
 public:
  // Construction arguments:
  //   - If a decoder is specified, the CPUFeaturesAuditor automatically
  //     registers itself as a visitor. Otherwise, this can be done manually.
  //
  //   - If an `available` features list is provided, it is used as a hint in
  //     cases where instructions may be provided by multiple separate features.
  //     An example of this is FP&SIMD loads and stores: some of these are used
  //     in both FP and integer SIMD code. If exactly one of those features is
  //     in `available` when one of these instructions is encountered, then the
  //     auditor will record that feature. Otherwise, it will record _both_
  //     features.
  explicit CPUFeaturesAuditor(
      Decoder* decoder, const CPUFeatures& available = CPUFeatures::None())
      : available_(available), decoder_(decoder) {
    if (decoder_ != NULL) decoder_->AppendVisitor(this);
  }

  explicit CPUFeaturesAuditor(
      const CPUFeatures& available = CPUFeatures::None())
      : available_(available), decoder_(NULL) {}

  virtual ~CPUFeaturesAuditor() {
    if (decoder_ != NULL) decoder_->RemoveVisitor(this);
  }

  void ResetSeenFeatures() {
    seen_ = CPUFeatures::None();
    last_instruction_ = CPUFeatures::None();
    seen_requirements_ = CPUFeatureRequirements();
    last_requirements_ = CPUFeatureRequirements();
  }

  // Query or set available CPUFeatures.
  const CPUFeatures& GetAvailableFeatures() const { return available_; }
  void SetAvailableFeatures(const CPUFeatures& available) {
    available_ = available;
  }

  // Query CPUFeatures seen since construction (or the last call to `Reset()`).
  //
  // This is the legacy interface. It records, for instructions whose encoding
  // can be provided by more than one feature, a single feature selected using
  // the `available` hint (or both features when no hint applies).
  const CPUFeatures& GetSeenFeatures() const { return seen_; }

  // Query CPUFeatures from the last instruction visited by this auditor.
  // This is the legacy interface; see `GetSeenFeatures()`.
  const CPUFeatures& GetInstructionFeatures() const {
    return last_instruction_;
  }

  // Query the features required by the last instruction visited by this
  // auditor. Unlike `GetInstructionFeatures()`, alternatives are represented
  // exactly and are not influenced by the `available` hint. The result is a
  // self-contained value, so a saved copy is unaffected by later decoding,
  // `ResetSeenFeatures()` or `SetAvailableFeatures()`.
  CPUFeatureRequirements GetInstructionRequirements() const {
    return last_requirements_;
  }

  // Query the combined requirements of every instruction seen since
  // construction or the last call to `ResetSeenFeatures()`. Alternatives are
  // conjoined across instructions: a feature set satisfies the result exactly
  // when it satisfies every instruction's own requirements. As above, the
  // result is a self-contained value.
  CPUFeatureRequirements GetSeenRequirements() const {
    return seen_requirements_;
  }

  bool InstructionIsAvailable() const {
    return available_.Has(last_instruction_);
  }

  // The common CPUFeatures interface operates on the available_ list.
  CPUFeatures* GetCPUFeatures() { return &available_; }
  void SetCPUFeatures(const CPUFeatures& available) {
    SetAvailableFeatures(available);
  }

  virtual void Visit(Metadata* metadata,
                     const Instruction* instr) VIXL_OVERRIDE;

 private:
  class RecordInstructionFeaturesScope;

#define DECLARE(A) virtual void Visit##A(const Instruction* instr);
  VISITOR_LIST(DECLARE)
#undef DECLARE
  void VisitCryptoSM3(const Instruction* instr);
  void VisitCryptoSM4(const Instruction* instr);

  void LoadStoreHelper(const Instruction* instr);
  void LoadStorePairHelper(const Instruction* instr);

  CPUFeatures seen_;
  CPUFeatures last_instruction_;
  CPUFeatures available_;

  // Accurate, alternative-aware counterparts of `seen_` and
  // `last_instruction_`. These are unaffected by `available_`.
  CPUFeatureRequirements seen_requirements_;
  CPUFeatureRequirements last_requirements_;

  Decoder* decoder_;

  using FormToVisitorFnMap = std::unordered_map<
      uint32_t,
      std::function<void(CPUFeaturesAuditor*, const Instruction*)>>;
  static const FormToVisitorFnMap* GetFormToVisitorFnMap();
  uint32_t form_hash_;
};

}  // namespace aarch64
}  // namespace vixl

#endif  // VIXL_AARCH64_CPU_FEATURES_AUDITOR_AARCH64_H_
