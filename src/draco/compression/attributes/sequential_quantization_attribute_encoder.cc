// Copyright 2016 The Draco Authors.
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
//
#include "draco/compression/attributes/sequential_quantization_attribute_encoder.h"

#include <algorithm>
#include <utility>

#include "draco/compression/attributes/topology_preserving_quantization.h"
#include "draco/core/quantization_utils.h"

namespace draco {

SequentialQuantizationAttributeEncoder::
    SequentialQuantizationAttributeEncoder() = default;

bool SequentialQuantizationAttributeEncoder::Init(PointCloudEncoder* encoder,
                                                  int attribute_id) {
  if (!SequentialIntegerAttributeEncoder::Init(encoder, attribute_id)) {
    return false;
  }
  // This encoder currently works only for floating point attributes.
  const PointAttribute* const attribute =
      encoder->point_cloud()->attribute(attribute_id);
  if (attribute->data_type() != DT_FLOAT32) {
    return false;
  }

  // The topology preserving plan is optional and only applies to the POSITION
  // attribute of a mesh. When it is active the plan's quantization parameters
  // are authoritative so that the lattice/dequantizer used for the fixed point
  // computation and the lattice actually written to the stream cannot drift.
  topology_plan_ = nullptr;
  const auto& plan = encoder->topology_preserving_quantization_plan();
  if (plan != nullptr && plan->position_attribute_id == attribute_id &&
      attribute->attribute_type() == GeometryAttribute::POSITION) {
    topology_plan_ = plan.get();
  }

  if (topology_plan_ != nullptr) {
    std::vector<float> min_values(attribute->num_components());
    for (int c = 0; c < attribute->num_components(); ++c) {
      min_values[c] = topology_plan_->min_value[c];
    }
    if (!attribute_quantization_transform_.SetParameters(
            topology_plan_->quantization_bits, min_values.data(),
            attribute->num_components(), topology_plan_->quantization_range)) {
      return false;
    }
    return true;
  }

  // Initialize AttributeQuantizationTransform.
  const int quantization_bits = encoder->options()->GetAttributeInt(
      attribute_id, "quantization_bits", -1);
  if (quantization_bits < 1) {
    return false;
  }
  if (encoder->options()->IsAttributeOptionSet(attribute_id,
                                               "quantization_origin") &&
      encoder->options()->IsAttributeOptionSet(attribute_id,
                                               "quantization_range")) {
    // Quantization settings are explicitly specified in the provided options.
    std::vector<float> quantization_origin(attribute->num_components());
    encoder->options()->GetAttributeVector(attribute_id, "quantization_origin",
                                           attribute->num_components(),
                                           &quantization_origin[0]);
    const float range = encoder->options()->GetAttributeFloat(
        attribute_id, "quantization_range", 1.f);
    if (!attribute_quantization_transform_.SetParameters(
            quantization_bits, quantization_origin.data(),
            attribute->num_components(), range)) {
      return false;
    }
  } else {
    // Compute quantization settings from the attribute values.
    if (!attribute_quantization_transform_.ComputeParameters(
            *attribute, quantization_bits)) {
      return false;
    }
  }
  return true;
}

bool SequentialQuantizationAttributeEncoder::
    EncodeDataNeededByPortableTransform(EncoderBuffer* out_buffer) {
  return attribute_quantization_transform_.EncodeParameters(out_buffer);
}

bool SequentialQuantizationAttributeEncoder::
    InstallTopologyPreservingExactValues(
        const std::vector<PointIndex>& point_ids) {
  if (topology_plan_ == nullptr) {
    return true;
  }
  const PointAttribute* const attribute = this->attribute();
  const std::vector<uint8_t>& exact_mask = topology_plan_->exact_value;
  // point_ids defines the portable (encoded) attribute value order. Each entry
  // |i| of the portable attribute is produced from attribute[mapped_index(
  // point_ids[i])]. Walking that order and looking up the plan mask makes the
  // exception records refer to the actual encoded values, which keeps them
  // correct after Edgebreaker reordering, for shared attribute values (several
  // points mapping to one value each contribute their own portable entry) and
  // for unused values (they have no portable entry and no record).
  std::vector<TopologyPreservingExactValue> exact_values;
  for (size_t i = 0; i < point_ids.size(); ++i) {
    const AttributeValueIndex input_value =
        attribute->mapped_index(point_ids[i]);
    if (input_value.value() >= attribute->size()) {
      return false;
    }
    if (input_value.value() >= exact_mask.size() ||
        !exact_mask[input_value.value()]) {
      continue;
    }
    TopologyPreservingExactValue record;
    record.encoded_value_index = static_cast<uint32_t>(i);
    float value[3];
    if (!attribute->ConvertValue<float>(input_value, 3, value)) {
      return false;
    }
    record.value[0] = value[0];
    record.value[1] = value[1];
    record.value[2] = value[2];
    exact_values.push_back(record);
  }
  attribute_quantization_transform_.set_num_encoded_values(
      static_cast<uint32_t>(point_ids.size()));
  attribute_quantization_transform_.SetExactValues(std::move(exact_values));
  return true;
}

bool SequentialQuantizationAttributeEncoder::PrepareValues(
    const std::vector<PointIndex>& point_ids, int num_points) {
  auto portable_attribute =
      attribute_quantization_transform_.InitTransformedAttribute(
          *attribute(), point_ids.size());
  if (!attribute_quantization_transform_.TransformAttribute(
          *(attribute()), point_ids, portable_attribute.get())) {
    return false;
  }
  if (!InstallTopologyPreservingExactValues(point_ids)) {
    return false;
  }
  SetPortableAttribute(std::move(portable_attribute));
  return true;
}

}  // namespace draco
