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
#ifndef DRACO_ATTRIBUTES_ATTRIBUTE_TRANSFORM_TYPE_H_
#define DRACO_ATTRIBUTES_ATTRIBUTE_TRANSFORM_TYPE_H_

namespace draco {

// List of all currently supported attribute transforms.
enum AttributeTransformType {
  ATTRIBUTE_INVALID_TRANSFORM = -1,
  ATTRIBUTE_NO_TRANSFORM = 0,
  ATTRIBUTE_QUANTIZATION_TRANSFORM = 1,
  ATTRIBUTE_OCTAHEDRON_TRANSFORM = 2,
  // Quantization transform that additionally carries the exact position
  // exceptions of the mesh topology preserving quantization mode. The ordinary
  // quantization parameters come first and the exception payload is appended
  // (see attribute_quantization_transform_extension.h). A stream that uses this
  // transform is not a legacy stream: its mesh bitstream version is bumped so
  // that older decoders reject it instead of silently returning quantized
  // positions.
  ATTRIBUTE_QUANTIZATION_TOPOLOGY_TRANSFORM = 3,
};

}  // namespace draco

#endif  // DRACO_ATTRIBUTES_ATTRIBUTE_TRANSFORM_TYPE_H_
