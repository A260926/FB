package scene

import (
	"github.com/peterstace/grayt/colour"
	"github.com/peterstace/grayt/xmath"
)

type Scene struct {
	Camera  Camera
	Objects []Object
}

// LiveScene is a scene description that can be submitted to a running
// renderer. Geometry is organised into prototypes, each of which is compiled
// once per submission. Placements instance a prototype at a transform, so
// many copies can share the same compiled primitives.
type LiveScene struct {
	Camera     Camera      `json:"camera"`
	Prototypes []Prototype `json:"prototypes"`
	Placements []Placement `json:"placements"`
}

// Prototype is a named bundle of geometry that placements can instance.
type Prototype struct {
	Objects []Object `json:"objects"`
}

// Placement instances a prototype into the world. Transform is a row major
// 3x4 affine matrix (3 rows of 4 columns, the last column being the
// translation); the identity matrix must be written out explicitly. Hidden
// placements are not intersected.
type Placement struct {
	Prototype int         `json:"prototype"`
	Transform [12]float64 `json:"transform"`
	Visible   bool        `json:"visible"`
}

type Camera struct {
	Location             xmath.Vector `json:"location"`
	LookingAt            xmath.Vector `json:"looking_at"`
	UpDirection          xmath.Vector `json:"up_direction"`
	FieldOfViewInRadians float64      `json:"field_of_view_in_radians"`
	FocalLength          float64      `json:"focal_length"`
	FocalRatio           float64      `json:"focal_ratio"`
	AspectWide           int          `json:"aspect_wide"`
	AspectHigh           int          `json:"aspect_high"`
}

type Object struct {
	Surface  Surface  `json:"surface"`
	Material Material `json:"material"`
}

type Surface struct {
	Triangles     []Triangle     `json:"triangles,omitempty"`
	AlignedBoxes  []AlignedBox   `json:"aligned_boxes,omitempty"`
	Spheres       []Sphere       `json:"spheres,omitempty"`
	AlignXSquares []AlignXSquare `json:"align_x_squares,omitempty"`
	AlignYSquares []AlignYSquare `json:"align_y_squares,omitempty"`
	AlignZSquares []AlignZSquare `json:"align_z_squares,omitempty"`
	Discs         []Disc         `json:"discs,omitempty"`
	Pipes         []Pipe         `json:"pipes,omitempty"`
}

type Material struct {
	Colour    colour.Colour `json:"colour"`
	Emittance float64       `json:"emittance"`
	Mirror    bool          `json:"mirror"`
}

type Triangle struct {
	A xmath.Vector `json:"a"`
	B xmath.Vector `json:"b"`
	C xmath.Vector `json:"c"`
}

type AlignedBox struct {
	CornerA xmath.Vector `json:"a"`
	CornerB xmath.Vector `json:"b"`
}

type Sphere struct {
	Center xmath.Vector `json:"center"`
	Radius float64      `json:"radius"`
}

type AlignXSquare struct {
	X  float64 `json:"x"`
	Y1 float64 `json:"y_1"`
	Y2 float64 `json:"y_2"`
	Z1 float64 `json:"z_1"`
	Z2 float64 `json:"z_2"`
}

type AlignYSquare struct {
	X1 float64 `json:"x_1"`
	X2 float64 `json:"x_2"`
	Y  float64 `json:"y"`
	Z1 float64 `json:"z_1"`
	Z2 float64 `json:"z_2"`
}

type AlignZSquare struct {
	X1 float64 `json:"x_1"`
	X2 float64 `json:"x_2"`
	Y1 float64 `json:"y_1"`
	Y2 float64 `json:"y_2"`
	Z  float64 `json:"z"`
}

type Disc struct {
	Center xmath.Vector `json:"center"`
	Radius float64      `json:"radius"`
	// TODO: Should just accept any norm.
	UnitNorm xmath.Vector `json:"unit_norm"`
}

type Pipe struct {
	EndpointA xmath.Vector `json:"endpoint_a"`
	EndpointB xmath.Vector `json:"endpoint_b"`
	Radius    float64      `json:"radius"`
}
