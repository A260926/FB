package pt

import "math"

// mediumRegion is one absorbing volume that a path is currently inside.
// id is the concrete shape instance: two transforms referencing the same
// original Sphere produce distinct regions and never clear one another.
type mediumRegion struct {
	id    Shape
	sigma Color
}

// mediumState is the set of absorbing regions active along a path segment.
// It is passed by value through the recursive sampler so each reflection or
// refraction branch owns an independent set; a Sample call never inherits
// media from a previous call.
type mediumState struct {
	regions []mediumRegion
}

func (m mediumState) copy() mediumState {
	if len(m.regions) == 0 {
		return mediumState{}
	}
	regions := make([]mediumRegion, len(m.regions))
	copy(regions, m.regions)
	return mediumState{regions}
}

// sigma returns the combined extinction of all active regions; overlapping
// absorbers therefore add their coefficients per channel.
func (m mediumState) sigma() Color {
	var s Color
	for _, region := range m.regions {
		s = s.Add(region.sigma)
	}
	return s
}

// transmittance is the Beer-Lambert attenuation over a world-space distance.
// An empty set (the zero-absorption case) returns White without any math and
// without touching the random stream.
func (m mediumState) transmittance(distance float64) Color {
	if len(m.regions) == 0 || distance <= 0 {
		return White
	}
	s := m.sigma()
	return Color{
		beer(s.R, distance),
		beer(s.G, distance),
		beer(s.B, distance),
	}
}

// envTransmittance is the attenuation along a ray that escapes all geometry.
// Bounded regions should all have been exited first; a still-active channel
// with positive extinction goes to zero over an infinite remaining path.
func (m mediumState) envTransmittance() Color {
	if len(m.regions) == 0 {
		return White
	}
	s := m.sigma()
	return Color{beer(s.R, math.Inf(1)), beer(s.G, math.Inf(1)), beer(s.B, math.Inf(1))}
}

func beer(sigma, distance float64) float64 {
	if sigma == 0 {
		return 1
	}
	return math.Exp(-sigma * distance)
}

// enter returns a state with the region added. Zero-extinction regions are
// skipped so fully transparent scenes keep allocating nothing.
func (m mediumState) enter(id Shape, sigma Color) mediumState {
	if sigma.R == 0 && sigma.G == 0 && sigma.B == 0 {
		return m
	}
	n := m.copy()
	n.regions = append(n.regions, mediumRegion{id, sigma})
	return n
}

// exit returns a state with the given region removed. Removing one instance
// leaves every other active region (including a twin instance of the same
// original sphere) untouched.
func (m mediumState) exit(id Shape) mediumState {
	if len(m.regions) == 0 {
		return m
	}
	n := m.copy()
	for i := len(n.regions) - 1; i >= 0; i-- {
		if n.regions[i].id == id {
			n.regions = append(n.regions[:i], n.regions[i+1:]...)
			break
		}
	}
	return n
}

// isSphereRegion reports whether a region identity is a Sphere wrapped in any
// number of TransformedShape layers. Volume absorption is only tracked for
// these; other geometry leaves the medium set alone.
func isSphereRegion(s Shape) bool {
	for {
		switch t := s.(type) {
		case *Sphere:
			return true
		case *TransformedShape:
			s = t.Shape
		default:
			return false
		}
	}
}
