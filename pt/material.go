package pt

type Material struct {
	Color          Color
	Texture        Texture
	NormalTexture  Texture
	BumpTexture    Texture
	GlossTexture   Texture
	BumpMultiplier float64
	Emittance      float64
	Index          float64 // refractive index
	Gloss          float64 // reflection cone angle in radians
	Tint           float64 // specular and refractive tinting
	Reflectivity   float64 // metallic reflection
	Transparent    bool
	Absorption     Color // per-world-unit extinction coefficients (Beer-Lambert)
}

func DiffuseMaterial(color Color) Material {
	return Material{color, nil, nil, nil, nil, 1, 0, 1, 0, 0, -1, false, Color{}}
}

func SpecularMaterial(color Color, index float64) Material {
	return Material{color, nil, nil, nil, nil, 1, 0, index, 0, 0, -1, false, Color{}}
}

func GlossyMaterial(color Color, index, gloss float64) Material {
	return Material{color, nil, nil, nil, nil, 1, 0, index, gloss, 0, -1, false, Color{}}
}

func ClearMaterial(index, gloss float64) Material {
	return Material{Black, nil, nil, nil, nil, 1, 0, index, gloss, 0, -1, true, Color{}}
}

func TransparentMaterial(color Color, index, gloss, tint float64) Material {
	return Material{color, nil, nil, nil, nil, 1, 0, index, gloss, tint, -1, true, Color{}}
}

func MetallicMaterial(color Color, gloss, tint float64) Material {
	return Material{color, nil, nil, nil, nil, 1, 0, 1, gloss, tint, 1, false, Color{}}
}

func LightMaterial(color Color, emittance float64) Material {
	return Material{color, nil, nil, nil, nil, 1, emittance, 1, 0, 0, -1, false, Color{}}
}

func MaterialAt(shape Shape, point Vector) Material {
	material := shape.MaterialAt(point)
	uv := shape.UV(point)
	if material.Texture != nil {
		material.Color = material.Texture.Sample(uv.X, uv.Y)
	}
	if material.GlossTexture != nil {
		c := material.GlossTexture.Sample(uv.X, uv.Y)
		material.Gloss = (c.R + c.G + c.B) / 3
	}
	return material
}
