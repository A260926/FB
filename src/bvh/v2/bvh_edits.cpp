// Explicit instantiation of the editable BVH implementation for the type exposed by the C API.
// The editable-mode entry points are part of `Bvh` and are only instantiated on demand for other
// node types, leaving their behavior and ABI unchanged.
#include "bvh/v2/bvh.h"
#include "bvh/v2/node.h"

namespace bvh::v2 {

template struct Bvh<Node<float, 3>>;

} // namespace bvh::v2
