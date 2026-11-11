package pt

import (
	"fmt"
	"math"
	"math/rand"
	"testing"
)

func seedNear(t *testing.T, got, want float64) {
	t.Helper()
	if math.Abs(got-want) > 1e-9 {
		t.Fatalf("got %.12g want %.12g", got, want)
	}
}

func TestSeedPrimitivePaths(t *testing.T) {
	m := DiffuseMaterial(Color{0.2, 0.4, 0.6})
	sphere := NewSphere(V(0, 0, 0), 1, m)
	r := Ray{Origin: V(0, 0, -3), Direction: V(0, 0, 1)}
	h := sphere.Intersect(r)
	if !h.Ok() {
		t.Fatal("sphere entry missed")
	}
	seedNear(t, h.T, 2)
	h = sphere.Intersect(Ray{Origin: V(0, 0, 0), Direction: V(1, 0, 0)})
	if !h.Ok() {
		t.Fatal("sphere exit missed")
	}
	seedNear(t, h.T, 1)
	cube := NewCube(V(-1, -1, -1), V(1, 1, 1), m)
	h = cube.Intersect(r)
	if !h.Ok() {
		t.Fatal("cube entry missed")
	}
	seedNear(t, h.T, 2)
	ellipsoid := NewTransformedShape(sphere, Scale(V(2, 1, 0.5)).Translate(V(0, 0, 5)))
	r = Ray{Origin: V(0, 0, 0), Direction: V(0, 0, 1)}
	h = ellipsoid.Intersect(r)
	if !h.Ok() {
		t.Fatal("transformed sphere missed")
	}
	seedNear(t, h.T, 4.5)
	info := h.Info(r)
	seedNear(t, info.Position.Z, 4.5)
	seedNear(t, info.Normal.Z, -1)
	if info.Material.Color != m.Color {
		t.Fatal("material lost")
	}
	mesh := cube.Mesh()
	mesh.Compile()
	h = mesh.Intersect(Ray{Origin: V(0.2, 0.3, -3), Direction: V(0, 0, 1)})
	if !h.Ok() {
		t.Fatal("compiled mesh missed")
	}
	seedNear(t, h.T, 2)
	fmt.Println("STATIC_PATHS sphere_entry=2 sphere_exit=1 cube_entry=2 ellipsoid_entry=4.5 mesh_entry=2")
}

func TestSeedSceneSampler(t *testing.T) {
	scene := &Scene{Color: Color{0.2, 0.4, 0.6}}
	scene.Add(NewSphere(V(0, 0, 0), 1, LightMaterial(Color{0.5, 0.25, 1}, 2)))
	scene.Compile()
	s := NewSampler(1, 0)
	s.DirectLighting = false
	r := Ray{Origin: V(0, 0, -3), Direction: V(0, 0, 1)}
	c := s.Sample(scene, r, rand.New(rand.NewSource(17)))
	seedNear(t, c.R, 1)
	seedNear(t, c.G, 0.5)
	seedNear(t, c.B, 2)
	c = s.Sample(scene, Ray{Origin: V(0, 0, -3), Direction: V(0, 1, 0)}, rand.New(rand.NewSource(17)))
	if c != scene.Color {
		t.Fatal("environment changed")
	}
	fmt.Println("STATIC_SAMPLER emission=(1,0.5,2) environment=(0.2,0.4,0.6)")
}

func TestSeedSeparateObjects(t *testing.T) {
	scene := &Scene{}
	m := DiffuseMaterial(White)
	scene.Add(NewSphere(V(-0.5, 0, 0), 1, m))
	scene.Add(NewSphere(V(0.5, 0, 0), 1, m))
	scene.Compile()
	h := scene.Intersect(Ray{Origin: V(0, 0, 0), Direction: V(1, 0, 0)})
	if !h.Ok() {
		t.Fatal("existing object surface missed")
	}
	seedNear(t, h.T, 0.5)
	// This is the existing scene behavior, not a Boolean implementation.
	fmt.Println("SEPARATE_OBJECTS first_surface=0.5 union_boundary_required=1.5")
}

func TestSeedNestedHitInfo(t *testing.T) {
	m := DiffuseMaterial(White)
	inner := NewTransformedShape(NewSphere(V(0, 0, 0), 1, m), Scale(V(2, 1, 1)).Translate(V(0, 0, 5)))
	outer := NewTransformedShape(inner, Identity())
	r := Ray{Origin: V(0.5, 0, 0), Direction: V(0, 0, 1)}
	hi, ho := inner.Intersect(r), outer.Intersect(r)
	if !hi.Ok() || !ho.Ok() {
		t.Fatal("static transformed paths missed")
	}
	i, o := hi.Info(r), ho.Info(r)
	seedNear(t, hi.T, ho.T)
	// The Identity wrapper must preserve the child's resolved hit
	// information instead of reinterpreting the leaf primitive.
	if i.Inside || o.Inside {
		t.Fatal("outside hit was misreported as inside through a nested transform")
	}
	if i.Normal.X <= 0 || o.Normal.X <= 0 {
		t.Fatal("shading normal sign lost through nested transform")
	}
	seedNear(t, o.Normal.X, i.Normal.X)
	seedNear(t, o.Normal.Y, i.Normal.Y)
	seedNear(t, o.Normal.Z, i.Normal.Z)
	if o.Material.Color != i.Material.Color {
		t.Fatal("material lost through nested transform")
	}
	fmt.Printf("NESTED_HITINFO inner_inside=%t outer_inside=%t inner_nx=%.9f outer_nx=%.9f\n", i.Inside, o.Inside, i.Normal.X, o.Normal.X)
}
