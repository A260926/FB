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

// Tests for CPUFeatureRequirements, and for the exact CPU feature queries on
// CPUFeaturesAuditor and the exact annotations from PrintDisassembler.
//
// The instructions used below are read from synthesised instruction words; the
// code under analysis is never executed.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "task_support/check.h"

#include "aarch64/cpu-features-auditor-aarch64.h"
#include "aarch64/decoder-aarch64.h"
#include "aarch64/disasm-aarch64.h"
#include "cpu-features.h"

using vixl::CPUFeatures;
using vixl::CPUFeatureRequirements;
using vixl::aarch64::CPUFeaturesAuditor;
using vixl::aarch64::Decoder;
using vixl::aarch64::Disassembler;
using vixl::aarch64::Instruction;
using vixl::aarch64::PrintDisassembler;

// Synthesised instruction words.
static const uint32_t kAdd = 0x8b020020;            // add x0, x1, x2.
static const uint32_t kFaddD = 0x1e622820;          // fadd d0, d1, d2.
static const uint32_t kLdrD = 0xfd400020;           // ldr d0, [x1].
static const uint32_t kLdrS = 0xbd400020;           // ldr s0, [x1].
static const uint32_t kLdrQ = 0x3dc00020;           // ldr q0, [x1].
static const uint32_t kLdpD = 0x6d404a71;           // ldp d17, d18, [x19].
static const uint32_t kStpS = 0x2d000820;           // stp s0, s2, [x1].
static const uint32_t kLdrDLiteral = 0x5c000000;    // ldr d0, <literal>.
static const uint32_t kLdrQLiteral = 0x9c000000;    // ldr q0, <literal>.
static const uint32_t kFmovDx = 0x9e670222;         // fmov d2, x17.
static const uint32_t kFmovXd1 = 0x9eae0000;        // fmov x0, v0.D[1].
static const uint32_t kFcmpH = 0x1ee02020;          // fcmp h1, #0.0.
static const uint32_t kSha256h = 0x5e024020;        // sha256h q0, q1, v2.4s.
static const uint32_t kAese = 0x4e284820;           // aese v0.16b, v1.16b.
static const uint32_t kPaciasp = 0xd503233f;        // paciasp (a HINT).
static const uint32_t kEsb = 0xd503221f;            // esb (a HINT).
static const uint32_t kCfinv = 0xd500401f;          // cfinv.

// The decoder consumes instruction words through an `Instruction*`, so provide
// properly-aligned storage.
struct InstructionWord {
  alignas(4) uint32_t bits;
  explicit InstructionWord(uint32_t bits_) : bits(bits_) {}
  const Instruction* instruction() const {
    return reinterpret_cast<const Instruction*>(&bits);
  }
};

static CPUFeatures Features(CPUFeatures::Feature f0 = CPUFeatures::kNone,
                            CPUFeatures::Feature f1 = CPUFeatures::kNone,
                            CPUFeatures::Feature f2 = CPUFeatures::kNone,
                            CPUFeatures::Feature f3 = CPUFeatures::kNone) {
  return CPUFeatures(f0, f1, f2, f3);
}

static CPUFeatureRequirements Requirements(
    std::initializer_list<CPUFeatures> alternatives) {
  return CPUFeatureRequirements(alternatives);
}

static bool SameAlternatives(const CPUFeatureRequirements& requirements,
                             std::initializer_list<CPUFeatures> expected) {
  std::vector<CPUFeatures> actual = requirements.GetAlternatives();
  CPUFeatureRequirements expected_requirements(expected);
  std::vector<CPUFeatures> expected_list =
      expected_requirements.GetAlternatives();
  if (actual.size() != expected_list.size()) return false;
  for (size_t i = 0; i < actual.size(); ++i) {
    if (actual[i] != expected_list[i]) return false;
  }
  return true;
}

// Decode a single instruction with a fresh auditor and return the exact
// requirements. If `legacy` is non-NULL, also return the historical feature
// set.
static CPUFeatureRequirements InstructionRequirements(
    uint32_t word,
    const CPUFeatures& available = CPUFeatures::None(),
    CPUFeatures* legacy = NULL) {
  InstructionWord instr(word);
  Decoder decoder;
  CPUFeaturesAuditor auditor(&decoder, available);
  Disassembler dis;
  decoder.AppendVisitor(&dis);
  decoder.Decode(instr.instruction());
  if (legacy != NULL) *legacy = auditor.GetInstructionFeatures();
  return auditor.GetInstructionRequirements();
}

// A tiny decoded-code holder, used by the accumulation tests.
class AuditedSequence {
 public:
  explicit AuditedSequence(const CPUFeatures& available = CPUFeatures::None())
      : decoder_(), auditor_(&decoder_, available), disasm_() {
    decoder_.AppendVisitor(&disasm_);
  }

  void Decode(uint32_t word) {
    InstructionWord instr(word);
    decoder_.Decode(instr.instruction());
  }

  CPUFeaturesAuditor& auditor() { return auditor_; }
  const CPUFeaturesAuditor& auditor() const { return auditor_; }

 private:
  Decoder decoder_;
  CPUFeaturesAuditor auditor_;
  Disassembler disasm_;
};

// Disassemble `count` words and return the text written by the
// PrintDisassembler.
static std::string DisassembleText(uint32_t word,
                                   bool exact,
                                   const CPUFeatures& available,
                                   const char* prefix = NULL,
                                   const char* suffix = NULL,
                                   bool use_buffer_entry = false) {
  // Use an anonymous temporary file. The test environment may restrict names
  // in the temporary directory, but `tmpfile()` only needs the file itself.
  FILE* stream = tmpfile();
  if (stream == NULL) return std::string();

  InstructionWord instr(word);
  CPUFeaturesAuditor auditor(available);
  PrintDisassembler disasm(stream);
  if (prefix != NULL) disasm.SetCPUFeaturesPrefix(prefix);
  if (suffix != NULL) disasm.SetCPUFeaturesSuffix(suffix);
  disasm.SetExactCPUFeatures(exact);
  disasm.RegisterCPUFeaturesAuditor(&auditor);
  // PrintDisassembler's convenience entry points register both the auditor and
  // the disassembler on a private decoder.
  if (use_buffer_entry) {
    disasm.DisassembleBuffer(instr.instruction(), 4);
  } else {
    disasm.Disassemble(instr.instruction());
  }

  fflush(stream);
  fseek(stream, 0, SEEK_END);
  long size = ftell(stream);
  std::string text;
  if (size > 0) {
    text.resize(static_cast<size_t>(size));
    fseek(stream, 0, SEEK_SET);
    size_t read = fread(&text[0], 1, text.size(), stream);
    text.resize(read);
  }
  fclose(stream);
  return text;
}

// ---------------------------------------------------------------------------
// Alternatives: normalisation, ordering and satisfaction of the value type.
// ---------------------------------------------------------------------------

static int TestDefaultRequiresNothing() {
  CPUFeatureRequirements requirements;
  TASK_CHECK(requirements.GetAlternatives().size() == 1);
  TASK_CHECK(requirements.GetAlternatives()[0].HasNoFeatures());
  TASK_CHECK(requirements.IsSatisfiedBy(CPUFeatures::None()));
  TASK_CHECK(requirements.IsSatisfiedBy(Features(CPUFeatures::kFP)));
  TASK_CHECK(requirements == Requirements({CPUFeatures::None()}));
  return 0;
}

static int TestSingleAlternative() {
  CPUFeatureRequirements requirements(Features(CPUFeatures::kFP));
  TASK_CHECK(SameAlternatives(requirements, {Features(CPUFeatures::kFP)}));
  TASK_CHECK(requirements.IsSatisfiedBy(Features(CPUFeatures::kFP)));
  TASK_CHECK(requirements.IsSatisfiedBy(
      Features(CPUFeatures::kFP, CPUFeatures::kNEON)));
  TASK_CHECK(!requirements.IsSatisfiedBy(Features(CPUFeatures::kNEON)));
  TASK_CHECK(!requirements.IsSatisfiedBy(CPUFeatures::None()));
  return 0;
}

static int TestDuplicatesAreRemoved() {
  CPUFeatureRequirements requirements = Requirements(
      {Features(CPUFeatures::kFP), Features(CPUFeatures::kFP),
       Features(CPUFeatures::kNEON)});
  TASK_CHECK(SameAlternatives(
      requirements,
      {Features(CPUFeatures::kFP), Features(CPUFeatures::kNEON)}));
  return 0;
}

static int TestStrictSupersetsAreRemoved() {
  // {FP, NEON} is a strict superset of {FP}, so it carries no information.
  CPUFeatureRequirements requirements = Requirements(
      {Features(CPUFeatures::kFP, CPUFeatures::kNEON),
       Features(CPUFeatures::kFP)});
  TASK_CHECK(SameAlternatives(requirements, {Features(CPUFeatures::kFP)}));

  // An empty alternative makes every other alternative redundant.
  CPUFeatureRequirements with_empty =
      Requirements({Features(CPUFeatures::kFP), CPUFeatures::None()});
  TASK_CHECK(SameAlternatives(with_empty, {CPUFeatures::None()}));
  return 0;
}

static int TestNonSubsetAlternativesAreKept() {
  CPUFeatureRequirements requirements = Requirements(
      {Features(CPUFeatures::kFP, CPUFeatures::kCRC32),
       Features(CPUFeatures::kNEON),
       Features(CPUFeatures::kFP, CPUFeatures::kNEON, CPUFeatures::kCRC32)});
  // The last alternative is a strict superset of both earlier ones.
  TASK_CHECK(SameAlternatives(
      requirements,
      {Features(CPUFeatures::kFP, CPUFeatures::kCRC32),
       Features(CPUFeatures::kNEON)}));
  return 0;
}

static int TestLexicographicOrdering() {
  // FP has a lower feature number than NEON, so {FP, CRC32} sorts before
  // {NEON}, even though the latter has fewer features.
  CPUFeatureRequirements requirements = Requirements(
      {Features(CPUFeatures::kNEON),
       Features(CPUFeatures::kFP, CPUFeatures::kCRC32)});
  std::vector<CPUFeatures> alternatives = requirements.GetAlternatives();
  TASK_CHECK(alternatives.size() == 2);
  TASK_CHECK(alternatives[0] ==
             Features(CPUFeatures::kFP, CPUFeatures::kCRC32));
  TASK_CHECK(alternatives[1] == Features(CPUFeatures::kNEON));

  // Input order does not matter.
  CPUFeatureRequirements reversed = Requirements(
      {Features(CPUFeatures::kFP, CPUFeatures::kCRC32),
       Features(CPUFeatures::kNEON)});
  TASK_CHECK(requirements == reversed);
  return 0;
}

static int TestOrderingWithinAnAlternative() {
  CPUFeatureRequirements requirements = Requirements(
      {Features(CPUFeatures::kNEON, CPUFeatures::kFP),
       Features(CPUFeatures::kCRC32, CPUFeatures::kAES)});
  std::vector<CPUFeatures> alternatives = requirements.GetAlternatives();
  TASK_CHECK(alternatives.size() == 2);
  TASK_CHECK(alternatives[0] == Features(CPUFeatures::kFP, CPUFeatures::kNEON));
  std::vector<CPUFeatures::Feature> expected_order;
  for (CPUFeatures::Feature feature : alternatives[1]) {
    expected_order.push_back(feature);
  }
  TASK_CHECK(expected_order.size() == 2);
  TASK_CHECK(expected_order[0] == CPUFeatures::kCRC32);
  TASK_CHECK(expected_order[1] == CPUFeatures::kAES);
  return 0;
}

static int TestSatisfactionAcrossAlternatives() {
  CPUFeatureRequirements requirements = Requirements(
      {Features(CPUFeatures::kFP), Features(CPUFeatures::kNEON)});
  TASK_CHECK(requirements.IsSatisfiedBy(Features(CPUFeatures::kFP)));
  TASK_CHECK(requirements.IsSatisfiedBy(Features(CPUFeatures::kNEON)));
  TASK_CHECK(requirements.IsSatisfiedBy(
      Features(CPUFeatures::kNEON, CPUFeatures::kCRC32)));
  TASK_CHECK(!requirements.IsSatisfiedBy(Features(CPUFeatures::kCRC32)));
  TASK_CHECK(!requirements.IsSatisfiedBy(CPUFeatures::None()));
  return 0;
}

static int TestCombineIsLogicalAnd() {
  CPUFeatureRequirements a = Requirements(
      {Features(CPUFeatures::kFP), Features(CPUFeatures::kNEON)});
  CPUFeatureRequirements b = Requirements(
      {Features(CPUFeatures::kCRC32), Features(CPUFeatures::kAES)});
  CPUFeatureRequirements combined(a);
  combined.Combine(b);

  // The result is the Cartesian product, and {FP, CRC32} is a subset of
  // {FP, NEON, CRC32}? No: neither is a subset of the other. The only
  // redundant entries would come from duplicates, so all four remain.
  TASK_CHECK(SameAlternatives(
      combined,
      {Features(CPUFeatures::kFP, CPUFeatures::kCRC32),
       Features(CPUFeatures::kFP, CPUFeatures::kAES),
       Features(CPUFeatures::kNEON, CPUFeatures::kCRC32),
       Features(CPUFeatures::kNEON, CPUFeatures::kAES)}));
  TASK_CHECK(combined.IsSatisfiedBy(
      Features(CPUFeatures::kFP, CPUFeatures::kAES)));
  TASK_CHECK(!combined.IsSatisfiedBy(Features(CPUFeatures::kFP)));
  return 0;
}

static int TestCombineWithFeatureSet() {
  CPUFeatureRequirements requirements = Requirements(
      {Features(CPUFeatures::kFP), Features(CPUFeatures::kNEON)});
  requirements.Combine(Features(CPUFeatures::kCRC32));
  TASK_CHECK(SameAlternatives(
      requirements,
      {Features(CPUFeatures::kFP, CPUFeatures::kCRC32),
       Features(CPUFeatures::kNEON, CPUFeatures::kCRC32)}));
  return 0;
}

static int TestCombineAlternativesIsLogicalOr() {
  CPUFeatureRequirements a(Features(CPUFeatures::kFP));
  CPUFeatureRequirements b(Features(CPUFeatures::kNEON));
  a.CombineAlternatives(b);
  TASK_CHECK(SameAlternatives(
      a, {Features(CPUFeatures::kFP), Features(CPUFeatures::kNEON)}));
  return 0;
}

static int TestWithoutReducesAndNormalises() {
  CPUFeatureRequirements requirements = Requirements(
      {Features(CPUFeatures::kFP), Features(CPUFeatures::kNEON)});
  // The {FP} alternative is satisfied, so nothing further is needed: the
  // result is a single empty supplement.
  CPUFeatureRequirements remaining =
      requirements.Without(Features(CPUFeatures::kFP));
  TASK_CHECK(SameAlternatives(remaining, {CPUFeatures::None()}));

  CPUFeatureRequirements none_remaining =
      requirements.Without(Features(CPUFeatures::kNEON, CPUFeatures::kFP));
  TASK_CHECK(SameAlternatives(none_remaining, {CPUFeatures::None()}));

  // Unrelated available features leave the requirement unchanged.
  TASK_CHECK(
      SameAlternatives(requirements.Without(Features(CPUFeatures::kCRC32)),
                       {Features(CPUFeatures::kFP),
                        Features(CPUFeatures::kNEON)}));

  // A multi-feature alternative is reduced by the available features, and
  // the reduced alternatives are normalised again.
  CPUFeatureRequirements both = Requirements(
      {Features(CPUFeatures::kFP, CPUFeatures::kNEON),
       Features(CPUFeatures::kCRC32, CPUFeatures::kAES)});
  CPUFeatureRequirements reduced =
      both.Without(Features(CPUFeatures::kFP, CPUFeatures::kAES));
  TASK_CHECK(SameAlternatives(
      reduced,
      {Features(CPUFeatures::kNEON), Features(CPUFeatures::kCRC32)}));
  return 0;
}

// ---------------------------------------------------------------------------
// Accumulation: auditor behaviour for individual and combined instructions.
// ---------------------------------------------------------------------------

static int TestSharedRequirementIsExact() {
  // `ldr d0, [x1]` can be provided by either FP or NEON.
  CPUFeatureRequirements expected =
      Requirements({Features(CPUFeatures::kFP), Features(CPUFeatures::kNEON)});

  CPUFeatures legacy_none;
  TASK_CHECK(
      InstructionRequirements(kLdrD, CPUFeatures::None(), &legacy_none) ==
      expected);
  TASK_CHECK(legacy_none ==
             Features(CPUFeatures::kFP, CPUFeatures::kNEON));

  // The legacy hint chooses one, but the exact requirement does not change.
  CPUFeatures legacy_fp;
  TASK_CHECK(
      InstructionRequirements(kLdrD, Features(CPUFeatures::kFP), &legacy_fp) ==
      expected);
  TASK_CHECK(legacy_fp == Features(CPUFeatures::kFP));

  CPUFeatures legacy_neon;
  TASK_CHECK(InstructionRequirements(kLdrD, Features(CPUFeatures::kNEON),
                                     &legacy_neon) == expected);
  TASK_CHECK(legacy_neon == Features(CPUFeatures::kNEON));
  return 0;
}

static int TestAllRecordOneOrBothSitesAreRepresented() {
  CPUFeatureRequirements expected =
      Requirements({Features(CPUFeatures::kFP), Features(CPUFeatures::kNEON)});

  // Ordinary loads and stores.
  TASK_CHECK(InstructionRequirements(kLdrD) == expected);
  TASK_CHECK(InstructionRequirements(kLdrS) == expected);
  // A 128-bit access requires NEON only.
  TASK_CHECK(SameAlternatives(InstructionRequirements(kLdrQ),
                             {Features(CPUFeatures::kNEON)}));

  // Load/store pairs.
  TASK_CHECK(InstructionRequirements(kLdpD) == expected);
  TASK_CHECK(InstructionRequirements(kStpS) == expected);

  // Literal loads.
  TASK_CHECK(InstructionRequirements(kLdrDLiteral) == expected);
  TASK_CHECK(SameAlternatives(InstructionRequirements(kLdrQLiteral),
                             {Features(CPUFeatures::kNEON)}));

  // FMOV between a general register and a double register.
  TASK_CHECK(InstructionRequirements(kFmovDx) == expected);
  return 0;
}

static int TestMultipleRecordsOnOneInstructionAreConjunctive() {
  // `fmov x0, v0.D[1]` requires both FP and NEON; recording them separately
  // means "both at once", not "either".
  CPUFeatureRequirements requirements = InstructionRequirements(kFmovXd1);
  TASK_CHECK(SameAlternatives(
      requirements, {Features(CPUFeatures::kFP, CPUFeatures::kNEON)}));
  TASK_CHECK(!requirements.IsSatisfiedBy(Features(CPUFeatures::kFP)));
  TASK_CHECK(requirements.IsSatisfiedBy(
      Features(CPUFeatures::kFP, CPUFeatures::kNEON)));

  // `fcmp h1, #0.0` records FP then FPHalf.
  TASK_CHECK(SameAlternatives(
      InstructionRequirements(kFcmpH),
      {Features(CPUFeatures::kFP, CPUFeatures::kFPHalf)}));
  return 0;
}

static int TestGenericFormMetadataIsPreserved() {
  // These forms are handled by the generic metadata path, which records a
  // combined feature set.
  TASK_CHECK(SameAlternatives(
      InstructionRequirements(kSha256h),
      {Features(CPUFeatures::kNEON, CPUFeatures::kSHA2)}));
  TASK_CHECK(SameAlternatives(
      InstructionRequirements(kAese),
      {Features(CPUFeatures::kNEON, CPUFeatures::kAES)}));
  return 0;
}

static int TestInstructionWithoutSpecialFeatures() {
  TASK_CHECK(SameAlternatives(InstructionRequirements(kAdd),
                             {CPUFeatures::None()}));
  TASK_CHECK(InstructionRequirements(kAdd).IsSatisfiedBy(CPUFeatures::None()));
  return 0;
}

static int TestHintsRequireNothing() {
  // A HINT is a NOP when the extension is absent, so it has no exact
  // requirement.
  TASK_CHECK(SameAlternatives(InstructionRequirements(kPaciasp),
                             {CPUFeatures::None()}));
  TASK_CHECK(SameAlternatives(InstructionRequirements(kEsb),
                             {CPUFeatures::None()}));

  // The historical interface still reports the feature when it is available.
  CPUFeatures legacy;
  InstructionRequirements(kPaciasp, Features(CPUFeatures::kPAuth), &legacy);
  TASK_CHECK(legacy == Features(CPUFeatures::kPAuth));
  TASK_CHECK(SameAlternatives(
      InstructionRequirements(kPaciasp, Features(CPUFeatures::kPAuth)),
      {CPUFeatures::None()}));

  // A non-HINT system instruction keeps its audit requirement.
  TASK_CHECK(SameAlternatives(InstructionRequirements(kCfinv),
                             {Features(CPUFeatures::kFlagM)}));
  return 0;
}

static int TestSeenRequirementsCombineAcrossInstructions() {
  AuditedSequence sequence;
  sequence.Decode(kLdrD);
  TASK_CHECK(SameAlternatives(
      sequence.auditor().GetSeenRequirements(),
      {Features(CPUFeatures::kFP), Features(CPUFeatures::kNEON)}));

  // Adding `fadd d0, d1, d2` (which requires FP) selects the FP alternative.
  sequence.Decode(kFaddD);
  TASK_CHECK(SameAlternatives(sequence.auditor().GetSeenRequirements(),
                             {Features(CPUFeatures::kFP)}));

  // A second shared instruction does not re-introduce NEON.
  sequence.Decode(kLdpD);
  TASK_CHECK(SameAlternatives(sequence.auditor().GetSeenRequirements(),
                             {Features(CPUFeatures::kFP)}));
  return 0;
}

static int TestSeenRequirementsCrossInstructionReduction() {
  AuditedSequence sequence;
  // FP | NEON ...
  sequence.Decode(kLdrD);
  // ... AND (NEON & SHA2): the second alternative is now a subset of the
  // first, so only {NEON, SHA2} remains.
  sequence.Decode(kSha256h);
  TASK_CHECK(SameAlternatives(
      sequence.auditor().GetSeenRequirements(),
      {Features(CPUFeatures::kNEON, CPUFeatures::kSHA2)}));

  sequence.Decode(kAdd);
  TASK_CHECK(SameAlternatives(
      sequence.auditor().GetSeenRequirements(),
      {Features(CPUFeatures::kNEON, CPUFeatures::kSHA2)}));
  return 0;
}

static int TestSeenRequirementsAreNotConcatenated() {
  // The naive union of alternative lists would yield
  // {FP}, {NEON}, {NEON, SHA2}; the correct combination is the intersection
  // of choices, reducing to {NEON, SHA2}.
  AuditedSequence sequence;
  sequence.Decode(kLdrD);
  sequence.Decode(kSha256h);
  CPUFeatureRequirements seen = sequence.auditor().GetSeenRequirements();
  std::vector<CPUFeatures> alternatives = seen.GetAlternatives();
  TASK_CHECK(alternatives.size() == 1);
  TASK_CHECK(alternatives[0] ==
             Features(CPUFeatures::kNEON, CPUFeatures::kSHA2));
  return 0;
}

static int TestInstructionRequirementsAreReplaced() {
  AuditedSequence sequence;
  sequence.Decode(kLdrD);
  TASK_CHECK(SameAlternatives(
      sequence.auditor().GetInstructionRequirements(),
      {Features(CPUFeatures::kFP), Features(CPUFeatures::kNEON)}));

  sequence.Decode(kAdd);
  TASK_CHECK(SameAlternatives(
      sequence.auditor().GetInstructionRequirements(),
      {CPUFeatures::None()}));
  return 0;
}

static int TestSavedRequirementsAreIndependentCopies() {
  AuditedSequence sequence;

  sequence.Decode(kLdrD);
  CPUFeatureRequirements saved_instruction =
      sequence.auditor().GetInstructionRequirements();
  CPUFeatureRequirements saved_seen = sequence.auditor().GetSeenRequirements();

  // Decode more, reset, and change the available features. None of these
  // operations may affect the saved copies.
  sequence.Decode(kSha256h);
  sequence.auditor().SetAvailableFeatures(Features(CPUFeatures::kNEON));
  TASK_CHECK(SameAlternatives(
      saved_instruction,
      {Features(CPUFeatures::kFP), Features(CPUFeatures::kNEON)}));
  TASK_CHECK(SameAlternatives(
      saved_seen,
      {Features(CPUFeatures::kFP), Features(CPUFeatures::kNEON)}));

  sequence.auditor().ResetSeenFeatures();
  TASK_CHECK(SameAlternatives(sequence.auditor().GetSeenRequirements(),
                             {CPUFeatures::None()}));
  TASK_CHECK(SameAlternatives(sequence.auditor().GetInstructionRequirements(),
                             {CPUFeatures::None()}));
  TASK_CHECK(SameAlternatives(
      saved_instruction,
      {Features(CPUFeatures::kFP), Features(CPUFeatures::kNEON)}));
  TASK_CHECK(SameAlternatives(
      saved_seen,
      {Features(CPUFeatures::kFP), Features(CPUFeatures::kNEON)}));

  // Fresh decoding after the reset accumulates from scratch.
  sequence.Decode(kFaddD);
  TASK_CHECK(SameAlternatives(sequence.auditor().GetSeenRequirements(),
                             {Features(CPUFeatures::kFP)}));
  return 0;
}

static int TestLegacyQueriesKeepTheirMeaning() {
  AuditedSequence sequence(Features(CPUFeatures::kFP));
  sequence.Decode(kLdrD);
  TASK_CHECK(sequence.auditor().GetInstructionFeatures() ==
             Features(CPUFeatures::kFP));
  TASK_CHECK(sequence.auditor().InstructionIsAvailable());
  TASK_CHECK(SameAlternatives(
      sequence.auditor().GetInstructionRequirements(),
      {Features(CPUFeatures::kFP), Features(CPUFeatures::kNEON)}));

  sequence.auditor().SetAvailableFeatures(Features(CPUFeatures::kCRC32));
  TASK_CHECK(!sequence.auditor().InstructionIsAvailable());
  sequence.Decode(kLdrD);
  TASK_CHECK(sequence.auditor().GetInstructionFeatures() ==
             Features(CPUFeatures::kFP, CPUFeatures::kNEON));

  // The legacy cumulative set remains a plain union.
  TASK_CHECK(sequence.auditor().GetSeenFeatures() ==
             Features(CPUFeatures::kFP, CPUFeatures::kNEON));
  return 0;
}

// ---------------------------------------------------------------------------
// Display: PrintDisassembler annotations.
// ---------------------------------------------------------------------------

static bool Contains(const std::string& text, const char* needle) {
  return text.find(needle) != std::string::npos;
}

static int TestExactSharedAnnotation() {
  std::string text =
      DisassembleText(kLdrD, true, CPUFeatures::None());
  TASK_CHECK(Contains(text, "ldr d0, [x1]"));
  TASK_CHECK(Contains(text, "// Needs: (FP) | (NEON)"));
  return 0;
}

static int TestExactAnnotationWithOneSatisfiedAlternative() {
  // If any alternative is already available, no annotation is needed at all.
  std::string with_fp =
      DisassembleText(kLdrD, true, Features(CPUFeatures::kFP));
  TASK_CHECK(Contains(with_fp, "ldr d0, [x1]"));
  TASK_CHECK(!Contains(with_fp, "Needs"));

  std::string with_neon =
      DisassembleText(kLdrD, true, Features(CPUFeatures::kNEON));
  TASK_CHECK(!Contains(with_neon, "Needs"));

  std::string with_unrelated =
      DisassembleText(kLdrD, true, Features(CPUFeatures::kCRC32));
  TASK_CHECK(Contains(with_unrelated, "// Needs: (FP) | (NEON)"));
  return 0;
}

static int TestExactConjunctiveAnnotation() {
  // `fmov x0, v0.D[1]` needs FP and NEON together.
  std::string text =
      DisassembleText(kFmovXd1, true, CPUFeatures::None());
  TASK_CHECK(Contains(text, "fmov x0, v0.D[1]"));
  TASK_CHECK(Contains(text, "// Needs: (FP, NEON)"));

  // Supplying one of the two leaves the minimal supplement.
  std::string with_fp =
      DisassembleText(kFmovXd1, true, Features(CPUFeatures::kFP));
  TASK_CHECK(Contains(with_fp, "// Needs: (NEON)"));

  // Supplying both removes the annotation.
  std::string with_both = DisassembleText(
      kFmovXd1, true, Features(CPUFeatures::kFP, CPUFeatures::kNEON));
  TASK_CHECK(!Contains(with_both, "Needs"));
  return 0;
}

static int TestExactAnnotationSuppressesSatisfiedFeatures() {
  // A plain FP instruction reduces to nothing when FP is available, and to
  // just (FP) otherwise.
  std::string without =
      DisassembleText(kFaddD, true, CPUFeatures::None());
  TASK_CHECK(Contains(without, "// Needs: (FP)"));

  std::string with_fp =
      DisassembleText(kFaddD, true, Features(CPUFeatures::kFP));
  TASK_CHECK(!Contains(with_fp, "Needs"));

  // Instructions with no requirements are never annotated.
  std::string add = DisassembleText(kAdd, true, CPUFeatures::None());
  TASK_CHECK(Contains(add, "add x0, x1, x2"));
  TASK_CHECK(!Contains(add, "Needs"));
  return 0;
}

static int TestExactHintAnnotation() {
  // HINTs are NOPs without their extension, so they need nothing exact, even
  // though the legacy interface would report PAuth when it is available.
  std::string without =
      DisassembleText(kPaciasp, true, CPUFeatures::None());
  TASK_CHECK(Contains(without, "paciasp"));
  TASK_CHECK(!Contains(without, "Needs"));
  return 0;
}

static int TestDefaultAnnotationUnchanged() {
  // The default mode keeps the historical annotation behaviour.
  std::string text =
      DisassembleText(kLdrD, false, CPUFeatures::None());
  TASK_CHECK(Contains(text, "ldr d0, [x1]"));
  TASK_CHECK(Contains(text, "// Needs: FP, NEON"));

  std::string with_fp =
      DisassembleText(kLdrD, false, Features(CPUFeatures::kFP));
  // The historical hint selects FP, which is available, so nothing is missing.
  TASK_CHECK(!Contains(with_fp, "Needs"));

  std::string with_crc =
      DisassembleText(kLdrD, false, Features(CPUFeatures::kCRC32));
  TASK_CHECK(Contains(with_crc, "// Needs: FP, NEON"));

  std::string with_both = DisassembleText(
      kLdrD, false, Features(CPUFeatures::kFP, CPUFeatures::kNEON));
  TASK_CHECK(!Contains(with_both, "Needs"));
  return 0;
}

static int TestCustomPrefixAndSuffix() {
  std::string exact = DisassembleText(kLdrD,
                                      true,
                                      CPUFeatures::None(),
                                      "### {",
                                      "} ###");
  TASK_CHECK(Contains(exact, "ldr d0, [x1]"));
  TASK_CHECK(Contains(exact, "### {(FP) | (NEON)} ###"));

  std::string legacy = DisassembleText(kLdrD,
                                       false,
                                       CPUFeatures::None(),
                                       "### {",
                                       "} ###");
  TASK_CHECK(Contains(legacy, "### {FP, NEON} ###"));
  return 0;
}

static int TestExactBufferEntryPoint() {
  // The bulk disassembly entry point uses the same exact logic.
  std::string text = DisassembleText(
      kLdrD, true, CPUFeatures::None(), NULL, NULL, true);
  TASK_CHECK(Contains(text, "ldr d0, [x1]"));
  TASK_CHECK(Contains(text, "// Needs: (FP) | (NEON)"));
  return 0;
}

// ---------------------------------------------------------------------------
// Registration.
// ---------------------------------------------------------------------------

extern const struct task_case task_cases[] = {
    {"Alternatives", "default_requires_nothing", TestDefaultRequiresNothing},
    {"Alternatives", "single_alternative", TestSingleAlternative},
    {"Alternatives", "duplicates_removed", TestDuplicatesAreRemoved},
    {"Alternatives", "strict_supersets_removed", TestStrictSupersetsAreRemoved},
    {"Alternatives", "non_subset_alternatives_kept",
     TestNonSubsetAlternativesAreKept},
    {"Alternatives", "lexicographic_ordering", TestLexicographicOrdering},
    {"Alternatives", "ordering_within_alternative",
     TestOrderingWithinAnAlternative},
    {"Alternatives", "satisfaction", TestSatisfactionAcrossAlternatives},
    {"Alternatives", "combine_is_and", TestCombineIsLogicalAnd},
    {"Alternatives", "combine_feature_set", TestCombineWithFeatureSet},
    {"Alternatives", "combine_alternatives_is_or",
     TestCombineAlternativesIsLogicalOr},
    {"Alternatives", "without_reduces", TestWithoutReducesAndNormalises},

    {"Accumulation", "shared_requirement_exact", TestSharedRequirementIsExact},
    {"Accumulation", "one_or_both_sites",
     TestAllRecordOneOrBothSitesAreRepresented},
    {"Accumulation", "multiple_records_conjunctive",
     TestMultipleRecordsOnOneInstructionAreConjunctive},
    {"Accumulation", "generic_form_metadata",
     TestGenericFormMetadataIsPreserved},
    {"Accumulation", "no_special_features",
     TestInstructionWithoutSpecialFeatures},
    {"Accumulation", "hints_require_nothing", TestHintsRequireNothing},
    {"Accumulation", "seen_combines_instructions",
     TestSeenRequirementsCombineAcrossInstructions},
    {"Accumulation", "cross_instruction_reduction",
     TestSeenRequirementsCrossInstructionReduction},
    {"Accumulation", "seen_is_not_concatenated",
     TestSeenRequirementsAreNotConcatenated},
    {"Accumulation", "instruction_replaced",
     TestInstructionRequirementsAreReplaced},
    {"Accumulation", "saved_copies_independent",
     TestSavedRequirementsAreIndependentCopies},
    {"Accumulation", "legacy_queries_unchanged",
     TestLegacyQueriesKeepTheirMeaning},

    {"Display", "exact_shared", TestExactSharedAnnotation},
    {"Display", "exact_satisfied_alternative",
     TestExactAnnotationWithOneSatisfiedAlternative},
    {"Display", "exact_conjunctive", TestExactConjunctiveAnnotation},
    {"Display", "exact_suppresses_satisfied",
     TestExactAnnotationSuppressesSatisfiedFeatures},
    {"Display", "exact_hint", TestExactHintAnnotation},
    {"Display", "default_unchanged", TestDefaultAnnotationUnchanged},
    {"Display", "custom_prefix_suffix", TestCustomPrefixAndSuffix},
    {"Display", "exact_buffer_entry", TestExactBufferEntryPoint},
};

extern const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
