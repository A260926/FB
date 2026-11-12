package hgeom.hmesh.operations;

import java.util.Collections;
import java.util.Map;
import java.util.Objects;

import hgeom.hmesh.elements.HEdge;
import hgeom.hmesh.elements.HFace;
import hgeom.hmesh.elements.HMesh3D;
import hgeom.hmesh.elements.HVertex;

/**
 * Immutable result of a {@link RegionExtrusion#extrude region extrusion}
 * performed on a {@link HMesh3D}.
 * <p>
 * A result contains the newly built, fully editable {@link HMesh3D} together
 * with four read-only maps that connect the elements of the source mesh to the
 * elements of the result. The maps identify elements by their object identity;
 * they never rely on coordinate lookups.
 * <p>
 * The result mesh does not share any mutable coordinate storage with its source
 * and the maps only reference elements that are actually part of the two
 * meshes. Editing the source mesh after the extrusion, or calling
 * {@link HMesh3D#trim()} on the result, therefore never makes a map point to an
 * element belonging to another mesh.
 *
 * @author Pierre B.
 */
public final class ExtrusionResult {

	/**
	 *
	 */
	private final HMesh3D mesh;

	/**
	 *
	 */
	private final Map<HFace, HFace> originalFaces;

	/**
	 *
	 */
	private final Map<HVertex, HVertex> baseVertices;

	/**
	 *
	 */
	private final Map<HVertex, HVertex> liftedVertices;

	/**
	 *
	 */
	private final Map<HEdge, HFace> sideFaces;

	/**
	 * Interne a {@link RegionExtrusion}
	 *
	 * @param mesh
	 * @param originalFaces
	 * @param baseVertices
	 * @param liftedVertices
	 * @param sideFaces
	 */
	ExtrusionResult(HMesh3D mesh, Map<HFace, HFace> originalFaces,
			Map<HVertex, HVertex> baseVertices,
			Map<HVertex, HVertex> liftedVertices,
			Map<HEdge, HFace> sideFaces) {

		this.mesh = Objects.requireNonNull(mesh);
		this.originalFaces = Collections.unmodifiableMap(originalFaces);
		this.baseVertices = Collections.unmodifiableMap(baseVertices);
		this.liftedVertices = Collections.unmodifiableMap(liftedVertices);
		this.sideFaces = Collections.unmodifiableMap(sideFaces);
	}

	/**
	 * Gets the mesh produced by the extrusion. It is fully independent from the
	 * source mesh and supports all the usual half-edge data structure edits such
	 * as {@link HMesh3D#splitEdge} or {@link HMesh3D#splitFace}
	 *
	 * @return the extruded mesh. Never {@code null}
	 */
	public HMesh3D mesh() {
		return mesh;
	}

	/**
	 * Gets the map connecting each {@link hgeom.hmesh.elements.HFace.Status
	 * INTERIOR} face of the source mesh to the face that takes its place in the
	 * result. An unselected face is mapped to its in-place copy while a selected
	 * face is mapped to its lifted top face
	 *
	 * @return the read-only face map
	 */
	public Map<HFace, HFace> originalFaces() {
		return originalFaces;
	}

	/**
	 * Gets the map connecting each in-place vertex of the source mesh that is
	 * still referenced by the result to its copy. Boundary vertices that are
	 * only used by lifted faces do not appear here; source vertices that are no
	 * longer referenced are not kept
	 *
	 * @return the read-only in-place vertex map
	 */
	public Map<HVertex, HVertex> baseVertices() {
		return baseVertices;
	}

	/**
	 * Gets the map connecting each vertex of the selected region to the
	 * translated vertex created for the lifted top faces. A vertex shared by
	 * several selected faces is lifted only once
	 *
	 * @return the read-only lifted vertex map
	 */
	public Map<HVertex, HVertex> liftedVertices() {
		return liftedVertices;
	}

	/**
	 * Gets the map connecting each oriented boundary half-edge of the selected
	 * region to the quadrilateral side face generated for it. The key is the
	 * half-edge that belongs to a selected face; it points from a base vertex to
	 * the matching lifted vertex
	 *
	 * @return the read-only side face map
	 */
	public Map<HEdge, HFace> sideFaces() {
		return sideFaces;
	}
}
