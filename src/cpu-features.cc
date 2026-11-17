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

#include "cpu-features.h"

#include <algorithm>
#include <ostream>
#include <vector>

#include "globals-vixl.h"
#include "utils-vixl.h"

#if defined(__aarch64__) && defined(VIXL_INCLUDE_TARGET_AARCH64)
#include "aarch64/cpu-aarch64.h"
#define VIXL_USE_AARCH64_CPU_HELPERS
#endif

namespace vixl {

CPUFeatures CPUFeatures::All() {
  CPUFeatures all;
  all.features_.set();
  return all;
}

CPUFeatures CPUFeatures::InferFromIDRegisters() {
  // This function assumes that kIDRegisterEmulation is available.
  CPUFeatures features(CPUFeatures::kIDRegisterEmulation);
#ifdef VIXL_USE_AARCH64_CPU_HELPERS
  // Note that the Linux kernel filters these values during emulation, so the
  // results may not exactly match the expected hardware support.
  features.Combine(aarch64::CPU::InferCPUFeaturesFromIDRegisters());
#endif
  return features;
}

CPUFeatures CPUFeatures::InferFromOS(QueryIDRegistersOption option) {
#ifdef VIXL_USE_AARCH64_CPU_HELPERS
  return aarch64::CPU::InferCPUFeaturesFromOS(option);
#else
  USE(option);
  return CPUFeatures();
#endif
}

void CPUFeatures::Combine(const CPUFeatures& other) {
  features_ |= other.features_;
}

void CPUFeatures::Combine(Feature feature) {
  if (feature != CPUFeatures::kNone) features_.set(feature);
}

void CPUFeatures::Remove(const CPUFeatures& other) {
  features_ &= ~other.features_;
}

void CPUFeatures::Remove(Feature feature) {
  if (feature != CPUFeatures::kNone) features_.reset(feature);
}

bool CPUFeatures::Has(const CPUFeatures& other) const {
  return (features_ & other.features_) == other.features_;
}

bool CPUFeatures::Has(Feature feature) const {
  return (feature == CPUFeatures::kNone) || features_[feature];
}

size_t CPUFeatures::Count() const { return features_.count(); }

std::ostream& operator<<(std::ostream& os, CPUFeatures::Feature feature) {
  // clang-format off
  switch (feature) {
#define VIXL_FORMAT_FEATURE(SYMBOL, NAME, CPUINFO) \
    case CPUFeatures::SYMBOL:                      \
      return os << NAME;
VIXL_CPU_FEATURE_LIST(VIXL_FORMAT_FEATURE)
#undef VIXL_FORMAT_FEATURE
    case CPUFeatures::kNone:
      return os << "none";
    case CPUFeatures::kNumberOfFeatures:
      VIXL_UNREACHABLE();
  }
  // clang-format on
  VIXL_UNREACHABLE();
  return os;
}

CPUFeatures::const_iterator CPUFeatures::begin() const {
  // For iterators in general, it's undefined to increment `end()`, but here we
  // control the implementation and it is safe to do this.
  return ++end();
}

CPUFeatures::const_iterator CPUFeatures::end() const {
  return const_iterator(this, kNone);
}

std::ostream& operator<<(std::ostream& os, const CPUFeatures& features) {
  bool need_separator = false;
  for (CPUFeatures::Feature feature : features) {
    if (need_separator) os << ", ";
    need_separator = true;
    os << feature;
  }
  return os;
}

bool CPUFeaturesConstIterator::operator==(
    const CPUFeaturesConstIterator& other) const {
  VIXL_ASSERT(IsValid());
  return (cpu_features_ == other.cpu_features_) && (feature_ == other.feature_);
}

CPUFeaturesConstIterator& CPUFeaturesConstIterator::operator++() {  // Prefix
  VIXL_ASSERT(IsValid());
  do {
    // Find the next feature. The order is unspecified.
    feature_ = static_cast<CPUFeatures::Feature>(feature_ + 1);
    if (feature_ == CPUFeatures::kNumberOfFeatures) {
      feature_ = CPUFeatures::kNone;
      VIXL_STATIC_ASSERT(CPUFeatures::kNone == -1);
    }
    VIXL_ASSERT(CPUFeatures::kNone <= feature_);
    VIXL_ASSERT(feature_ < CPUFeatures::kNumberOfFeatures);
    // cpu_features_->Has(kNone) is always true, so this will terminate even if
    // the features list is empty.
  } while (!cpu_features_->Has(feature_));
  return *this;
}

CPUFeaturesConstIterator CPUFeaturesConstIterator::operator++(int) {  // Postfix
  CPUFeaturesConstIterator result = *this;
  ++(*this);
  return result;
}

namespace {

// Return the enabled features of `features`, in feature-enumeration order.
std::vector<CPUFeatures::Feature> FeatureSequence(const CPUFeatures& features) {
  std::vector<CPUFeatures::Feature> sequence;
  for (int i = 0; i < static_cast<int>(CPUFeatures::kNumberOfFeatures); ++i) {
    CPUFeatures::Feature feature = static_cast<CPUFeatures::Feature>(i);
    if (features.Has(feature)) sequence.push_back(feature);
  }
  return sequence;
}

bool FeatureSequenceLess(const CPUFeatures& a, const CPUFeatures& b) {
  return std::lexicographical_compare(FeatureSequence(a).begin(),
                                      FeatureSequence(a).end(),
                                      FeatureSequence(b).begin(),
                                      FeatureSequence(b).end());
}

}  // namespace

void CPUFeatureRequirements::Normalize() {
  // Store the alternatives in a canonical order so that comparison and
  // reduction are deterministic. Duplicate alternatives are removed first, as
  // they would otherwise each hide the other from the strict-superset check.
  std::sort(alternatives_.begin(), alternatives_.end(), FeatureSequenceLess);
  alternatives_.erase(
      std::unique(alternatives_.begin(), alternatives_.end()),
      alternatives_.end());

  // Drop any alternative that is a strict superset of another: if `b` is a
  // subset of `a`, then anything that satisfies `b` also satisfies `a`, so `a`
  // carries no extra information.
  std::vector<CPUFeatures> reduced;
  for (size_t i = 0; i < alternatives_.size(); ++i) {
    bool is_redundant = false;
    for (size_t j = 0; j < alternatives_.size(); ++j) {
      if (i == j) continue;
      // Equality was handled by std::unique above, so any other subset
      // relationship here is strict.
      if (alternatives_[i].Has(alternatives_[j])) {
        is_redundant = true;
        break;
      }
    }
    if (!is_redundant) reduced.push_back(alternatives_[i]);
  }
  // Sort again: removing elements does not disturb the existing order, but
  // being explicit keeps the invariant obvious.
  std::sort(reduced.begin(), reduced.end(), FeatureSequenceLess);
  alternatives_.swap(reduced);
}

bool CPUFeatureRequirements::IsSatisfiedBy(
    const CPUFeatures& available) const {
  for (const CPUFeatures& alternative : alternatives_) {
    if (available.Has(alternative)) return true;
  }
  return false;
}

void CPUFeatureRequirements::Combine(const CPUFeatures& features) {
  for (CPUFeatures& alternative : alternatives_) {
    alternative.Combine(features);
  }
  Normalize();
}

void CPUFeatureRequirements::Combine(const CPUFeatureRequirements& other) {
  std::vector<CPUFeatures> combined;
  for (const CPUFeatures& a : alternatives_) {
    for (const CPUFeatures& b : other.alternatives_) {
      CPUFeatures union_of_alternatives(a);
      union_of_alternatives.Combine(b);
      combined.push_back(union_of_alternatives);
    }
  }
  alternatives_.swap(combined);
  Normalize();
}

void CPUFeatureRequirements::CombineAlternatives(
    const CPUFeatureRequirements& other) {
  alternatives_.insert(alternatives_.end(),
                       other.alternatives_.begin(),
                       other.alternatives_.end());
  Normalize();
}

CPUFeatureRequirements CPUFeatureRequirements::Without(
    const CPUFeatures& available) const {
  std::vector<CPUFeatures> remaining;
  for (const CPUFeatures& alternative : alternatives_) {
    CPUFeatures supplement(alternative);
    supplement.Remove(available);
    remaining.push_back(supplement);
  }
  return CPUFeatureRequirements(remaining);
}

}  // namespace vixl
