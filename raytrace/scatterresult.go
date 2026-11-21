package raytrace

type ScatterResult struct {
	isScattered bool
	specularRay *Ray
	attenuation ColorVector
	pdf         Pdf
}

func NewScatterResult(isScattered bool, attenuation ColorVector, specularRay *Ray, pdf Pdf) *ScatterResult {
	return &ScatterResult{isScattered, specularRay, attenuation, pdf}
}

func NewFalseScatterResult() *ScatterResult {
	return NewScatterResult(false, NewColorVector(0.0, 0.0, 0.0), nil, nil)
}

func (r *ScatterResult) IsScattered() bool {
	return r.isScattered
}

func (r *ScatterResult) IsSpecular() bool {
	return r.SpecularRay() != nil
}

func (r *ScatterResult) SpecularRay() *Ray {
	return r.specularRay
}

func (r *ScatterResult) Attenuation() ColorVector {
	return r.attenuation
}

func (r *ScatterResult) Pdf() Pdf {
	return r.pdf
}
