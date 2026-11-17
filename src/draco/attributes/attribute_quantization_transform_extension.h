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
//
// Optional extension of AttributeQuantizationTransform for the mesh topology
// preserving quantization mode.
//
// Stream representation
// ---------------------
// The ordinary quantization transform parameters are unchanged and are still
// written first:
//
//   float min_values[num_components]
//   float range
//   uint8 quantization_bits
//
// When (and only when) the mesh position protection is active, the ordinary
// parameter block is followed by the extension. The extension begins with a
// dedicated one-byte marker (0xA5) that the legacy format never writes; a
// decoder that does not know the extension would misparse the following bytes
// as the next attribute, but it can never do so silently as valid quantized
// positions because:
//   1. the stream uses mesh bitstream minor version 2.3, so an older decoder
//      rejects the stream during the normal version check, and
//   2. this decoder validates the marker, the version, the value count, the
//      record ordering/bounds and the exact coordinate finiteness, returning an
//      error for anything malformed.
//// Version 1 layout (all multi-byte values are little-endian, matching the
// rest of the Draco bit stream; there is no varint coding here):
//
//   uint8  tag                         == 0xA5
//   uint8  version                     == 1
//   uint32 num_values                  number of portable POSITION values
//   uint32 num_exact_values            number of exact records
//   repeat num_exact_values:
//     uint32 encoded_value_index       index into the *encoded attribute value
//                                      order* (NOT the input PointIndex and not
//                                      the input AttributeValueIndex of the
//                                      mesh; see below)
//     float32 x, y, z                  the original FLOAT32 bits, exactly
//
// Records must be strictly increasing in |encoded_value_index| and every index
// must be < num_values; duplicates, out-of-range indices, truncated payloads,
// non-finite coordinates and a num_values that disagrees with the number of
// portable values the decoder is about to materialize are all hard errors.
//
// The value indices refer to the portable attribute value order that the
// ordinary position quantizer writes (the traversal order for Edgebreaker, the
// input order for sequential encoding). That is the order in which the decoder
// reconstructs values, so an encoder and decoder agree without exchanging any
// extra remapping table. If several encoded entries produce the same POSITION
// value (several points that map to one input value, each with its own portable
// entry) each such entry gets its own record; input values that no point maps
// to have no portable entry and therefore no record.
//
// Version boundary
// ----------------
// A stream that carries version 1 extension bytes is not a legacy stream. Its
// mesh header minor version is bumped to
// kDracoMeshTopologyBitstreamVersionMinor (2.3, see compression_shared.h)
// while all other mesh streams keep the legacy 2.2 version. An older decoder
// that only knows 2.2 therefore rejects a 2.3 stream via its normal version
// check instead of accepting the quantized values and silently dropping the
// exact exceptions.
#ifndef DRACO_ATTRIBUTES_ATTRIBUTE_QUANTIZATION_TRANSFORM_EXTENSION_H_
#define DRACO_ATTRIBUTES_ATTRIBUTE_QUANTIZATION_TRANSFORM_EXTENSION_H_

#include <cstdint>
#include <vector>

#include "draco/attributes/attribute_transform_data.h"
#include "draco/core/decoder_buffer.h"
#include "draco/core/encoder_buffer.h"
#include "draco/core/status.h"

namespace draco {

// Identifies the topology preserving position quantization extension.
static constexpr uint8_t kTopologyPreservingQuantizationTag = 0xA5;
static constexpr uint8_t kTopologyPreservingQuantizationVersion = 1;

// A single exactly restored position value.
struct TopologyPreservingExactValue {
  // Index into the encoded (portable) attribute value order.
  uint32_t encoded_value_index = 0;
  float value[3] = {0.f, 0.f, 0.f};

  bool operator==(const TopologyPreservingExactValue& other) const {
    return encoded_value_index == other.encoded_value_index &&
           value[0] == other.value[0] && value[1] == other.value[1] &&
           value[2] == other.value[2];
  }
};

// Writes the extension tag and payload to |buffer|. The records are sorted by
// encoded value index and must not contain duplicates.
bool EncodeTopologyPreservingQuantizationExtension(
    uint32_t num_values,
    const std::vector<TopologyPreservingExactValue>& exact_values,
    EncoderBuffer* buffer);

// Decodes the extension payload that follows the ordinary quantization
// parameters. Returns false on truncation, duplicate/out-of-range records, a
// non-finite exact coordinate, or any trailing inconsistency. |num_values| is
// the number of portable values the caller expects; records with an index >=
// num_values are rejected.
bool DecodeTopologyPreservingQuantizationExtension(
    uint32_t num_values,
    std::vector<TopologyPreservingExactValue>* exact_values,
    DecoderBuffer* buffer);

// Stores the extension in an attribute transform data payload so that a
// caller who skipped the inverse transform can still recover the exact
// coordinates through
// AttributeQuantizationTransform::InverseTransformAttribute (see
// sequential_quantization_attribute_decoder.cc). Unlike the stream form, the
// payload uses fixed-width fields throughout so it can be read back with a
// simple byte walk:
//   uint8 tag, uint8 version, uint32 num_values, uint32 num_exact,
//   then records (uint32 encoded_value_index, 3 floats).
void StoreTopologyPreservingQuantizationExtension(
    uint32_t num_values,
    const std::vector<TopologyPreservingExactValue>& exact_values,
    AttributeTransformData* out_data);

// Reads back the payload written by
// StoreTopologyPreservingQuantizationExtension. |out_data| may be nullptr.
// Returns false if the payload is missing/invalid.
bool LoadTopologyPreservingQuantizationExtension(
    const AttributeTransformData* in_data, uint32_t* out_num_values,
    std::vector<TopologyPreservingExactValue>* out_exact_values);

// Number of bytes needed by the extension record area (excluding the tag,
// version and count fields).
int64_t TopologyPreservingQuantizationExtensionPayloadSize(
    const std::vector<TopologyPreservingExactValue>& exact_values);

}  // namespace draco

#endif  // DRACO_ATTRIBUTES_ATTRIBUTE_QUANTIZATION_TRANSFORM_EXTENSION_H_
