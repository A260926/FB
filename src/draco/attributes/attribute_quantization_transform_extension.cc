// Copyright 2026 The Draco Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "draco/attributes/attribute_quantization_transform_extension.h"

#include <cmath>
#include <cstring>

namespace draco {
namespace {

bool IsFiniteValue(float v) { return std::isfinite(v); }

// Validates the ordering/bounds of a decoded record list. Records must be
// strictly increasing in encoded value index and within [0, num_values).
bool ValidateRecords(uint32_t num_values,
                     const std::vector<TopologyPreservingExactValue>& records) {
  uint32_t previous = 0;
  bool first = true;
  for (const TopologyPreservingExactValue& record : records) {
    if (record.encoded_value_index >= num_values) {
      return false;
    }
    if (!first && record.encoded_value_index <= previous) {
      return false;
    }
    first = false;
    previous = record.encoded_value_index;
    for (int c = 0; c < 3; ++c) {
      if (!IsFiniteValue(record.value[c])) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

bool EncodeTopologyPreservingQuantizationExtension(
    uint32_t num_values,
    const std::vector<TopologyPreservingExactValue>& exact_values,
    EncoderBuffer* buffer) {
  if (buffer == nullptr) {
    return false;
  }
  if (!ValidateRecords(num_values, exact_values)) {
    return false;
  }
  if (!buffer->Encode(kTopologyPreservingQuantizationTag) ||
      !buffer->Encode(kTopologyPreservingQuantizationVersion) ||
      !buffer->Encode(num_values) ||
      !buffer->Encode(static_cast<uint32_t>(exact_values.size()))) {
    return false;
  }
  for (const TopologyPreservingExactValue& record : exact_values) {
    if (!buffer->Encode(record.encoded_value_index) ||
        !buffer->Encode(record.value[0]) || !buffer->Encode(record.value[1]) ||
        !buffer->Encode(record.value[2])) {
      return false;
    }
  }
  return true;
}

bool DecodeTopologyPreservingQuantizationExtension(
    uint32_t num_values,
    std::vector<TopologyPreservingExactValue>* exact_values,
    DecoderBuffer* buffer) {
  if (exact_values == nullptr || buffer == nullptr) {
    return false;
  }
  exact_values->clear();
  uint8_t tag;
  uint8_t version;
  uint32_t encoded_num_values;
  uint32_t num_exact;
  if (!buffer->Decode(&tag) || !buffer->Decode(&version) ||
      !buffer->Decode(&encoded_num_values) || !buffer->Decode(&num_exact)) {
    return false;
  }
  if (tag != kTopologyPreservingQuantizationTag ||
      version != kTopologyPreservingQuantizationVersion) {
    return false;
  }
  if (encoded_num_values != num_values) {
    return false;
  }
  // Each record takes 16 bytes on the wire; reject counts that cannot fit in
  // the remaining buffer before allocating.
  if (static_cast<int64_t>(num_exact) * 16 > buffer->remaining_size()) {
    return false;
  }
  if (num_exact > encoded_num_values) {
    return false;
  }
  exact_values->resize(num_exact);
  for (uint32_t i = 0; i < num_exact; ++i) {
    TopologyPreservingExactValue& record = (*exact_values)[i];
    if (!buffer->Decode(&record.encoded_value_index) ||
        !buffer->Decode(&record.value[0]) ||
        !buffer->Decode(&record.value[1]) ||
        !buffer->Decode(&record.value[2])) {
      exact_values->clear();
      return false;
    }
  }
  if (!ValidateRecords(num_values, *exact_values)) {
    exact_values->clear();
    return false;
  }
  return true;
}

void StoreTopologyPreservingQuantizationExtension(
    uint32_t num_values,
    const std::vector<TopologyPreservingExactValue>& exact_values,
    AttributeTransformData* out_data) {
  if (out_data == nullptr) {
    return;
  }
  out_data->AppendParameterValue(kTopologyPreservingQuantizationTag);
  out_data->AppendParameterValue(kTopologyPreservingQuantizationVersion);
  out_data->AppendParameterValue(num_values);
  out_data->AppendParameterValue(static_cast<uint32_t>(exact_values.size()));
  for (const TopologyPreservingExactValue& record : exact_values) {
    out_data->AppendParameterValue(record.encoded_value_index);
    out_data->AppendParameterValue(record.value[0]);
    out_data->AppendParameterValue(record.value[1]);
    out_data->AppendParameterValue(record.value[2]);
  }
}

bool LoadTopologyPreservingQuantizationExtension(
    const AttributeTransformData* in_data, uint32_t* out_num_values,
    std::vector<TopologyPreservingExactValue>* out_exact_values) {
  if (in_data == nullptr || out_num_values == nullptr ||
      out_exact_values == nullptr) {
    return false;
  }
  out_exact_values->clear();
  const int64_t size = in_data->data_size();
  int64_t offset = 0;
  if (size < static_cast<int64_t>(sizeof(uint8_t) * 2 + sizeof(uint32_t) * 2)) {
    return false;
  }
  const uint8_t tag = in_data->GetParameterValue<uint8_t>(offset);
  offset += sizeof(uint8_t);
  const uint8_t version = in_data->GetParameterValue<uint8_t>(offset);
  offset += sizeof(uint8_t);
  if (tag != kTopologyPreservingQuantizationTag ||
      version != kTopologyPreservingQuantizationVersion) {
    return false;
  }
  const uint32_t num_values = in_data->GetParameterValue<uint32_t>(offset);
  offset += sizeof(uint32_t);
  const uint32_t num_exact = in_data->GetParameterValue<uint32_t>(offset);
  offset += sizeof(uint32_t);
  if (num_exact > num_values) {
    return false;
  }
  const int64_t record_bytes =
      static_cast<int64_t>(num_exact) *
      (sizeof(uint32_t) + 3 * sizeof(float));
  if (offset + record_bytes != size) {
    return false;
  }
  out_exact_values->resize(num_exact);
  for (uint32_t i = 0; i < num_exact; ++i) {
    TopologyPreservingExactValue& record = (*out_exact_values)[i];
    record.encoded_value_index = in_data->GetParameterValue<uint32_t>(offset);
    offset += sizeof(uint32_t);
    record.value[0] = in_data->GetParameterValue<float>(offset);
    offset += sizeof(float);
    record.value[1] = in_data->GetParameterValue<float>(offset);
    offset += sizeof(float);
    record.value[2] = in_data->GetParameterValue<float>(offset);
    offset += sizeof(float);
  }
  if (!ValidateRecords(num_values, *out_exact_values)) {
    out_exact_values->clear();
    return false;
  }
  *out_num_values = num_values;
  return true;
}

int64_t TopologyPreservingQuantizationExtensionPayloadSize(
    const std::vector<TopologyPreservingExactValue>& exact_values) {
  return static_cast<int64_t>(exact_values.size()) *
         (sizeof(uint32_t) + 3 * sizeof(float));
}

}  // namespace draco
