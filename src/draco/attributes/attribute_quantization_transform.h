// Copyright 2017 The Draco Authors.
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
#ifndef DRACO_ATTRIBUTES_ATTRIBUTE_QUANTIZATION_TRANSFORM_H_
#define DRACO_ATTRIBUTES_ATTRIBUTE_QUANTIZATION_TRANSFORM_H_

#include <cstdint>
#include <vector>

#include "draco/attributes/attribute_quantization_transform_extension.h"
#include "draco/attributes/attribute_transform.h"
#include "draco/attributes/point_attribute.h"
#include "draco/core/encoder_buffer.h"

namespace draco {

// Attribute transform for quantized attributes.
//
// This transform can optionally carry the exact position exceptions of the
// mesh topology preserving quantization mode (see
// topology_preserving_quantization.h). When the exact exception list is
// non-empty, Type() reports ATTRIBUTE_QUANTIZATION_TOPOLOGY_TRANSFORM and the
// ordinary parameter block is followed by the versioned extension payload.
// Anything else is byte-for-byte identical to the legacy quantization
// transform.
class AttributeQuantizationTransform : public AttributeTransform {
 public:
  AttributeQuantizationTransform() : quantization_bits_(-1), range_(0.f) {}
  // Return attribute transform type.
  AttributeTransformType Type() const override {
    return extension_enabled_ ? ATTRIBUTE_QUANTIZATION_TOPOLOGY_TRANSFORM
                              : ATTRIBUTE_QUANTIZATION_TRANSFORM;
  }
  // Try to init transform from attribute.
  bool InitFromAttribute(const PointAttribute& attribute) override;
  // Copy parameter values into the provided AttributeTransformData instance.
  void CopyToAttributeTransformData(
      AttributeTransformData* out_data) const override;

  bool TransformAttribute(const PointAttribute& attribute,
                          const std::vector<PointIndex>& point_ids,
                          PointAttribute* target_attribute) override;

  bool InverseTransformAttribute(const PointAttribute& attribute,
                                 PointAttribute* target_attribute) override;

  bool SetParameters(int quantization_bits, const float* min_values,
                     int num_components, float range);

  bool ComputeParameters(const PointAttribute& attribute,
                         const int quantization_bits);

  // Encode relevant parameters into buffer.
  bool EncodeParameters(EncoderBuffer* encoder_buffer) const override;

  bool DecodeParameters(const PointAttribute& attribute,
                        DecoderBuffer* decoder_buffer) override;

  // Decodes the ordinary parameters and then the topology preserving
  // extension payload that follows them. |expected_num_values| is the number
  // of portable values the caller is going to decode; the extension's own
  // value count must match it exactly. Returns false for a missing/truncated/
  // inconsistent extension.
  bool DecodeParametersWithExtension(const PointAttribute& attribute,
                                     uint32_t expected_num_values,
                                     DecoderBuffer* decoder_buffer);

  int32_t quantization_bits() const { return quantization_bits_; }
  float min_value(int axis) const { return min_values_[axis]; }
  const std::vector<float>& min_values() const { return min_values_; }
  float range() const { return range_; }
  bool is_initialized() const { return quantization_bits_ != -1; }

  // Optional exact position exceptions, indexed by the portable (encoded)
  // attribute value order. The list must be sorted by encoded_value_index and
  // free of duplicates; values are original FLOAT32 coordinates. Calling this
  // always enables the extension representation, even with an empty list, so
  // "the protection is active but nothing had to be restored" is still
  // distinguishable from the legacy transform.
  void SetExactValues(std::vector<TopologyPreservingExactValue> exact_values);
  const std::vector<TopologyPreservingExactValue>& exact_values() const {
    return exact_values_;
  }
  bool has_exact_values() const { return !exact_values_.empty(); }
  // True when the transform uses the topology preserving representation.
  bool extension_enabled() const { return extension_enabled_; }
  // Number of portable values the exception indices refer to. Only meaningful
  // when the extension is enabled. |num_encoded_values_| is filled from the
  // encoder; the decoder learns it from the extension payload.
  void set_num_encoded_values(uint32_t num_encoded_values) {
    num_encoded_values_ = num_encoded_values;
  }
  uint32_t num_encoded_values() const { return num_encoded_values_; }

 protected:
  // Create portable attribute using 1:1 mapping between points in the input and
  // output attribute.
  void GeneratePortableAttribute(const PointAttribute& attribute,
                                 int num_points,
                                 PointAttribute* target_attribute) const;

  // Create portable attribute using custom mapping between input and output
  // points.
  void GeneratePortableAttribute(const PointAttribute& attribute,
                                 const std::vector<PointIndex>& point_ids,
                                 int num_points,
                                 PointAttribute* target_attribute) const;

  DataType GetTransformedDataType(
      const PointAttribute& attribute) const override {
    return DT_UINT32;
  }
  int GetTransformedNumComponents(
      const PointAttribute& attribute) const override {
    return attribute.num_components();
  }

  static bool IsQuantizationValid(int quantization_bits);

 private:
  // Restores the exact FLOAT32 bits of the exception records into the float
  // attribute produced by the regular inverse transform. |target_attribute|
  // is indexed in the portable (encoded) attribute value order that the
  // records use. Returns false for any record that does not fit.
  bool ApplyExactValues(PointAttribute* target_attribute) const;

  int32_t quantization_bits_;

  // Minimal dequantized value for each component of the attribute.
  std::vector<float> min_values_;

  // Bounds of the dequantized attribute (max delta over all components).
  float range_;

  // Optional exact position exceptions (portable value order).
  std::vector<TopologyPreservingExactValue> exact_values_;
  uint32_t num_encoded_values_ = 0;
  bool extension_enabled_ = false;
};

}  // namespace draco

#endif  // DRACO_COMPRESSION_ATTRIBUTES_ATTRIBUTE_DEQUANTIZATION_TRANSFORM_H_
