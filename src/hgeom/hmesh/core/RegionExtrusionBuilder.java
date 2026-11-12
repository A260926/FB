package hgeom.hmesh.core;

import java.util.ArrayList;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

import hgeom.hmesh.elements.HEdge;
import hgeom.hmesh.elements.HFace;
import hgeom.hmesh.elements.HFace.Status;
import hgeom.hmesh.elements.HMesh3D;
import hgeom.hmesh.elements.HVertex;
import hgeom.hmesh.util.Loops;

/**
 * Builds the mesh resulting from a region extrusion by wiring half-edges,
 * faces and vertices directly.
 * <p>
 * The topology is derived from the topology of the source mesh, so the build
 * never relies on geometric arrow sorting. This is what makes the extrusion
 * robust when a wall edge is geometrically collinear with an edge kept in
 * place (for instance when a face of a closed box is lifted along the box
 * edge direction).
 *
 * @author Pierre B.
 */
public final class RegionExtrusionBuilder {

	/**
	 *
	 */
	private final HMesh3D source;

	/**
	 *
	 */
	private final Set<HFace> selection;

	/**
	 *
	 */
	private final double dx;

	/**
	 *
	 */
	private final double dy;

	/**
	 *
	 */
	private final double dz;

	/**
	 *
	 */
	private final HElementFactory elementFactory = new HElementFactory(true,
			false);

	/**
	 * In place copies of the source vertices
	 */
	private final Map<HVertex, HVertex> baseVertices = new IdentityHashMap<>();

	/**
	 * Lifted copies of the source vertices
	 */
	private final Map<HVertex, HVertex> liftedVertices = new IdentityHashMap<>();

	/**
	 * Half-edges indexed by their tail and head result vertices
	 */
	private final Map<HVertex, Map<HVertex, HEdge>> halfEdges = new IdentityHashMap<>();

	/**
	 * All the faces created for the result mesh
	 */
	private final List<HFace> allFaces = new ArrayList<>();

	/**
	 *
	 */
	private final Map<HFace, HFace> originalFaces = new IdentityHashMap<>();

	/**
	 *
	 */
	private final Map<HEdge, HFace> sideFaces = new IdentityHashMap<>();

	/**
	 * @param source
	 * @param selection
	 * @param dx
	 * @param dy
	 * @param dz
	 */
	private RegionExtrusionBuilder(HMesh3D source, Set<HFace> selection,
			double dx, double dy, double dz) {

		this.source = source;
		this.selection = selection;
		this.dx = dx;
		this.dy = dy;
		this.dz = dz;
	}

	/**
	 * Builds the extruded mesh. The four given maps are filled with the
	 * identity based connections between the source elements and the result
	 * elements
	 *
	 * @param source         the mesh containing the selected faces
	 * @param selection      the selected INTERIOR faces
	 * @param dx             the x component of the translation vector
	 * @param dy             the y component of the translation vector
	 * @param dz             the z component of the translation vector
	 * @param originalFaces  filled with the source face to result face map
	 * @param baseVertices   filled with the in place vertex map
	 * @param liftedVertices filled with the lifted vertex map
	 * @param sideFaces      filled with the border edge to side face map
	 * @return the newly built, fully editable mesh
	 */
	public static HMesh3D build(HMesh3D source, Set<HFace> selection,
			double dx, double dy, double dz,
			Map<HFace, HFace> originalFaces,
			Map<HVertex, HVertex> baseVertices,
			Map<HVertex, HVertex> liftedVertices,
			Map<HEdge, HFace> sideFaces) {

		RegionExtrusionBuilder builder = new RegionExtrusionBuilder(source,
				selection, dx, dy, dz);

		builder.buildInteriorFaces();
		builder.buildBoundaryFaces();

		HMesh3D result = new HMesh3DImpl(builder.allFaces,
				builder.elementFactory);

		builder.copyCoordinates(result);
		originalFaces.putAll(builder.originalFaces);
		baseVertices.putAll(builder.baseVertices);
		liftedVertices.putAll(builder.liftedVertices);
		sideFaces.putAll(builder.sideFaces);
		return result;
	}

	/**
	 * Builds the copies of the source INTERIOR faces together with the side
	 * faces generated on the region border
	 */
	private void buildInteriorFaces() {
		for (HFace sourceFace : source.faces().toList()) {
			if (sourceFace.status() != Status.INTERIOR) {
				continue;
			}

			boolean lifted = selection.contains(sourceFace);
			List<HVertex> resultCycle = sourceFace.vertices()
					.map(vertex -> lifted ? liftedVertex(vertex)
							: baseVertex(vertex))
					.toList();

			List<HEdge> cycle = new ArrayList<>(resultCycle.size());
			Loops.forEachPair(resultCycle,
					(tail, head) -> cycle.add(halfEdge(tail, head)));

			HFace resultFace = createFace(cycle, Status.INTERIOR);
			originalFaces.put(sourceFace, resultFace);
		}

		// Side faces on the oriented border of the selected region
		for (HFace sourceFace : selection) {
			for (HEdge borderEdge : sourceFace.edges().toList()) {
				if (selection.contains(borderEdge.opposite().face())) {
					continue;
				}

				createSideFace(borderEdge);
			}
		}
	}

	/**
	 * Creates the quadrilateral side face associated with a selected region
	 * border edge
	 *
	 * @param borderEdge the selected face half-edge pointing from its tail to
	 *                   its head
	 */
	private void createSideFace(HEdge borderEdge) {
		HVertex baseTail = baseVertex(borderEdge.tail());
		HVertex baseHead = baseVertex(borderEdge.head());
		HVertex liftedHead = liftedVertex(borderEdge.head());
		HVertex liftedTail = liftedVertex(borderEdge.tail());

		// The base rim keeps the direction of the selected border edge and the
		// lifted rim runs in reverse, so the side face is opposite to both the
		// neighboring unselected face and the lifted top face
		List<HVertex> resultCycle = List.of(baseTail, baseHead, liftedHead,
				liftedTail);

		List<HEdge> cycle = new ArrayList<>(4);
		Loops.forEachPair(resultCycle,
				(tail, head) -> cycle.add(halfEdge(tail, head)));

		HFace sideFace = createFace(cycle, Status.INTERIOR);
		sideFaces.put(borderEdge, sideFace);
	}

	/**
	 * Creates the BOUNDARY faces of the result from the BOUNDARY faces of the
	 * source. Every open border of the result is the in place image of an open
	 * border of the source: an outer border or a hole stays open below the
	 * lifted region and the base vertices involved are all created by the
	 * interior and side faces. Reusing the source border loops also gives the
	 * correct orientation when several borders meet at one vertex
	 */
	private void buildBoundaryFaces() {
		for (HFace sourceBoundary : source.faces().toList()) {
			if (sourceBoundary.status() != Status.BOUNDARY) {
				continue;
			}

			List<HEdge> cycle = new ArrayList<>();

			for (HEdge sourceEdge : sourceBoundary.edges().toList()) {
				HVertex tail = baseVertex(sourceEdge.tail());
				HVertex head = baseVertex(sourceEdge.head());
				HEdge resultEdge = halfEdges.get(tail).get(head);

				if (resultEdge.face() != null) {
					throw new IllegalStateException(
							"region extrusion closed a source open border");
				}

				cycle.add(resultEdge);
			}

			createFace(cycle, Status.BOUNDARY);
		}
	}

	/**
	 * Gets or creates the in place copy of a source vertex
	 *
	 * @param sourceVertex
	 * @return the in place result vertex
	 */
	private HVertex baseVertex(HVertex sourceVertex) {
		return baseVertices.computeIfAbsent(sourceVertex,
				vertex -> elementFactory.createVertex());
	}

	/**
	 * Gets or creates the lifted copy of a source vertex. A vertex shared by
	 * several selected faces is therefore lifted only once
	 *
	 * @param sourceVertex
	 * @return the lifted result vertex
	 */
	private HVertex liftedVertex(HVertex sourceVertex) {
		return liftedVertices.computeIfAbsent(sourceVertex,
				vertex -> elementFactory.createVertex());
	}

	/**
	 * Gets or creates the half-edge going from a tail result vertex to a head
	 * result vertex. The opposite half-edge is created at the same time and
	 * reused if the pair of vertices is encountered again
	 *
	 * @param tail
	 * @param head
	 * @return the half-edge from tail to head
	 */
	private HEdge halfEdge(HVertex tail, HVertex head) {
		Map<HVertex, HEdge> outgoing = halfEdges.get(tail);

		if (outgoing != null) {
			HEdge existing = outgoing.get(head);

			if (existing != null) {
				return existing;
			}
		}

		HEdge edge = elementFactory.createEdge(head);
		HEdge opposite = elementFactory.createEdge(tail);
		HEdgeImpl.linkAsOpposites(edge, opposite);
		HVertexImpl.setEdgeIfAbsent(head, edge);
		HVertexImpl.setEdgeIfAbsent(tail, opposite);

		halfEdges.computeIfAbsent(tail, k -> new IdentityHashMap<>()).put(head,
				edge);
		halfEdges.computeIfAbsent(head, k -> new IdentityHashMap<>()).put(tail,
				opposite);
		return edge;
	}

	/**
	 * Links the given half-edges as a closed face cycle and creates the face.
	 * The {@link HFaceImpl} constructor assigns every half-edge of the cycle to
	 * the created face
	 *
	 * @param cycle  the half-edges of the face, in winding order
	 * @param status the status of the face to create
	 * @return the created face
	 */
	private HFace createFace(List<HEdge> cycle, Status status) {
		Loops.forEachPair(cycle, HEdgeImpl::link);
		HFace face = elementFactory.createFace(cycle.get(0), status, false);
		allFaces.add(face);
		return face;
	}

	/**
	 * Copies the source coordinates into the in place vertices and writes the
	 * translated coordinates into the lifted vertices. The result gets its own
	 * coordinate storage
	 *
	 * @param result
	 */
	private void copyCoordinates(HMesh3D result) {
		for (Map.Entry<HVertex, HVertex> entry : baseVertices.entrySet()) {
			result.setVertexXYZ(entry.getValue(),
					source.vertexXYZ(entry.getKey()));
		}

		for (Map.Entry<HVertex, HVertex> entry : liftedVertices.entrySet()) {
			double[] xyz = source.vertexXYZ(entry.getKey());
			result.setVertexXYZ(entry.getValue(), xyz[0] + dx, xyz[1] + dy,
					xyz[2] + dz);
		}
	}
}
