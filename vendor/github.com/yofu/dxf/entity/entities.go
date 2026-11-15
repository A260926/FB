// ENTITIES section
package entity

import (
	"math"

	"github.com/yofu/dxf/format"
)

// Entities represents ENTITIES section.
type Entities []Entity

// New creates a new Entities.
func New() Entities {
	e := make([]Entity, 0)
	return e
}

// Format writes ENTITIES data to formatter.
func (es Entities) Format(f format.Formatter) {
	f.WriteString(0, "SECTION")
	f.WriteString(2, "ENTITIES")
	for _, e := range es {
		e.Format(f)
	}
	f.WriteString(0, "ENDSEC")
}

// Add adds a new entity to ENTITIES section.
func (es Entities) Add(e Entity) Entities {
	es = append(es, e)
	return es
}

// SetHandle sets handles to each entity.
func (es Entities) SetHandle(v *int) {
	for _, e := range es {
		e.SetHandle(v)
	}
}

// bulge computes the center and the ray of arc between 2 vertices.
func bulge(bulge float64, start []float64, end []float64) ([]float64, float64) {
	if bulge == 0 {
		return nil, 0
	}

	verticesDistance := math.Sqrt(math.Pow(end[0]-start[0], 2) + math.Pow(end[1]-start[1], 2))

	teta := math.Atan(math.Abs(bulge)) * 4

	radius := verticesDistance / (2 * math.Sin(teta/2))

	h := math.Sqrt(radius*radius - verticesDistance*verticesDistance/4)

	solutions := [][]float64{
		{
			0.5*(end[0]-start[0]) + (h/verticesDistance)*(end[1]-start[1]) + start[0],
			0.5*(end[1]-start[1]) - (h/verticesDistance)*(end[0]-start[0]) + start[1],
		},
		{
			0.5*(end[0]-start[0]) - (h/verticesDistance)*(end[1]-start[1]) + start[0],
			0.5*(end[1]-start[1]) + (h/verticesDistance)*(end[0]-start[0]) + start[1],
		},
	}

	firstSolutionIsLeft := (end[0]-start[0])*(solutions[0][1]-start[1])-(end[1]-start[1])*(solutions[0][0]-start[0]) > 0

	if firstSolutionIsLeft && bulge > 0 {
		return solutions[1], radius
	}

	return solutions[0], radius
}
