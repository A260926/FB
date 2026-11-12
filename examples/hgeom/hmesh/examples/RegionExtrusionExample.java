package hgeom.hmesh.examples;

import java.util.HashSet;
import java.util.Set;

import hgeom.hmesh.elements.Coord3DSource;
import hgeom.hmesh.elements.FaceSource;
import hgeom.hmesh.elements.HFace;
import hgeom.hmesh.elements.HMesh3D;
import hgeom.hmesh.elements.HVertex;
import hgeom.hmesh.operations.ExtrusionResult;
import hgeom.hmesh.operations.RegionExtrusion;

/**
 * Example of a region extrusion on a 3D half-edge data structure
 *
 * @author Pierre B.
 */
public final class RegionExtrusionExample {

	/**
	 *
	 */
	private RegionExtrusionExample() {
	}

	/**
	 * Lifts two adjacent square faces one unit along the z axis. The source mesh
	 * is left unchanged and the returned mesh contains the lifted top faces, the
	 * in place faces and the side faces connecting them
	 *
	 * @param args unused
	 */
	public static void main(String[] args) {

		// Two adjacent squares lying in the z = 0 plane
		int[][] faces = { { 0, 1, 4, 3 }, { 1, 2, 5, 4 } };

		double[][] coords = { { 0, 0, 0 }, { 1, 0, 0 }, { 2, 0, 0 },
				{ 0, 1, 0 }, { 1, 1, 0 }, { 2, 1, 0 } };

		HMesh3D source = new FaceSource(faces).toHMesh(new Coord3DSource(coords))
				.orElseThrow(IllegalStateException::new);

		// Selects the two INTERIOR faces of the mesh
		Set<HFace> selected = new HashSet<>();
		source.faces().filter(face -> face.status() == HFace.Status.INTERIOR)
				.forEach(selected::add);

		// Builds an independent extruded mesh. The source mesh is not modified
		ExtrusionResult result = RegionExtrusion.extrude(source, selected, 0, 0,
				1);

		HMesh3D extruded = result.mesh();

		// The result maps identify elements by their identity rather than by
		// their coordinates. They make it possible to transfer user data from
		// the source mesh to the extruded mesh, here a simple vertex label
		result.liftedVertices().forEach((sourceVertex, liftedVertex) -> {
			HVertex inPlaceVertex = result.baseVertices().get(sourceVertex);
			// ... sourceVertex, inPlaceVertex and liftedVertex are the three
			// representations of the same logical vertex
		});

		System.out.println("interior faces after extrusion: " + extruded.faces()
				.filter(face -> face.status() == HFace.Status.INTERIOR).count());
	}
}
