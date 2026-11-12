package hgeom.hmesh.operations;

import java.util.HashSet;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.stream.Collectors;

import hgeom.hmesh.core.RegionExtrusionBuilder;
import hgeom.hmesh.elements.HEdge;
import hgeom.hmesh.elements.HFace;
import hgeom.hmesh.elements.HMesh3D;
import hgeom.hmesh.elements.HVertex;

/**
 * Three dimensional modeling operation that lifts a region of adjacent faces
 * out of a {@link HMesh3D}.
 * <p>
 * The selected faces are moved together along a single translation vector and
 * become the top faces of the extrusion. The unselected faces stay in place.
 * Quadrilateral side faces are generated only where the region meets the rest
 * of the mesh or the outer border of the mesh: no side face is generated
 * between two selected faces and the selected faces are never kept as bottom
 * caps. {@link hgeom.hmesh.elements.HFace.Status BOUNDARY} faces, which model
 * outer borders or holes rather than physical faces, are rebuilt from the open
 * borders of the resulting mesh.
 * <p>
 * An extrusion never modifies its source mesh. It produces an independent
 * {@link HMesh3D} that supports all the usual half-edge data structure edits
 * and an {@link ExtrusionResult} exposing identity based maps connecting the
 * source elements to the result elements.
 *
 * @author Pierre B.
 */
public final class RegionExtrusion {

	/**
	 *
	 */
	private RegionExtrusion() {
	}

	/**
	 * Extrudes a region of adjacent faces of the specified mesh. The source mesh
	 * is left unchanged.
	 * <p>
	 * When the selection is empty, an independent equivalent copy of the source
	 * mesh is returned and no displacement requirement applies. Otherwise, the
	 * displacement must be finite and non zero and every half-edge located on the
	 * border of the selected region must not be parallel to the displacement, as
	 * a parallel border edge would generate a zero area side face.
	 * <p>
	 * The selected faces must all be live {@link hgeom.hmesh.elements.HFace.Status
	 * INTERIOR} faces of the source mesh. Faces coming from another mesh,
	 * discarded faces, {@link hgeom.hmesh.elements.HFace.Status BOUNDARY} faces
	 * and {@link hgeom.hmesh.elements.HFace.Status UNKNOWN} faces are rejected.
	 * A vertex at which the selected faces form several disconnected face fans
	 * is also rejected: such a butterfly vertex cannot be lifted without merging
	 * unrelated parts of the mesh.
	 *
	 * @param source   the mesh containing the faces to extrude
	 * @param selected the faces to extrude
	 * @param dx       the x component of the translation vector
	 * @param dy       the y component of the translation vector
	 * @param dz       the z component of the translation vector
	 * @return the independent extrusion result
	 * @throws IllegalArgumentException if one of the selected faces is not a live
	 *                                  INTERIOR face of the source mesh, if the
	 *                                  selected faces meet at a butterfly
	 *                                  vertex, if the non empty selection is
	 *                                  given a non finite or zero displacement,
	 *                                  or if a region border edge is parallel to
	 *                                  the displacement
	 * @throws NullPointerException     if the mesh, the selection or one of its
	 *                                  elements is {@code null}
	 */
	public static ExtrusionResult extrude(HMesh3D source, Set<HFace> selected,
			double dx, double dy, double dz) {

		Objects.requireNonNull(source, "source mesh is null");
		Objects.requireNonNull(selected, "selected face set is null");

		// The build only knows how to carry over INTERIOR faces and rebuild
		// BOUNDARY faces. A mesh containing an UNKNOWN face cannot be extruded
		source.faces().forEach(face -> {
			if (face.status() != HFace.Status.INTERIOR
					&& face.status() != HFace.Status.BOUNDARY) {

				throw new IllegalArgumentException(
						"mesh contains a face of status " + face.status()
								+ " which cannot be extruded: " + face);
			}
		});

		Set<HFace> sourceFaces = source.faces()
				.collect(Collectors.toCollection(HashSet::new));

		Set<HFace> selection = new HashSet<>();

		for (HFace face : selected) {
			Objects.requireNonNull(face, "selected face is null");

			if (!sourceFaces.contains(face)) {
				throw new IllegalArgumentException(
						"selected face is not part of the source mesh: " + face);
			}

			if (face.status() != HFace.Status.INTERIOR) {
				throw new IllegalArgumentException(
						"only INTERIOR faces can be extruded: " + face);
			}

			selection.add(face);
		}

		if (!selection.isEmpty()) {
			rejectButterflyVertices(selection);
			requireValidDisplacement(selection, source, dx, dy, dz);
		}

		Map<HFace, HFace> originalFaces = new IdentityHashMap<>();
		Map<HVertex, HVertex> baseVertices = new IdentityHashMap<>();
		Map<HVertex, HVertex> liftedVertices = new IdentityHashMap<>();
		Map<HEdge, HFace> sideFaces = new IdentityHashMap<>();

		HMesh3D result = RegionExtrusionBuilder.build(source, selection, dx, dy,
				dz, originalFaces, baseVertices, liftedVertices, sideFaces);

		return new ExtrusionResult(result, originalFaces, baseVertices,
				liftedVertices, sideFaces);
	}

	/**
	 * Rejects vertices at which the selected faces form several disconnected
	 * face fans
	 *
	 * @param selection
	 */
	private static void rejectButterflyVertices(Set<HFace> selection) {
		Map<HVertex, Set<HFace>> facesAtVertex = new IdentityHashMap<>();

		for (HFace face : selection) {
			for (HVertex vertex : face.vertices().toList()) {
				facesAtVertex.computeIfAbsent(vertex, k -> new HashSet<>())
						.add(face);
			}
		}

		for (Map.Entry<HVertex, Set<HFace>> entry : facesAtVertex.entrySet()) {
			HVertex vertex = entry.getKey();
			List<HEdge> incomingEdges = vertex.incomingEdges().toList();
			int numEdges = incomingEdges.size();
			int numFans = 0;

			// A new fan starts every time an incoming edge belonging to a
			// selected face follows one belonging to a non selected face in the
			// circular ring of half-edges pointing at the vertex
			for (int i = 0; i < numEdges; i++) {
				HFace current = incomingEdges.get(i).face();
				HFace previous = incomingEdges
						.get((i - 1 + numEdges) % numEdges).face();

				if (selection.contains(current)
						&& !selection.contains(previous)) {

					numFans++;
				}
			}

			if (numFans > 1) {
				throw new IllegalArgumentException(
						"selected faces form disconnected fans at vertex: "
								+ vertex);
			}
		}
	}

	/**
	 * Checks the displacement of a non empty selection
	 *
	 * @param selection
	 * @param source
	 * @param dx
	 * @param dy
	 * @param dz
	 */
	private static void requireValidDisplacement(Set<HFace> selection,
			HMesh3D source, double dx, double dy, double dz) {

		if (!Double.isFinite(dx) || !Double.isFinite(dy)
				|| !Double.isFinite(dz)) {

			throw new IllegalArgumentException(
					"extrusion displacement must be finite: (" + dx + ", " + dy
							+ ", " + dz + ")");
		}

		if (dx == 0.0 && dy == 0.0 && dz == 0.0) {
			throw new IllegalArgumentException(
					"extrusion displacement must not be zero");
		}

		for (HFace face : selection) {
			for (HEdge edge : face.edges().toList()) {
				if (selection.contains(edge.opposite().face())) {
					continue;
				}

				double[] tail = source.vertexXYZ(edge.tail());
				double[] head = source.vertexXYZ(edge.head());
				double ex = head[0] - tail[0];
				double ey = head[1] - tail[1];
				double ez = head[2] - tail[2];

				// Cross product of the border edge and the displacement
				double cx = ey * dz - ez * dy;
				double cy = ez * dx - ex * dz;
				double cz = ex * dy - ey * dx;

				if (cx == 0.0 && cy == 0.0 && cz == 0.0) {
					throw new IllegalArgumentException(
							"region border edge is parallel to the displacement: "
									+ edge);
				}
			}
		}
	}
}
