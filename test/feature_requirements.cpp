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

// Regression tests for CPUFeatureRequirements and its integration with
// CPUFeaturesAuditor and PrintDisassembler.
//
// These tests decode raw instruction words. They never execute the code they
// inspect.

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "task_support/check.h"

#include "aarch64/cpu-features-auditor-aarch64.h"
#include "aarch64/decoder-aarch64.h"
#include "aarch64/disasm-aarch64.h"

using vixl::CPUFeatureRequirements;
using vixl::CPUFeatures;
using vixl::aarch64::CPUFeaturesAuditor;
using vixl::aarch64::Decoder;
using vixl::aarch64::Disassembler;
using vixl::aarch64::Instruction;
using vixl::aarch64::PrintDisassembler;

namespace {

// Encodings used throughout the tests. All are read-only instruction words.
const uint32_t kLdrD0X1 = 0xfd400020;   // ldr d0, [x1]          FP or NEON
const uint32_t kStrD0X1 = 0xfd000020;   // str d0, [x1]          FP or NEON
const uint32_t kLdpD0D1 = 0x6d400020;   // ldp d0, d1, [x1]      FP or NEON
const uint32_t kStpD0D1 = 0x6d000020;   // stp d0, d1, [x1]      FP or NEON
const uint32_t kLdrSLit = 0x1c000000;   // ldr s0, <literal>     FP or NEON
const uint32_t kLdrDLit = 0x5c000000;   // ldr d0, <literal>     FP or NEON
const uint32_t kLdrQLit = 0x9c000000;   // ldr q0, <literal>     NEON
const uint32_t kFmovDX = 0x9e670000;    // fmov d0, x0           FP or NEON
const uint32_t kFmovXD = 0x9e660000;    // fmov x0, d0           FP
const uint32_t kFmovD1X = 0x9eaf01a0;   // fmov v0.D[1], x13     FP and NEON
const uint32_t kFmovXD1 = 0x9eae000d;   // fmov x13, v0.D[1]     FP and NEON
const uint32_t kFaddD = 0x1e622820;     // fadd d0, d1, d2       FP
const uint32_t kFaddV4S = 0x4e22d420;   // fadd v0.4s, v1.4s, v2.4s  FP and NEON
const uint32_t kAddX = 0x8b020020;      // add x0, x1, x2        nothing
const uint32_t kBdep = 0x4513b784;      // bdep z4.b, z28.b, z19.b   SVE2 + BitPerm
const uint32_t kAese = 0x4e28482a;      // aese v10.16b, v1.16b  NEON + AES
const uint32_t kCfinv = 0xd500401f;     // cfinv                 FlagM
const uint32_t kEsb = 0xd503221f;       // esb                   RAS (HINT: NOP without)
const uint32_t kPaciasp = 0xd503233f;   // paciasp               PAuth (HINT)

// A small helper that owns a decoder and an auditor, and decodes one word at a
// time. The auditor is kept in sync with `SetAvailableFeatures()`.
struct Harness {
  Decoder decoder;
  CPUFeaturesAuditor auditor;

  explicit Harness(const CPUFeatures& available = CPUFeatures::None())
      : auditor(&decoder, available) {}

  void Decode(uint32_t word) {
    alignas(4) uint32_t buffer = word;
    decoder.Decode(reinterpret_cast<const Instruction*>(&buffer));
  }
};

CPUFeatures F_None() { return CPUFeatures::None(); }
CPUFeatures F(CPUFeatures::Feature f0,
              CPUFeatures::Feature f1 = CPUFeatures::kNone,
              CPUFeatures::Feature f2 = CPUFeatures::kNone) {
  return CPUFeatures(f0, f1, f2);
}

// Render requirements in the canonical "(A) | (B)" notation.
std::string Render(const CPUFeatureRequirements& requirements) {
  std::stringstream ss;
  ss << requirements;
  return ss.str();
}

// Check that `requirements` consists of exactly the given alternatives, after
// normalisation. The alternatives are listed in the expected canonical order.
bool Matches(const CPUFeatureRequirements& requirements,
             const std::vector<CPUFeatures>& expected) {
  if (requirements.GetAlternatives().size() != expected.size()) return false;
  for (size_t i = 0; i < expected.size(); ++i) {
    if (requirements.GetAlternatives()[i] != expected[i]) return false;
  }
  return true;
}

// Commonly used alternative sets.
std::vector<CPUFeatures> FPOrNEON() {
  return {F(CPUFeatures::kFP), F(CPUFeatures::kNEON)};
}

bool IsFPOrNEON(const CPUFeatureRequirements& requirements) {
  return Matches(requirements, FPOrNEON());
}

bool IsNothing(const CPUFeatureRequirements& requirements) {
  return Matches(requirements, {F_None()});
}

bool IsFeature(const CPUFeatureRequirements& requirements,
               CPUFeatures::Feature feature) {
  return Matches(requirements, {F(feature)});
}

}  // namespace

// ===========================================================================
// Alternatives: the value type and per-instruction requirements.
// ===========================================================================

static int TestEmptyRequirements() {
  CPUFeatureRequirements requirements;
  TASK_CHECK(requirements.GetAlternatives().size() == 1);
  TASK_CHECK(requirements.GetAlternatives()[0].HasNoFeatures());
  TASK_CHECK(requirements.IsEmpty());
  TASK_CHECK(requirements.IsSatisfiedBy(F_None()));

  CPUFeatureRequirements from_features(F_None());
  TASK_CHECK(from_features == requirements);
  TASK_CHECK(Render(requirements) == "()");
  return 0;
}

static int TestDeduplicationAndSupersetRemoval() {
  // Duplicates are removed.
  CPUFeatureRequirements duplicates({F(CPUFeatures::kFP),
                                     F(CPUFeatures::kFP)});
  TASK_CHECK(IsFeature(duplicates, CPUFeatures::kFP));

  // A strict superset of another alternative is redundant.
  CPUFeatureRequirements superset({F(CPUFeatures::kFP, CPUFeatures::kNEON),
                                   F(CPUFeatures::kFP)});
  TASK_CHECK(IsFeature(superset, CPUFeatures::kFP));

  // Superset removal also happens when the subsets arrive later.
  CPUFeatureRequirements superset_first(
      {F(CPUFeatures::kFP, CPUFeatures::kNEON), F(CPUFeatures::kNEON)});
  TASK_CHECK(IsFeature(superset_first, CPUFeatures::kNEON));

  // The empty alternative subsumes everything.
  CPUFeatureRequirements with_empty({F(CPUFeatures::kFP), F_None()});
  TASK_CHECK(IsNothing(with_empty));
  return 0;
}

static int TestDeterministicOrdering() {
  // Build the same set in different orders; the representation is stable and
  // ordered by feature enumeration.
  CPUFeatureRequirements forward(
      {F(CPUFeatures::kFP), F(CPUFeatures::kNEON), F(CPUFeatures::kCRC32)});
  CPUFeatureRequirements reverse(
      {F(CPUFeatures::kCRC32), F(CPUFeatures::kNEON), F(CPUFeatures::kFP)});

  TASK_CHECK(forward == reverse);
  TASK_CHECK(Matches(forward, {F(CPUFeatures::kFP),
                               F(CPUFeatures::kNEON),
                               F(CPUFeatures::kCRC32)}));
  TASK_CHECK(Render(forward) == "(FP) | (NEON) | (CRC32)");

  // Sorting also extends into feature sets of different lengths. The shorter
  // set sorts first when it is a prefix.
  CPUFeatureRequirements prefix({F(CPUFeatures::kFP, CPUFeatures::kNEON),
                                 F(CPUFeatures::kFP)});
  // Normalisation removes the superset, leaving just the shorter set.
  TASK_CHECK(IsFeature(prefix, CPUFeatures::kFP));
  // Alternatives that share a prefix are ordered by the next feature in
  // enumeration order: NEON precedes CRC32.
  CPUFeatureRequirements lengths({F(CPUFeatures::kFP, CPUFeatures::kCRC32),
                                  F(CPUFeatures::kFP, CPUFeatures::kNEON)});
  TASK_CHECK(Matches(lengths, {F(CPUFeatures::kFP, CPUFeatures::kNEON),
                               F(CPUFeatures::kFP, CPUFeatures::kCRC32)}));
  return 0;
}

static int TestSatisfaction() {
  CPUFeatureRequirements fp_or_neon(FPOrNEON());
  TASK_CHECK(!fp_or_neon.IsSatisfiedBy(F_None()));
  TASK_CHECK(fp_or_neon.IsSatisfiedBy(F(CPUFeatures::kFP)));
  TASK_CHECK(fp_or_neon.IsSatisfiedBy(F(CPUFeatures::kNEON)));
  TASK_CHECK(fp_or_neon.IsSatisfiedBy(F(CPUFeatures::kFP, CPUFeatures::kNEON)));
  TASK_CHECK(
      fp_or_neon.IsSatisfiedBy(F(CPUFeatures::kFP, CPUFeatures::kCRC32)));
  TASK_CHECK(!fp_or_neon.IsSatisfiedBy(F(CPUFeatures::kCRC32)));

  // Simultaneous requirements need every feature.
  CPUFeatureRequirements simultaneous({F(CPUFeatures::kFP, CPUFeatures::kNEON)});
  TASK_CHECK(!simultaneous.IsSatisfiedBy(F(CPUFeatures::kFP)));
  TASK_CHECK(!simultaneous.IsSatisfiedBy(F(CPUFeatures::kNEON)));
  TASK_CHECK(
      simultaneous.IsSatisfiedBy(F(CPUFeatures::kFP, CPUFeatures::kNEON)));

  // The empty requirements are always satisfied.
  TASK_CHECK(CPUFeatureRequirements().IsSatisfiedBy(F_None()));
  return 0;
}

static int TestCombine() {
  CPUFeatureRequirements left(F(CPUFeatures::kFP));
  CPUFeatureRequirements right(FPOrNEON());
  // (FP) AND ((FP) | (NEON)) == (FP).
  left.Combine(right);
  TASK_CHECK(IsFeature(left, CPUFeatures::kFP));

  CPUFeatureRequirements a(F(CPUFeatures::kFP));
  CPUFeatureRequirements b(F(CPUFeatures::kNEON));
  a.Combine(b);
  TASK_CHECK(Matches(a, {F(CPUFeatures::kFP, CPUFeatures::kNEON)}));

  // Cross product, with redundant combinations removed.
  CPUFeatureRequirements c(FPOrNEON());
  CPUFeatureRequirements d(F(CPUFeatures::kCRC32));
  c.Combine(d);
  TASK_CHECK(Matches(c, {F(CPUFeatures::kFP, CPUFeatures::kCRC32),
                         F(CPUFeatures::kNEON, CPUFeatures::kCRC32)}));

  // Combining with "nothing" leaves the other operand unchanged.
  CPUFeatureRequirements e(FPOrNEON());
  e.Combine(CPUFeatureRequirements());
  TASK_CHECK(IsFPOrNEON(e));
  return 0;
}

static int TestSharedLoadStoreRequirements() {
  // FP and NEON both provide these accesses.
  const uint32_t words[] = {kLdrD0X1, kStrD0X1, kLdpD0D1,
                            kStpD0D1, kLdrSLit, kLdrDLit, kFmovDX};
  for (uint32_t word : words) {
    Harness harness;
    harness.Decode(word);
    TASK_CHECK(IsFPOrNEON(harness.auditor.GetInstructionRequirements()));
    TASK_CHECK(IsFPOrNEON(harness.auditor.GetSeenRequirements()));
  }

  // 128-bit accesses only need NEON.
  {
    Harness harness;
    harness.Decode(kLdrQLit);
    TASK_CHECK(IsFeature(harness.auditor.GetInstructionRequirements(),
                         CPUFeatures::kNEON));
    TASK_CHECK(IsFeature(harness.auditor.GetSeenRequirements(),
                         CPUFeatures::kNEON));
  }
  return 0;
}

static int TestSimultaneousRequirements() {
  // FMOV between a general register and the top half of a Q register requires
  // both FP and NEON at the same time.
  const uint32_t words[] = {kFmovD1X, kFmovXD1};
  for (uint32_t word : words) {
    Harness harness;
    harness.Decode(word);
    const CPUFeatureRequirements& requirements =
        harness.auditor.GetInstructionRequirements();
    TASK_CHECK(Matches(requirements,
                       {F(CPUFeatures::kFP, CPUFeatures::kNEON)}));
    TASK_CHECK(!requirements.IsSatisfiedBy(F(CPUFeatures::kFP)));
    TASK_CHECK(!requirements.IsSatisfiedBy(F(CPUFeatures::kNEON)));
    TASK_CHECK(requirements.IsSatisfiedBy(
        F(CPUFeatures::kFP, CPUFeatures::kNEON)));
  }

  // Other multi-feature forms keep their joint requirements too.
  {
    Harness harness;
    harness.Decode(kBdep);
    TASK_CHECK(Matches(harness.auditor.GetInstructionRequirements(),
                       {F(CPUFeatures::kSVE2, CPUFeatures::kSVEBitPerm)}));
  }
  {
    Harness harness;
    harness.Decode(kAese);
    TASK_CHECK(Matches(harness.auditor.GetInstructionRequirements(),
                       {F(CPUFeatures::kNEON, CPUFeatures::kAES)}));
  }
  {
    Harness harness;
    harness.Decode(kCfinv);
    TASK_CHECK(IsFeature(harness.auditor.GetInstructionRequirements(),
                         CPUFeatures::kFlagM));
  }

  // Instructions with no extra requirements are represented by the empty set.
  {
    Harness harness;
    harness.Decode(kAddX);
    TASK_CHECK(IsNothing(harness.auditor.GetInstructionRequirements()));
    harness.Decode(kFaddD);
    TASK_CHECK(IsFeature(harness.auditor.GetInstructionRequirements(),
                         CPUFeatures::kFP));
  }
  return 0;
}

static int TestHintsNeedNothing() {
  // HINT-space instructions decode and execute as NOPs when their extension
  // is absent, so the accurate requirements are empty regardless of hints.
  for (uint32_t word : {kEsb, kPaciasp}) {
    Harness harness;
    harness.Decode(word);
    TASK_CHECK(IsNothing(harness.auditor.GetInstructionRequirements()));
    TASK_CHECK(IsNothing(harness.auditor.GetSeenRequirements()));
  }

  // ... but the legacy interface still records the feature when it is
  // available.
  {
    Harness harness(F(CPUFeatures::kRAS));
    harness.Decode(kEsb);
    TASK_CHECK(harness.auditor.GetInstructionFeatures() ==
               F(CPUFeatures::kRAS));
  }
  {
    Harness harness;  // RAS unavailable: the hint records nothing.
    harness.Decode(kEsb);
    TASK_CHECK(harness.auditor.GetInstructionFeatures() == F_None());
  }
  return 0;
}

static int TestAvailableDoesNotAffectRequirements() {
  // Whether the available hint mentions FP, NEON or both, the accurate
  // requirements are the same.
  const CPUFeatures hint_sets[] = {
      F_None(), F(CPUFeatures::kFP), F(CPUFeatures::kNEON),
      F(CPUFeatures::kFP, CPUFeatures::kNEON), F(CPUFeatures::kCRC32)};
  for (const CPUFeatures& available : hint_sets) {
    Harness harness(available);
    harness.Decode(kLdrD0X1);
    TASK_CHECK(IsFPOrNEON(harness.auditor.GetInstructionRequirements()));
    TASK_CHECK(IsFPOrNEON(harness.auditor.GetSeenRequirements()));

    // The legacy interface does follow the hint, and is unchanged.
    CPUFeatures legacy = harness.auditor.GetInstructionFeatures();
    if (available.Has(CPUFeatures::kFP) &&
        !available.Has(CPUFeatures::kNEON)) {
      TASK_CHECK(legacy == F(CPUFeatures::kFP));
    } else if (available.Has(CPUFeatures::kNEON) &&
               !available.Has(CPUFeatures::kFP)) {
      TASK_CHECK(legacy == F(CPUFeatures::kNEON));
    } else {
      TASK_CHECK(legacy == F(CPUFeatures::kFP, CPUFeatures::kNEON));
    }
  }
  return 0;
}

static int TestLegacyInterfaceUnchanged() {
  // The legacy queries keep their original meaning, including the hint-based
  // choice, while the new queries report the alternatives.
  Harness harness(F(CPUFeatures::kFP));
  harness.Decode(kLdrD0X1);
  TASK_CHECK(harness.auditor.GetInstructionFeatures() == F(CPUFeatures::kFP));
  TASK_CHECK(harness.auditor.GetSeenFeatures() == F(CPUFeatures::kFP));
  TASK_CHECK(IsFPOrNEON(harness.auditor.GetInstructionRequirements()));

  // InstructionIsAvailable() still uses the legacy, hint-selected set: with FP
  // available, only FP was recorded, so this instruction is available.
  TASK_CHECK(harness.auditor.InstructionIsAvailable());
  harness.auditor.SetAvailableFeatures(F(CPUFeatures::kCRC32));
  TASK_CHECK(!harness.auditor.InstructionIsAvailable());
  // Changing availability does not change a decoded instruction's
  // requirements.
  TASK_CHECK(IsFPOrNEON(harness.auditor.GetInstructionRequirements()));

  // An instruction that needs nothing is always available.
  harness.Decode(kAddX);
  TASK_CHECK(harness.auditor.InstructionIsAvailable());
  TASK_CHECK(harness.auditor.GetInstructionFeatures() == F_None());
  return 0;
}

// ===========================================================================
// Accumulation: GetSeenRequirements() over multiple instructions.
// ===========================================================================

static int TestAccumulationCombinesAlternatives() {
  Harness harness;
  harness.Decode(kLdrD0X1);  // (FP) | (NEON)
  TASK_CHECK(IsFPOrNEON(harness.auditor.GetSeenRequirements()));

  harness.Decode(kFmovXD);  // (FP)
  // ((FP) | (NEON)) AND (FP) reduces to (FP).
  TASK_CHECK(IsFeature(harness.auditor.GetSeenRequirements(),
                       CPUFeatures::kFP));

  // A later alternative re-introduces a choice.
  harness.Decode(kFaddV4S);  // (FP, NEON)
  // (FP) AND (FP, NEON) == (FP, NEON).
  TASK_CHECK(Matches(harness.auditor.GetSeenRequirements(),
                     {F(CPUFeatures::kFP, CPUFeatures::kNEON)}));
  return 0;
}

static int TestAccumulationIsNotConcatenation() {
  Harness harness;
  harness.Decode(kLdrD0X1);  // (FP) | (NEON)
  harness.Decode(kFaddD);    // (FP)
  // Concatenating alternatives would give "(FP) | (NEON)"; the correct
  // combination needs FP and one of FP or NEON, which reduces to FP alone.
  TASK_CHECK(IsFeature(harness.auditor.GetSeenRequirements(),
                       CPUFeatures::kFP));
  TASK_CHECK(Render(harness.auditor.GetSeenRequirements()) == "(FP)");

  // Order does not matter.
  Harness reversed;
  reversed.Decode(kFaddD);
  reversed.Decode(kLdrD0X1);
  TASK_CHECK(reversed.auditor.GetSeenRequirements() ==
             harness.auditor.GetSeenRequirements());
  return 0;
}

static int TestAccumulationReducesAcrossInstructions() {
  Harness harness;
  harness.Decode(kLdrD0X1);   // (FP) | (NEON)
  harness.Decode(kStrD0X1);   // (FP) | (NEON)
  TASK_CHECK(IsFPOrNEON(harness.auditor.GetSeenRequirements()));

  harness.Decode(kLdrQLit);   // (NEON)
  // ((FP) | (NEON)) AND (NEON) == (NEON).
  TASK_CHECK(IsFeature(harness.auditor.GetSeenRequirements(),
                       CPUFeatures::kNEON));
  TASK_CHECK(!harness.auditor.GetSeenRequirements().IsSatisfiedBy(
      F(CPUFeatures::kFP)));

  // Adding instructions that need nothing changes nothing.
  CPUFeatureRequirements before = harness.auditor.GetSeenRequirements();
  harness.Decode(kAddX);
  harness.Decode(kEsb);
  TASK_CHECK(harness.auditor.GetSeenRequirements() == before);
  return 0;
}

static int TestSavedCopiesAndReset() {
  Harness harness;

  // The instruction requirements are a value; saving one and then decoding
  // another instruction must not alter it.
  harness.Decode(kLdrD0X1);
  CPUFeatureRequirements saved_instruction =
      harness.auditor.GetInstructionRequirements();
  harness.Decode(kFaddD);
  TASK_CHECK(IsFPOrNEON(saved_instruction));
  TASK_CHECK(IsFeature(harness.auditor.GetInstructionRequirements(),
                       CPUFeatures::kFP));

  // The seen requirements are likewise a value: `saved_seen` was taken before
  // SVE2 and SVE BitPerm were added to the accumulated set.
  CPUFeatureRequirements saved_seen = harness.auditor.GetSeenRequirements();
  harness.Decode(kFaddV4S);
  harness.Decode(kBdep);
  TASK_CHECK(IsFeature(saved_seen, CPUFeatures::kFP));
  TASK_CHECK(saved_seen.IsSatisfiedBy(F(CPUFeatures::kFP)));
  TASK_CHECK(saved_seen != harness.auditor.GetSeenRequirements());
  TASK_CHECK(!harness.auditor.GetSeenRequirements().IsSatisfiedBy(
      F(CPUFeatures::kFP)));

  // Changing availability does not change saved values.
  CPUFeatureRequirements saved_before_available =
      harness.auditor.GetSeenRequirements();
  harness.auditor.SetAvailableFeatures(CPUFeatures::All());
  TASK_CHECK(harness.auditor.GetSeenRequirements() ==
             saved_before_available);
  TASK_CHECK(harness.auditor.GetInstructionRequirements() ==
             CPUFeatureRequirements(
                 {F(CPUFeatures::kSVE2, CPUFeatures::kSVEBitPerm)}));

  // Reset clears both the legacy and the accurate accumulated state, and
  // leaves saved copies intact.
  harness.auditor.ResetSeenFeatures();
  TASK_CHECK(IsNothing(harness.auditor.GetSeenRequirements()));
  TASK_CHECK(IsNothing(harness.auditor.GetInstructionRequirements()));
  TASK_CHECK(IsFeature(saved_seen, CPUFeatures::kFP));
  TASK_CHECK(harness.auditor.GetSeenFeatures() == F_None());
  return 0;
}

// ===========================================================================
// Display: PrintDisassembler in exact and legacy modes.
// ===========================================================================

namespace {

// Capture a single instruction's PrintDisassembler output line. The
// disassembler always prints a fixed address/bytes prefix; the tests compare
// the instruction text and annotation portion.
std::string DisassembleOne(const uint32_t* words,
                           size_t count,
                           bool exact,
                           const CPUFeatures& available,
                           const char* prefix = NULL,
                           const char* suffix = NULL) {
  FILE* stream = tmpfile();
  if (stream == NULL) return "<tmpfile failed>";

  {
    Decoder decoder;
    CPUFeaturesAuditor auditor(&decoder, available);
    PrintDisassembler disassembler(stream);
    disassembler.RegisterCPUFeaturesAuditor(&auditor);
    disassembler.SetExactCPUFeatures(exact);
    if (prefix != NULL) disassembler.SetCPUFeaturesPrefix(prefix);
    if (suffix != NULL) disassembler.SetCPUFeaturesSuffix(suffix);
    decoder.AppendVisitor(&disassembler);
    decoder.Decode(reinterpret_cast<const Instruction*>(words),
                   reinterpret_cast<const Instruction*>(words + count));
  }

  std::string result;
  std::rewind(stream);
  char buffer[1024];
  while (fgets(buffer, sizeof(buffer), stream) != NULL) {
    result += buffer;
  }
  fclose(stream);
  return result;
}

// Extract everything after the two tab characters that separate the encoded
// bytes from the disassembly proper.
std::string TailOf(const std::string& line) {
  size_t tabs = line.find("\t\t");
  if (tabs == std::string::npos) return line;
  std::string tail = line.substr(tabs + 2);
  while (!tail.empty() && (tail.back() == '\n')) tail.pop_back();
  return tail;
}

// True if `text` ends with `expected_annotation` (an annotation includes its
// prefix and suffix). This tolerates the arbitrary padding spaces.
bool EndsWithAnnotation(const std::string& text,
                        const std::string& expected_annotation) {
  if (text.size() < expected_annotation.size()) return false;
  return text.compare(text.size() - expected_annotation.size(),
                      expected_annotation.size(),
                      expected_annotation) == 0;
}

std::string InstructionText(const std::string& line) {
  std::string tail = TailOf(line);
  size_t marker = tail.find("// ");
  if (marker != std::string::npos) {
    size_t end = marker;
    while (end > 0 && tail[end - 1] == ' ') --end;
    tail = tail.substr(0, end);
  }
  // Trim trailing spaces.
  while (!tail.empty() && tail.back() == ' ') tail.pop_back();
  return tail;
}

}  // namespace

static int TestExactDisplaySharedLoad() {
  std::string line = DisassembleOne(&kLdrD0X1, 1, true, F_None());
  TASK_CHECK(InstructionText(line) == "ldr d0, [x1]");
  TASK_CHECK(EndsWithAnnotation(TailOf(line), "// Needs: (FP) | (NEON)"));

  // Once either feature is available, nothing is printed.
  TASK_CHECK(TailOf(DisassembleOne(&kLdrD0X1, 1, true, F(CPUFeatures::kFP))) ==
             "ldr d0, [x1]");
  TASK_CHECK(
      TailOf(DisassembleOne(&kLdrD0X1, 1, true, F(CPUFeatures::kNEON))) ==
      "ldr d0, [x1]");
  // Having an unrelated feature does not suppress the annotation.
  TASK_CHECK(EndsWithAnnotation(
      TailOf(DisassembleOne(&kLdrD0X1, 1, true, F(CPUFeatures::kCRC32))),
      "// Needs: (FP) | (NEON)"));
  return 0;
}

static int TestExactDisplaySimultaneous() {
  std::string line = DisassembleOne(&kFmovD1X, 1, true, F_None());
  TASK_CHECK(InstructionText(line) == "fmov v0.D[1], x13");
  TASK_CHECK(EndsWithAnnotation(TailOf(line), "// Needs: (FP, NEON)"));

  // A partial alternative is printed without the already-available feature.
  TASK_CHECK(EndsWithAnnotation(
      TailOf(DisassembleOne(&kFmovD1X, 1, true, F(CPUFeatures::kFP))),
      "// Needs: (NEON)"));
  TASK_CHECK(EndsWithAnnotation(
      TailOf(DisassembleOne(&kFmovD1X, 1, true, F(CPUFeatures::kNEON))),
      "// Needs: (FP)"));
  // Both available: no annotation.
  TASK_CHECK(TailOf(DisassembleOne(
                 &kFmovD1X, 1, true,
                 F(CPUFeatures::kFP, CPUFeatures::kNEON))) ==
             "fmov v0.D[1], x13");
  return 0;
}

static int TestExactDisplayReducesComplements() {
  // Build requirements and render them the same way the disassembler does:
  // subtract the available features from every alternative, stop if any
  // alternative is already satisfied, then normalise what is left.
  auto render_remaining = [](const CPUFeatureRequirements& requirements,
                             const CPUFeatures& available) {
    std::vector<CPUFeatures> remaining;
    for (const CPUFeatures& alternative : requirements.GetAlternatives()) {
      CPUFeatures missing = alternative.Without(available);
      if (missing.HasNoFeatures()) return std::string();
      remaining.push_back(missing);
    }
    return Render(CPUFeatureRequirements(remaining));
  };

  // With only an unrelated feature available, both options are still shown.
  CPUFeatureRequirements choice({F(CPUFeatures::kFP), F(CPUFeatures::kNEON)});
  TASK_CHECK(render_remaining(choice, F(CPUFeatures::kCRC32)) ==
             "(FP) | (NEON)");
  // With either option available, the instruction is satisfied.
  TASK_CHECK(render_remaining(choice, F(CPUFeatures::kFP)).empty());
  TASK_CHECK(render_remaining(choice, F(CPUFeatures::kNEON)).empty());

  // A superset alternative is removed once it becomes redundant after the
  // subtraction: (FP, AES) | (NEON, AES) | (FP, NEON), with AES available,
  // leaves (FP) | (NEON) | (FP, NEON), and the last is dropped as a superset.
  CPUFeatureRequirements overlapping({F(CPUFeatures::kFP, CPUFeatures::kAES),
                                      F(CPUFeatures::kNEON, CPUFeatures::kAES),
                                      F(CPUFeatures::kFP, CPUFeatures::kNEON)});
  TASK_CHECK(render_remaining(overlapping, F(CPUFeatures::kAES)) ==
             "(FP) | (NEON)");

  // A superset alternative is also removed when the requirements are built:
  // (FP) | (FP, NEON) is just (FP), so with FP available there is no
  // annotation, even though the NEON part of the larger alternative is
  // missing.
  CPUFeatureRequirements superset(
      {F(CPUFeatures::kFP), F(CPUFeatures::kFP, CPUFeatures::kNEON)});
  TASK_CHECK(superset == CPUFeatureRequirements(F(CPUFeatures::kFP)));
  TASK_CHECK(render_remaining(superset, F(CPUFeatures::kFP)).empty());
  // With NEON available instead, subtraction leaves (FP) | (FP), which
  // reduces to a single (FP) rather than duplicating it.
  TASK_CHECK(render_remaining(superset, F(CPUFeatures::kNEON)) == "(FP)");

  // End to end: the instruction's genuine two-way choice is displayed as both
  // alternatives, even with an unrelated feature available.
  std::string line = DisassembleOne(&kLdrD0X1, 1, true,
                                    F(CPUFeatures::kCRC32));
  TASK_CHECK(EndsWithAnnotation(TailOf(line), "// Needs: (FP) | (NEON)"));
  return 0;
}

static int TestExactDisplayNoRequirements() {
  // Instructions that need nothing are not annotated, even in exact mode.
  for (uint32_t word : {kAddX, kEsb}) {
    std::string line = DisassembleOne(&word, 1, true, F_None());
    TASK_CHECK(line.find("// Needs:") == std::string::npos);
  }
  // A plain FP instruction shows just its feature.
  TASK_CHECK(EndsWithAnnotation(
      TailOf(DisassembleOne(&kFaddD, 1, true, F_None())),
      "// Needs: (FP)"));
  // And is suppressed when FP is available.
  TASK_CHECK(TailOf(DisassembleOne(&kFaddD, 1, true, F(CPUFeatures::kFP))) ==
             "fadd d0, d1, d2");
  return 0;
}

static int TestExactDisplayWithCustomAffixes() {
  // Custom prefix and suffix wrap the exact alternatives just like the legacy
  // annotations.
  std::string line = DisassembleOne(&kLdrD0X1, 1, true, F_None(),
                                    "[", "]");
  TASK_CHECK(EndsWithAnnotation(TailOf(line), "[(FP) | (NEON)]"));
  TASK_CHECK(line.find("// Needs:") == std::string::npos);
  return 0;
}

static int TestLegacyDisplayUnchanged() {
  // Default mode: the legacy annotation is used, with one feature set.
  std::string line = DisassembleOne(&kLdrD0X1, 1, false, F_None());
  TASK_CHECK(InstructionText(line) == "ldr d0, [x1]");
  TASK_CHECK(EndsWithAnnotation(TailOf(line), "// Needs: FP, NEON"));

  // The available features are still subtracted, in the legacy way. As with
  // the original interface, the hint selects FP for the recorded set, so no
  // annotation appears when FP is available.
  TASK_CHECK(TailOf(DisassembleOne(&kLdrD0X1, 1, false,
                                   F(CPUFeatures::kFP))) == "ldr d0, [x1]");
  // With only an unrelated feature available, both recorded features remain.
  TASK_CHECK(EndsWithAnnotation(
      TailOf(DisassembleOne(&kLdrD0X1, 1, false, F(CPUFeatures::kCRC32))),
      "// Needs: FP, NEON"));
  TASK_CHECK(TailOf(DisassembleOne(&kLdrD0X1, 1, false,
                                   F(CPUFeatures::kFP, CPUFeatures::kNEON))) ==
             "ldr d0, [x1]");

  // Custom affixes still work in legacy mode.
  TASK_CHECK(EndsWithAnnotation(
      TailOf(DisassembleOne(&kFaddD, 1, false, F_None(), "NEEDS<", ">")),
      "NEEDS<FP>"));

  // Toggling exact mode off restores the legacy output.
  FILE* stream = tmpfile();
  TASK_CHECK(stream != NULL);
  {
    Decoder decoder;
    CPUFeaturesAuditor auditor(&decoder, F_None());
    PrintDisassembler disassembler(stream);
    disassembler.RegisterCPUFeaturesAuditor(&auditor);
    disassembler.SetExactCPUFeatures(true);
    decoder.AppendVisitor(&disassembler);
    alignas(4) uint32_t words[] = {kLdrD0X1, kLdrD0X1};
    decoder.Decode(reinterpret_cast<const Instruction*>(words),
                   reinterpret_cast<const Instruction*>(words + 1));
    disassembler.SetExactCPUFeatures(false);
    decoder.Decode(reinterpret_cast<const Instruction*>(words + 1),
                   reinterpret_cast<const Instruction*>(words + 2));
  }
  std::string all;
  std::rewind(stream);
  char buffer[1024];
  while (fgets(buffer, sizeof(buffer), stream) != NULL) all += buffer;
  fclose(stream);

  size_t newline = all.find('\n');
  TASK_CHECK(newline != std::string::npos);
  std::string first = all.substr(0, newline + 1);
  std::string second = all.substr(newline + 1);
  TASK_CHECK(EndsWithAnnotation(TailOf(first), "// Needs: (FP) | (NEON)"));
  TASK_CHECK(EndsWithAnnotation(TailOf(second), "// Needs: FP, NEON"));
  return 0;
}

// ===========================================================================
// Registration.
// ===========================================================================

const struct task_case task_cases[] = {
    {"Alternatives", "empty", TestEmptyRequirements},
    {"Alternatives", "dedup_and_superset", TestDeduplicationAndSupersetRemoval},
    {"Alternatives", "ordering", TestDeterministicOrdering},
    {"Alternatives", "satisfaction", TestSatisfaction},
    {"Alternatives", "combine", TestCombine},
    {"Alternatives", "shared_load_store", TestSharedLoadStoreRequirements},
    {"Alternatives", "simultaneous", TestSimultaneousRequirements},
    {"Alternatives", "hints_need_nothing", TestHintsNeedNothing},
    {"Alternatives", "available_hint_independent",
     TestAvailableDoesNotAffectRequirements},
    {"Alternatives", "legacy_interface", TestLegacyInterfaceUnchanged},
    {"Accumulation", "combine_alternatives", TestAccumulationCombinesAlternatives},
    {"Accumulation", "not_concatenation", TestAccumulationIsNotConcatenation},
    {"Accumulation", "cross_instruction_reduction",
     TestAccumulationReducesAcrossInstructions},
    {"Accumulation", "saved_copies_and_reset", TestSavedCopiesAndReset},
    {"Display", "shared_load", TestExactDisplaySharedLoad},
    {"Display", "simultaneous", TestExactDisplaySimultaneous},
    {"Display", "reduced_complements", TestExactDisplayReducesComplements},
    {"Display", "no_requirements", TestExactDisplayNoRequirements},
    {"Display", "custom_affixes", TestExactDisplayWithCustomAffixes},
    {"Display", "legacy_unchanged", TestLegacyDisplayUnchanged},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
