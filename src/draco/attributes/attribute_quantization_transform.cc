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
#include "draco/attributes/attribute_quantization_transform.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "draco/attributes/attribute_transform_type.h"
#include "draco/core/quantization_utils.h"

namespace draco {

bool AttributeQuantizationTransform::InitFromAttribute(
    const PointAttribute& attribute) {
  const AttributeTransformData* const transform_data =
      attribute.GetAttributeTransformData();
  if (!transform_data) {
    return false;  // Wrong transform type.
  }
  const bool has_extension =
      transform_data->transform_type() ==
      ATTRIBUTE_QUANTIZATION_TOPOLOGY_TRANSFORM;
  if (transform_data->transform_type() != ATTRIBUTE_QUANTIZATION_TRANSFORM &&
      !has_extension) {
    return false;  // Wrong transform type.
  }
  int32_t byte_offset = 0;
  quantization_bits_ = transform_data->GetParameterValue<int32_t>(byte_offset);
  byte_offset += 4;
  min_values_.resize(attribute.num_components());
  for (int i = 0; i < attribute.num_components(); ++i) {
    min_values_[i] = transform_data->GetParameterValue<float>(byte_offset);
    byte_offset += 4;
  }
  range_ = transform_data->GetParameterValue<float>(byte_offset);
  byte_offset += 4;
  exact_values_.clear();
  num_encoded_values_ = 0;
  extension_enabled_ = has_extension;
  if (has_extension) {
    // The exception payload starts right after the ordinary parameters.
    AttributeTransformData remainder;
    remainder.set_transform_type(ATTRIBUTE_QUANTIZATION_TOPOLOGY_TRANSFORM);
    const int64_t payload_size = transform_data->data_size() - byte_offset;
    if (payload_size < 0) {
      return false;
    }
    std::vector<uint8_t> payload(static_cast<size_t>(payload_size));
    if (payload_size > 0) {
      transform_data->GetBytes(byte_offset, payload.data(), payload_size);
      remainder.AppendBytes(payload.data(), payload_size);
    }
    uint32_t num_values = 0;
    if (!LoadTopologyPreservingQuantizationExtension(
            &remainder, &num_values, &exact_values_)) {
      exact_values_.clear();
      return false;
    }
    num_encoded_values_ = num_values;
  }
  return true;
}

// Copy parameter values into the provided AttributeTransformData instance.
void AttributeQuantizationTransform::CopyToAttributeTransformData(
    AttributeTransformData* out_data) const {
  out_data->set_transform_type(Type());
  out_data->AppendParameterValue(quantization_bits_);
  for (int i = 0; i < min_values_.size(); ++i) {
    out_data->AppendParameterValue(min_values_[i]);
  }
  out_data->AppendParameterValue(range_);
  if (extension_enabled_) {
    StoreTopologyPreservingQuantizationExtension(
        num_encoded_values_, exact_values_, out_data);
  }
}

bool AttributeQuantizationTransform::TransformAttribute(
    const PointAttribute& attribute, const std::vector<PointIndex>& point_ids,
    PointAttribute* target_attribute) {
  if (point_ids.empty()) {
    GeneratePortableAttribute(attribute, target_attribute->size(),
                              target_attribute);
  } else {
    GeneratePortableAttribute(attribute, point_ids, target_attribute->size(),
                              target_attribute);
  }
  return true;
}

void AttributeQuantizationTransform::SetExactValues(
    std::vector<TopologyPreservingExactValue> exact_values) {
  exact_values_ = std::move(exact_values);
  extension_enabled_ = true;
}

bool AttributeQuantizationTransform::ApplyExactValues(
    PointAttribute* target_attribute) const {
  if (exact_values_.empty()) {
    return true;
  }
  if (target_attribute == nullptr ||
      target_attribute->data_type() != DT_FLOAT32 ||
      target_attribute->num_components() != 3) {
    return false;
  }
  // The target attribute is indexed in the portable (encoded) attribute value
  // order, exactly the order the exception records use. In the regular decoder
  // this buffer holds one entry per encoded value; in the skip-transform path
  // the caller builds an equally sized float attribute from the returned
  // portable integer attribute. Either way a record always lands on the same
  // decoded value regardless of how points were renumbered or reordered.
  const int64_t stride = target_attribute->byte_stride();
  for (const TopologyPreservingExactValue& record : exact_values_) {
    if (record.encoded_value_index >= target_attribute->size()) {
      return false;
    }
    for (int c = 0; c < 3; ++c) {
      if (!std::isfinite(record.value[c])) {
        return false;
      }
    }
    const int64_t byte_pos =
        static_cast<int64_t>(record.encoded_value_index) * stride;
    if (byte_pos + static_cast<int64_t>(sizeof(float) * 3) >
        static_cast<int64_t>(target_attribute->buffer()->data_size())) {
      return false;
    }
    target_attribute->buffer()->Write(byte_pos, record.value,
                                      sizeof(float) * 3);
  }
  return true;
}

bool AttributeQuantizationTransform::InverseTransformAttribute(
    const PointAttribute& attribute, PointAttribute* target_attribute) {
  if (target_attribute->data_type() != DT_FLOAT32) {
    return false;
  }

  // Convert all quantized values back to floats.
  const int32_t max_quantized_value =
      (1u << static_cast<uint32_t>(quantization_bits_)) - 1;
  const int num_components = target_attribute->num_components();
  const int entry_size = sizeof(float) * num_components;
  const std::unique_ptr<float[]> att_val(new float[num_components]);
  int quant_val_id = 0;
  int out_byte_pos = 0;
  Dequantizer dequantizer;
  if (!dequantizer.Init(range_, max_quantized_value)) {
    return false;
  }
  const int32_t* const source_attribute_data = reinterpret_cast<const int32_t*>(
      attribute.GetAddress(AttributeValueIndex(0)));

  const int num_values = target_attribute->size();

  for (uint32_t i = 0; i < num_values; ++i) {
    for (int c = 0; c < num_components; ++c) {
      float value =
          dequantizer.DequantizeFloat(source_attribute_data[quant_val_id++]);
      value = value + min_values_[c];
      att_val[c] = value;
    }
    // Store the floating point value into the attribute buffer.
    target_attribute->buffer()->Write(out_byte_pos, att_val.get(), entry_size);
    out_byte_pos += entry_size;
  }
  // Overwrite the exception entries with their exact original FLOAT32 bits.
  return ApplyExactValues(target_attribute);
}

bool AttributeQuantizationTransform::IsQuantizationValid(
    int quantization_bits) {
  // Currently we allow only up to 30 bit quantization.
  return quantization_bits >= 1 && quantization_bits <= 30;
}

bool AttributeQuantizationTransform::SetParameters(int quantization_bits,
                                                   const float* min_values,
                                                   int num_components,
                                                   float range) {
  if (!IsQuantizationValid(quantization_bits)) {
    return false;
  }
  quantization_bits_ = quantization_bits;
  min_values_.assign(min_values, min_values + num_components);
  range_ = range;
  return true;
}

bool AttributeQuantizationTransform::ComputeParameters(
    const PointAttribute& attribute, const int quantization_bits) {
  if (quantization_bits_ != -1) {
    return false;  // already initialized.
  }
  if (!IsQuantizationValid(quantization_bits)) {
    return false;
  }
  quantization_bits_ = quantization_bits;

  const int num_components = attribute.num_components();
  range_ = 0.f;
  min_values_ = std::vector<float>(num_components, 0.f);
  const std::unique_ptr<float[]> max_values(new float[num_components]);
  const std::unique_ptr<float[]> att_val(new float[num_components]);
  // Compute minimum values and max value difference.
  attribute.GetValue(AttributeValueIndex(0), att_val.get());
  attribute.GetValue(AttributeValueIndex(0), min_values_.data());
  attribute.GetValue(AttributeValueIndex(0), max_values.get());

  for (AttributeValueIndex i(1); i < static_cast<uint32_t>(attribute.size());
       ++i) {
    attribute.GetValue(i, att_val.get());
    for (int c = 0; c < num_components; ++c) {
      if (std::isnan(att_val[c])) {
        return false;
      }
      if (min_values_[c] > att_val[c]) {
        min_values_[c] = att_val[c];
      }
      if (max_values[c] < att_val[c]) {
        max_values[c] = att_val[c];
      }
    }
  }
  for (int c = 0; c < num_components; ++c) {
    if (std::isnan(min_values_[c]) || std::isinf(min_values_[c]) ||
        std::isnan(max_values[c]) || std::isinf(max_values[c])) {
      return false;
    }
    const float dif = max_values[c] - min_values_[c];
    if (dif > range_) {
      range_ = dif;
    }
  }

  // Both ends are finite, but their difference can overflow, for example for
  // values from -3.4e38 to 3.4e38. The range is stored as a float, so such
  // values cannot be quantized.
  if (std::isinf(range_)) {
    return false;
  }

  // In case all values are the same, initialize the range to unit length. This
  // will ensure that all values are quantized properly to the same value.
  if (range_ == 0.f) {
    range_ = 1.f;
  }

  return true;
}

bool AttributeQuantizationTransform::EncodeParameters(
    EncoderBuffer* encoder_buffer) const {
  if (is_initialized()) {
    encoder_buffer->Encode(min_values_.data(),
                           sizeof(float) * min_values_.size());
    encoder_buffer->Encode(range_);
    encoder_buffer->Encode(static_cast<uint8_t>(quantization_bits_));
    if (extension_enabled_) {
      return EncodeTopologyPreservingQuantizationExtension(
          num_encoded_values_, exact_values_, encoder_buffer);
    }
    return true;
  }
  return false;
}

bool AttributeQuantizationTransform::DecodeParameters(
    const PointAttribute& attribute, DecoderBuffer* decoder_buffer) {
  min_values_.resize(attribute.num_components());
  if (!decoder_buffer->Decode(&min_values_[0],
                              sizeof(float) * min_values_.size())) {
    return false;
  }
  if (!decoder_buffer->Decode(&range_)) {
    return false;
  }
  uint8_t quantization_bits;
  if (!decoder_buffer->Decode(&quantization_bits)) {
    return false;
  }
  if (!IsQuantizationValid(quantization_bits)) {
    return false;
  }
  quantization_bits_ = quantization_bits;
  exact_values_.clear();
  num_encoded_values_ = 0;
  extension_enabled_ = false;
  return true;
}

bool AttributeQuantizationTransform::DecodeParametersWithExtension(
    const PointAttribute& attribute, uint32_t expected_num_values,
    DecoderBuffer* decoder_buffer) {
  if (!DecodeParameters(attribute, decoder_buffer)) {
    return false;
  }
  if (!DecodeTopologyPreservingQuantizationExtension(
          expected_num_values, &exact_values_, decoder_buffer)) {
    exact_values_.clear();
    return false;
  }
  num_encoded_values_ = expected_num_values;
  extension_enabled_ = true;
  return true;
}

void AttributeQuantizationTransform::GeneratePortableAttribute(
    const PointAttribute& attribute, int num_points,
    PointAttribute* target_attribute) const {
  DRACO_DCHECK(is_initialized());

  const int num_components = attribute.num_components();

  // Quantize all values using the order given by point_ids.
  int32_t* const portable_attribute_data = reinterpret_cast<int32_t*>(
      target_attribute->GetAddress(AttributeValueIndex(0)));
  const uint32_t max_quantized_value = (1 << (quantization_bits_)) - 1;
  Quantizer quantizer;
  quantizer.Init(range(), max_quantized_value);
  int32_t dst_index = 0;
  const std::unique_ptr<float[]> att_val(new float[num_components]);
  for (PointIndex i(0); i < num_points; ++i) {
    const AttributeValueIndex att_val_id = attribute.mapped_index(i);
    attribute.GetValue(att_val_id, att_val.get());
    for (int c = 0; c < num_components; ++c) {
      const float value = (att_val[c] - min_values()[c]);
      const int32_t q_val = quantizer.QuantizeFloat(value);
      portable_attribute_data[dst_index++] = q_val;
    }
  }
}

void AttributeQuantizationTransform::GeneratePortableAttribute(
    const PointAttribute& attribute, const std::vector<PointIndex>& point_ids,
    int num_points, PointAttribute* target_attribute) const {
  DRACO_DCHECK(is_initialized());

  const int num_components = attribute.num_components();

  // Quantize all values using the order given by point_ids.
  int32_t* const portable_attribute_data = reinterpret_cast<int32_t*>(
      target_attribute->GetAddress(AttributeValueIndex(0)));
  const uint32_t max_quantized_value = (1 << (quantization_bits_)) - 1;
  Quantizer quantizer;
  quantizer.Init(range(), max_quantized_value);
  int32_t dst_index = 0;
  const std::unique_ptr<float[]> att_val(new float[num_components]);
  for (uint32_t i = 0; i < point_ids.size(); ++i) {
    const AttributeValueIndex att_val_id = attribute.mapped_index(point_ids[i]);
    attribute.GetValue(att_val_id, att_val.get());
    for (int c = 0; c < num_components; ++c) {
      const float value = (att_val[c] - min_values()[c]);
      const int32_t q_val = quantizer.QuantizeFloat(value);
      portable_attribute_data[dst_index++] = q_val;
    }
  }
}

}  // namespace draco
