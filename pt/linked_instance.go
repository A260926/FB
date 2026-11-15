package pt

// InstanceLinks controls how a LinkedInstance participates in the scene.
// Each mask is matched bitwise against the masks carried by other instances
// (or against all bits set for shapes that are not wrapped). The zero value
// of a mask is a real zero: use DefaultInstanceLinks to get every bit set.
type InstanceLinks struct {
	ReceiveMask uint64
	EmitMask    uint64
	CastMask    uint64
	Visible     bool
}

// allLinkBits is the effective mask assigned to shapes that are not wrapped
// in a LinkedInstance, so every bitwise link test succeeds against them.
const allLinkBits = ^uint64(0)

var _ Shape = (*LinkedInstance)(nil)

// DefaultInstanceLinks returns links that behave exactly like an unwrapped
// shape: every mask has all bits enabled and the instance is visible.
func DefaultInstanceLinks() InstanceLinks {
	return InstanceLinks{allLinkBits, allLinkBits, allLinkBits, true}
}

// LinkedInstance places a shared source Shape in the world using its own
// affine transform and per-instance light links. The source geometry,
// material and textures are shared between instances and can never be
// modified through an instance. A source may be a sphere, cube, triangle,
// mesh or TransformedShape; LinkedInstances are never nested.
type LinkedInstance struct {
	source      Shape
	matrix      Matrix
	inverse     Matrix
	links       InstanceLinks
	leaf        Shape
	leafToWorld Matrix
}

// NewLinkedInstance wraps source with the given transform and links.
func NewLinkedInstance(source Shape, transform Matrix, links InstanceLinks) *LinkedInstance {
	if _, nested := source.(*LinkedInstance); nested {
		panic("pt: LinkedInstance cannot wrap another LinkedInstance")
	}
	leaf, leafToWorld := flattenSource(source)
	return &LinkedInstance{
		source:      source,
		matrix:      transform,
		inverse:     transform.Inverse(),
		links:       links,
		leaf:        leaf,
		leafToWorld: leafToWorld,
	}
}

// flattenSource returns the original leaf shape and the transform that maps
// out of its local coordinate space. A TransformedShape source contributes
// its own matrix so that a LinkedInstance of it is geometrically identical
// to the same source wrapped in a plain TransformedShape.
func flattenSource(source Shape) (Shape, Matrix) {
	if transformed, ok := source.(*TransformedShape); ok {
		return transformed.Shape, transformed.Matrix
	}
	return source, Identity()
}

// SetTransform replaces the world transform. Call Scene.Compile afterwards
// to rebuild acceleration structures and bounding boxes for the new pose.
func (s *LinkedInstance) SetTransform(matrix Matrix) {
	s.matrix = matrix
	s.inverse = matrix.Inverse()
}

// SetLinks replaces the light links. Visibility and mask changes take effect
// on the next Scene.Compile without rebuilding geometry.
func (s *LinkedInstance) SetLinks(links InstanceLinks) {
	s.links = links
}

// Source returns the shared source shape. It must not be mutated while the
// instance is in use; instances only read from it.
func (s *LinkedInstance) Source() Shape {
	return s.source
}

// Links returns the current light links.
func (s *LinkedInstance) Links() InstanceLinks {
	return s.links
}

// Compile satisfies Shape and makes sure the shared source is ready for
// intersection. Compiling a source shared by several instances is safe.
func (s *LinkedInstance) Compile() {
	s.source.Compile()
}

// BoundingBox returns the world-space bounds under the current transform,
// matching Matrix.MulBox semantics used by TransformedShape.
func (s *LinkedInstance) BoundingBox() Box {
	return s.matrix.MulBox(s.source.BoundingBox())
}

// combinedMatrix maps points from the original leaf shape's local space to
// world space, including any transform carried by a TransformedShape source.
func (s *LinkedInstance) combinedMatrix() Matrix {
	return s.matrix.Mul(s.leafToWorld)
}

// Intersect transforms the world ray into the leaf's coordinate space,
// intersects the shared leaf geometry, and rebuilds the hit in world space.
// The returned Hit and HitInfo point at the original leaf shape, while
// HitInfo.Instance identifies this instance.
func (s *LinkedInstance) Intersect(r Ray) Hit {
	matrix := s.combinedMatrix()
	inverse := matrix.Inverse()
	leafRay := inverse.MulRay(r)
	hit := s.leaf.Intersect(leafRay)
	if !hit.Ok() {
		return NoHit
	}
	shape := hit.Shape
	leafPosition := leafRay.Position(hit.T)
	leafNormal := shape.NormalAt(leafPosition)
	position := matrix.MulPosition(leafPosition)
	normal := inverse.Transpose().MulDirection(leafNormal)
	material := MaterialAt(shape, leafPosition)
	inside := false
	if leafNormal.Dot(leafRay.Direction) > 0 {
		normal = normal.Negate()
		inside = true
	}
	ray := Ray{position, normal}
	info := HitInfo{
		Shape:    shape,
		Position: position,
		Normal:   normal,
		Ray:      ray,
		Material: material,
		Inside:   inside,
		Instance: s,
	}
	hit.T = position.Sub(r.Origin).Length()
	hit.HitInfo = &info
	return hit
}

// UV maps a world-space point through the full transform chain and asks the
// original leaf shape for its texture coordinates.
func (s *LinkedInstance) UV(p Vector) Vector {
	return s.leaf.UV(s.worldToLeaf(p))
}

// NormalAt returns the world-space surface normal at a world-space point.
func (s *LinkedInstance) NormalAt(p Vector) Vector {
	leafPoint := s.worldToLeaf(p)
	leafNormal := s.leaf.NormalAt(leafPoint)
	inverse := s.combinedMatrix().Inverse()
	return inverse.Transpose().MulDirection(leafNormal)
}

// MaterialAt returns the shared source material at a world-space point.
func (s *LinkedInstance) MaterialAt(p Vector) Material {
	return s.leaf.MaterialAt(s.worldToLeaf(p))
}

// worldToLeaf maps a world point through both the instance transform and any
// transform carried by a TransformedShape source.
func (s *LinkedInstance) worldToLeaf(p Vector) Vector {
	return s.leafToWorld.Inverse().MulPosition(s.inverse.MulPosition(p))
}

// emitMask returns the EmitMask of a light shape, treating unwrapped lights
// as carrying every bit.
func emitMask(light Shape) uint64 {
	if instance, ok := light.(*LinkedInstance); ok {
		return instance.links.EmitMask
	}
	return allLinkBits
}

// receiveMask returns the ReceiveMask carried by a hit, treating unwrapped
// shapes as carrying every bit.
func receiveMask(info *HitInfo) uint64 {
	if info != nil && info.Instance != nil {
		return info.Instance.links.ReceiveMask
	}
	return allLinkBits
}
