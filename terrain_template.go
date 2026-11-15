package fauxgl

// Shared indexed triangle topology for an 8x8-cell leaf block (9x9 vertices).
// The 16 entries cover every combination of stitched edges; every Terrain and
// every block reuses these slices instead of copying triangle topology.
//
// On a stitched edge the odd boundary vertices are remapped to the preceding
// even vertex on that same edge (the corner vertices stay put). Triangles
// that collapse to zero area after the remap are omitted.
var terrainIndexTemplates [16][][3]int

const terrainGridN = terrainLeafCells // 8 cells per leaf edge

func terrainVertexIndex(i, j int) int {
	return j*(terrainGridN+1) + i
}

func buildTerrainTemplates() {
	// base regular triangulation, wound so the geometric normal points up
	var base [][3]int
	for j := 0; j < terrainGridN; j++ {
		for i := 0; i < terrainGridN; i++ {
			a := terrainVertexIndex(i, j)
			b := terrainVertexIndex(i, j+1)
			c := terrainVertexIndex(i+1, j)
			d := terrainVertexIndex(i+1, j+1)
			base = append(base, [3]int{a, b, c}, [3]int{c, b, d})
		}
	}

	for stitch := 0; stitch < 16; stitch++ {
		mapping := make([]int, (terrainGridN+1)*(terrainGridN+1))
		for i := range mapping {
			mapping[i] = i
		}
		if stitch&stitchWest != 0 {
			for _, j := range []int{1, 3, 5, 7} {
				mapping[terrainVertexIndex(0, j)] = terrainVertexIndex(0, j-1)
			}
		}
		if stitch&stitchEast != 0 {
			for _, j := range []int{1, 3, 5, 7} {
				mapping[terrainVertexIndex(terrainGridN, j)] = terrainVertexIndex(terrainGridN, j-1)
			}
		}
		if stitch&stitchSouth != 0 {
			for _, i := range []int{1, 3, 5, 7} {
				mapping[terrainVertexIndex(i, 0)] = terrainVertexIndex(i-1, 0)
			}
		}
		if stitch&stitchNorth != 0 {
			for _, i := range []int{1, 3, 5, 7} {
				mapping[terrainVertexIndex(i, terrainGridN)] = terrainVertexIndex(i-1, terrainGridN)
			}
		}

		var indices [][3]int
		for _, tri := range base {
			m := [3]int{mapping[tri[0]], mapping[tri[1]], mapping[tri[2]]}
			// skip zero-area faces produced by the boundary collapse
			if m[0] == m[1] || m[1] == m[2] || m[0] == m[2] {
				continue
			}
			indices = append(indices, m)
		}
		terrainIndexTemplates[stitch] = indices
	}
}

func init() {
	buildTerrainTemplates()
}
