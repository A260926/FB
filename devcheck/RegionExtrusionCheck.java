import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;

import hgeom.hmesh.data.HData;
import hgeom.hmesh.data.HDData;
import hgeom.hmesh.elements.Coord3DSource;
import hgeom.hmesh.elements.EdgeSource;
import hgeom.hmesh.elements.FaceSource;
import hgeom.hmesh.elements.HEdge;
import hgeom.hmesh.elements.HFace;
import hgeom.hmesh.elements.HMesh3D;
import hgeom.hmesh.elements.HVertex;
import hgeom.hmesh.operations.ExtrusionResult;
import hgeom.hmesh.operations.RegionExtrusion;

/**
 * Devcheck driver for {@link RegionExtrusion}. Compiles and runs without any
 * external test framework. Any failed check throws an {@link AssertionError},
 * which makes the shell script return a non zero status.
 */
public final class RegionExtrusionCheck {

	private RegionExtrusionCheck() {
	}

	static void check(boolean condition, String message) {
		if (!condition) {
			throw new AssertionError(message);
		}
	}

	static HMesh3D mesh(int[][] faces, double[][] coords) {
		return new FaceSource(faces).toHMesh(new Coord3DSource(coords))
				.orElseThrow(() -> new AssertionError("mesh build failed"));
	}

	static Set<HFace> select(HMesh3D mesh, int... faceIndexes) {
		Set<HFace> selected = new HashSet<>();
		List<HFace> interiors = mesh.faces()
				.filter(f -> f.status() == HFace.Status.INTERIOR)
				.toList();
		for (int index : faceIndexes) {
			selected.add(interiors.get(index));
		}
		return selected;
	}

	static long count(HMesh3D mesh, HFace.Status status) {
		return mesh.faces().filter(f -> f.status() == status).count();
	}

	static double[] newellNormal(HFace face, HMesh3D mesh) {
		List<double[]> points = face.vertices().map(mesh::vertexXYZ).toList();
		double nx = 0;
		double ny = 0;
		double nz = 0;
		for (int i = 0; i < points.size(); i++) {
			double[] p1 = points.get(i);
			double[] p2 = points.get((i + 1) % points.size());
			nx += (p1[1] - p2[1]) * (p1[2] + p2[2]);
			ny += (p1[2] - p2[2]) * (p1[0] + p2[0]);
			nz += (p1[0] - p2[0]) * (p1[1] + p2[1]);
		}
		return new double[] { nx, ny, nz };
	}

	static double[] center(HFace face, HMesh3D mesh) {
		List<double[]> points = face.vertices().map(mesh::vertexXYZ).toList();
		double cx = 0;
		double cy = 0;
		double cz = 0;
		for (double[] p : points) {
			cx += p[0];
			cy += p[1];
			cz += p[2];
		}
		int n = points.size();
		return new double[] { cx / n, cy / n, cz / n };
	}

	static void assertConsistent(HMesh3D mesh) {
		check(mesh.edges().allMatch(e -> !e.isDiscarded()), "discarded edge");
		check(mesh.vertices().allMatch(v -> !v.isDiscarded()),
				"discarded vertex");
		check(mesh.faces().allMatch(f -> !f.isDiscarded()), "discarded face");
		check(mesh.edges().allMatch(e -> e.next() != null), "null next");
		check(mesh.edges().allMatch(e -> e.previous() != null),
				"null previous");
		check(mesh.edges().allMatch(e -> e.opposite() != null),
				"null opposite");
		check(mesh.edges().allMatch(e -> e == e.opposite().opposite()),
				"opposite symmetry");
		check(mesh.edges().allMatch(e -> e == e.next().previous()),
				"next/previous link 1");
		check(mesh.edges().allMatch(e -> e == e.previous().next()),
				"next/previous link 2");
		check(mesh.edges().allMatch(e -> e.head() == e.next().tail()),
				"broken edge chain");
		check(mesh.edges().allMatch(e -> e.cycle().count() > 2),
				"face with less than 3 edges");
		check(mesh.vertices().allMatch(v -> v == v.edge().head()),
				"vertex reference edge mismatch");
		check(mesh.vertices().allMatch(v -> v.incomingEdges()
				.map(HEdge::head).allMatch(v::equals)), "incoming edge head");
		check(mesh.edges().count() == mesh.edges().distinct().count(),
				"duplicate edges");
	}

	static void assertZsConsistent(HMesh3D mesh) {
		check(mesh.vertices().allMatch(v -> mesh.vertexZs().get(v) == mesh
				.vertexZ(v)), "vertexZs()/vertexZ(v) discrepancy");
	}

	/**
	 * Checks that every map value belongs to the result and every key to the
	 * source, i.e. mappings identify elements instead of crossing meshes
	 */
	static void assertMapsStayInMeshes(HMesh3D source, ExtrusionResult result) {
		Set<HVertex> sourceVertices = source.vertices()
				.collect(java.util.stream.Collectors.toSet());
		Set<HFace> sourceFaces = source.faces()
				.collect(java.util.stream.Collectors.toSet());
		Set<HEdge> sourceEdges = source.edges()
				.collect(java.util.stream.Collectors.toSet());
		Set<HVertex> resultVertices = result.mesh().vertices()
				.collect(java.util.stream.Collectors.toSet());
		Set<HFace> resultFaces = result.mesh().faces()
				.collect(java.util.stream.Collectors.toSet());

		result.originalFaces().forEach((key, value) -> {
			check(sourceFaces.contains(key), "originalFaces key left source");
			check(resultFaces.contains(value), "originalFaces value left result");
			check(key != value, "face map crosses meshes");
		});
		result.baseVertices().forEach((key, value) -> {
			check(sourceVertices.contains(key), "baseVertices key left source");
			check(resultVertices.contains(value),
					"baseVertices value left result");
		});
		result.liftedVertices().forEach((key, value) -> {
			check(sourceVertices.contains(key), "liftedVertices key left source");
			check(resultVertices.contains(value),
					"liftedVertices value left result");
		});
		result.sideFaces().forEach((key, value) -> {
			check(sourceEdges.contains(key), "sideFaces key left source");
			check(resultFaces.contains(value), "sideFaces value left result");
		});

		check(Collections.disjoint(sourceVertices, resultVertices),
				"source and result share a vertex");
		check(Collections.disjoint(sourceFaces, resultFaces),
				"source and result share a face");
		check(Collections.disjoint(sourceEdges,
				result.mesh().edges()
						.collect(java.util.stream.Collectors.toSet())),
				"source and result share an edge");
	}

	/**
	 * Two adjacent quads, selected both (open box, bottom stays open) and then
	 * only one of them (side faces only on the region border)
	 */
	static void testTwoAdjacentQuads() {
		int[][] faces = { { 0, 1, 4, 3 }, { 1, 2, 5, 4 } };
		double[][] coords = { { 0, 0, 0 }, { 1, 0, 0 }, { 2, 0, 0 },
				{ 0, 1, 0 }, { 1, 1, 0 }, { 2, 1, 0 } };
		HMesh3D source = mesh(faces, coords);

		ExtrusionResult all = RegionExtrusion.extrude(source,
				select(source, 0, 1), 0, 0, 1);
		HMesh3D result = all.mesh();
		assertConsistent(result);
		assertZsConsistent(result);
		check(result.vertices().count() == 12, "two quads: 12 vertices");
		check(count(result, HFace.Status.INTERIOR) == 8,
				"two quads: 8 interior faces (2 tops + 6 sides)");
		check(count(result, HFace.Status.BOUNDARY) == 1,
				"two quads: one open bottom border");
		check(all.originalFaces().size() == 2, "two quads: 2 face mappings");
		check(all.baseVertices().size() == 6, "two quads: 6 base vertices");
		check(all.liftedVertices().size() == 6,
				"two quads: 6 lifted vertices");
		check(all.sideFaces().size() == 6, "two quads: 6 side faces");
		check(all.sideFaces().values().stream().allMatch(
				f -> f.edges().count() == 4), "side faces must be quads");
		assertInteriorFacesAccounted(result, all);
		assertMapsStayInMeshes(source, all);
		assertSideFaceGeometry(source, all, 0, 0, 1);
		assertSideFaceKeyDirection(source, all);
		assertWindingsPreserved(source, all);

		// Only the first quad is lifted: 3 side faces, all 6 source vertices
		// remain referenced in place
		ExtrusionResult one = RegionExtrusion.extrude(source,
				select(source, 0), 0, 0, 1);
		assertConsistent(one.mesh());
		check(one.mesh().vertices().count() == 10,
				"one quad: 10 vertices (6 base + 4 lifted)");
		check(count(one.mesh(), HFace.Status.INTERIOR) == 5,
				"one quad: 5 interior faces (2 copies + 3 sides)");
		check(count(one.mesh(), HFace.Status.BOUNDARY) == 1,
				"one quad: a single open border");
		check(one.sideFaces().size() == 3, "one quad: 3 side faces");
		check(one.baseVertices().size() == 6,
				"one quad: every source vertex stays in place");
		check(one.liftedVertices().size() == 4,
				"one quad: 4 lifted vertices");
		assertInteriorFacesAccounted(one.mesh(), one);
		assertMapsStayInMeshes(source, one);
		assertSideFaceGeometry(source, one, 0, 0, 1);
	}

	/**
	 * Quad ring with a hole: the inner ring must get correctly oriented walls
	 * and the hole must remain open at the bottom
	 */
	static void testQuadRingWithHole() {
		int[][] faces = { { 0, 1, 5, 4 }, { 1, 2, 6, 5 }, { 2, 3, 7, 6 },
				{ 3, 0, 4, 7 } };
		double[][] coords = { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 4, 0 },
				{ 0, 4, 0 }, { 1, 1, 0 }, { 3, 1, 0 }, { 3, 3, 0 },
				{ 1, 3, 0 } };
		HMesh3D source = mesh(faces, coords);
		check(count(source, HFace.Status.BOUNDARY) == 2,
				"ring source: outer and hole borders");

		ExtrusionResult result = RegionExtrusion.extrude(source,
				select(source, 0, 1, 2, 3), 0, 0, 2);
		HMesh3D extruded = result.mesh();
		assertConsistent(extruded);
		check(extruded.vertices().count() == 16, "ring: 16 vertices");
		check(count(extruded, HFace.Status.INTERIOR) == 12,
				"ring: 4 tops + 8 walls");
		check(count(extruded, HFace.Status.BOUNDARY) == 2,
				"ring: outer bottom and hole bottom stay open");
		check(result.sideFaces().size() == 8, "ring: 8 side faces");
		check(result.liftedVertices().size() == 8, "ring: 8 lifted vertices");
		assertInteriorFacesAccounted(extruded, result);
		assertMapsStayInMeshes(source, result);
		assertSideFaceGeometry(source, result, 0, 0, 2);
		assertSideFaceKeyDirection(source, result);
		assertWindingsPreserved(source, result);
	}

	/**
	 * One face of a closed box: the extrusion of a closed component region
	 * stays closed and every generated edge has two INTERIOR incident faces
	 */
	static void testClosedBoxSingleFace() {
		HMesh3D source = boxMesh();
		check(count(source, HFace.Status.BOUNDARY) == 0,
				"box source must be closed");

		ExtrusionResult result = RegionExtrusion.extrude(source,
				select(source, 1), 0, 0, 1);
		HMesh3D extruded = result.mesh();
		assertConsistent(extruded);
		check(count(extruded, HFace.Status.BOUNDARY) == 0,
				"extruded box region must stay closed");
		check(extruded.vertices().count() == 12,
				"box face: 8 base + 4 lifted vertices");
		check(count(extruded, HFace.Status.INTERIOR) == 10,
				"box face: 6 source copies + 4 sides");
		check(result.sideFaces().size() == 4, "box face: 4 side faces");
		check(extruded.edges().allMatch(
				e -> e.opposite().face().status() == HFace.Status.INTERIOR),
				"closed mesh: no edge faces a BOUNDARY face");
		assertInteriorFacesAccounted(extruded, result);
		assertMapsStayInMeshes(source, result);
		assertSideFaceGeometry(source, result, 0, 0, 1);
		assertSideFaceKeyDirection(source, result);
		assertWindingsPreserved(source, result);
	}

	/**
	 * Two disjoint quads lifted together: several disconnected components in a
	 * single selection
	 */
	static void testTwoSeparatedRegions() {
		int[][] faces = { { 0, 1, 2, 3 }, { 4, 5, 6, 7 } };
		double[][] coords = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 },
				{ 0, 1, 0 }, { 5, 5, 0 }, { 6, 5, 0 }, { 6, 6, 0 },
				{ 5, 6, 0 } };
		HMesh3D source = mesh(faces, coords);

		ExtrusionResult result = RegionExtrusion.extrude(source,
				select(source, 0, 1), 0, 0, 1);
		HMesh3D extruded = result.mesh();
		assertConsistent(extruded);
		check(extruded.vertices().count() == 16,
				"separate regions: 8 base + 8 lifted vertices");
		check(count(extruded, HFace.Status.INTERIOR) == 10,
				"separate regions: 2 tops + 8 sides");
		check(count(extruded, HFace.Status.BOUNDARY) == 2,
				"separate regions: one open bottom per component");
		check(result.sideFaces().size() == 8,
				"separate regions: 8 side faces");
		check(result.originalFaces().size() == 2,
				"separate regions: 2 face mappings");
		assertInteriorFacesAccounted(extruded, result);
		assertMapsStayInMeshes(source, result);
	}

	/**
	 * Empty selection: independent equivalent mesh with identity based maps and
	 * no side faces
	 */
	static void testEmptySelection() {
		int[][] faces = { { 0, 1, 4, 3 }, { 1, 2, 5, 4 } };
		double[][] coords = { { 0, 0, 0 }, { 1, 0, 0 }, { 2, 0, 0 },
				{ 0, 1, 0 }, { 1, 1, 0 }, { 2, 1, 0 } };
		HMesh3D source = mesh(faces, coords);

		ExtrusionResult result = RegionExtrusion.extrude(source,
				new HashSet<>(), 3, 4, 5);
		HMesh3D copy = result.mesh();
		assertConsistent(copy);
		check(copy.vertices().count() == source.vertices().count(),
				"empty selection: same vertex count");
		check(copy.edges().count() == source.edges().count(),
				"empty selection: same edge count");
		check(copy.faces().count() == source.faces().count(),
				"empty selection: same face count");
		check(result.sideFaces().isEmpty(), "empty selection: no side face");
		check(result.liftedVertices().isEmpty(),
				"empty selection: no lifted vertex");
		check(result.baseVertices().size() == source.vertices().count(),
				"empty selection: every source vertex mapped in place");
		check(result.originalFaces().size() == count(source,
				HFace.Status.INTERIOR), "empty selection: every face mapped");

		result.baseVertices().forEach((sourceVertex, copyVertex) -> {
			check(Arrays.equals(source.vertexXYZ(sourceVertex),
					copy.vertexXYZ(copyVertex)),
					"empty selection: copied coordinates differ");
		});

		// The copy has its own coordinate storage: mutating it leaves the
		// source coordinates untouched
		Map.Entry<HVertex, HVertex> firstMapping = result.baseVertices()
				.entrySet().iterator().next();
		double sourceZBefore = source.vertexZ(firstMapping.getKey());
		HVertex firstCopy = firstMapping.getValue();
		copy.setVertexZ(firstCopy, sourceZBefore + 10);
		check(source.vertexZ(firstMapping.getKey()) == sourceZBefore,
				"empty selection: coordinate storage is shared");
		assertMapsStayInMeshes(source, result);
	}

	/**
	 * Whole closed component selected: plain translation, no side face and no
	 * in place vertex
	 */
	static void testEntireClosedComponent() {
		HMesh3D source = boxMesh();

		ExtrusionResult result = RegionExtrusion.extrude(source,
				new HashSet<>(source.faces()
						.filter(f -> f.status() == HFace.Status.INTERIOR)
						.toList()), 1, 2, 3);
		HMesh3D translated = result.mesh();
		assertConsistent(translated);
		check(translated.vertices().count() == 8,
				"whole component: 8 translated vertices");
		check(count(translated, HFace.Status.INTERIOR) == 6,
				"whole component: 6 translated faces");
		check(count(translated, HFace.Status.BOUNDARY) == 0,
				"whole component: still closed");
		check(result.sideFaces().isEmpty(),
				"whole component: no side face generated");
		check(result.baseVertices().isEmpty(),
				"whole component: no in place vertex");
		check(result.liftedVertices().size() == 8,
				"whole component: every vertex lifted once");

		result.liftedVertices().forEach((sourceVertex, liftedVertex) -> {
			double[] p = source.vertexXYZ(sourceVertex);
			double[] q = translated.vertexXYZ(liftedVertex);
			check(Math.abs(q[0] - (p[0] + 1)) < 1e-12
					&& Math.abs(q[1] - (p[1] + 2)) < 1e-12
					&& Math.abs(q[2] - (p[2] + 3)) < 1e-12,
					"whole component: wrong translation");
		});
		assertMapsStayInMeshes(source, result);
	}

	/**
	 * A fan of four triangles around a central vertex. Selecting two
	 * non adjacent triangles creates two disconnected selected face fans at
	 * the central vertex (butterfly vertex)
	 */
	static void testButterflyVertexRejected() {
		int[][] faces = { { 0, 1, 2 }, { 0, 2, 3 }, { 0, 3, 4 }, { 0, 4, 1 } };
		double[][] coords = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 },
				{ -1, 0, 0 }, { 0, -1, 0 } };
		HMesh3D source = mesh(faces, coords);
		expectIAE(() -> RegionExtrusion.extrude(source, select(source, 0, 2),
				0, 0, 1), "butterfly vertex must be rejected");

		// Two adjacent triangles of the same fan form a single connected fan
		// and are therefore accepted
		ExtrusionResult accepted = RegionExtrusion.extrude(source,
				select(source, 0, 1), 0, 0, 1);
		assertConsistent(accepted.mesh());
	}

	static void testInvalidDisplacements() {
		int[][] faces = { { 0, 1, 2, 3 } };
		double[][] coords = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 },
				{ 0, 1, 0 } };
		HMesh3D source = mesh(faces, coords);
		Set<HFace> selected = select(source, 0);

		expectIAE(() -> RegionExtrusion.extrude(source, selected, 0, 0, 0),
				"zero displacement must be rejected");
		expectIAE(() -> RegionExtrusion.extrude(source, selected, Double.NaN,
				0, 1), "NaN displacement must be rejected");
		expectIAE(
				() -> RegionExtrusion.extrude(source, selected,
						Double.POSITIVE_INFINITY, 0, 1),
				"infinite displacement must be rejected");
		expectIAE(
				() -> RegionExtrusion.extrude(source, selected, 1, 0, 0),
				"displacement parallel to a border edge must be rejected");
		expectIAE(
				() -> RegionExtrusion.extrude(source, selected, 2, 0, 0),
				"collinear displacement must be rejected");
	}

	static void testInvalidFaces() {
		int[][] faces = { { 0, 1, 4, 3 }, { 1, 2, 5, 4 } };
		double[][] coords = { { 0, 0, 0 }, { 1, 0, 0 }, { 2, 0, 0 },
				{ 0, 1, 0 }, { 1, 1, 0 }, { 2, 1, 0 } };
		HMesh3D source = mesh(faces, coords);

		// Face coming from another mesh
		HMesh3D other = mesh(faces, coords);
		HFace foreign = other.faces()
				.filter(f -> f.status() == HFace.Status.INTERIOR).findFirst()
				.orElseThrow();
		expectIAE(() -> RegionExtrusion.extrude(source,
				new HashSet<>(Collections.singletonList(foreign)), 0, 0, 1),
				"foreign face must be rejected");

		// BOUNDARY face
		HFace boundary = source.faces()
				.filter(f -> f.status() == HFace.Status.BOUNDARY).findFirst()
				.orElseThrow();
		expectIAE(() -> RegionExtrusion.extrude(source,
				new HashSet<>(Collections.singletonList(boundary)), 0, 0, 1),
				"BOUNDARY face must be rejected");

		// Discarded face, produced by merging the two adjacent quads
		List<HFace> interiors = source.faces()
				.filter(f -> f.status() == HFace.Status.INTERIOR).toList();
		HFace absorbed = interiors.get(1);
		check(source.mergeFaces(interiors.get(0), absorbed),
				"test setup: merge must succeed");
		check(absorbed.isDiscarded(), "test setup: absorbed face discarded");
		expectIAE(() -> RegionExtrusion.extrude(source,
				new HashSet<>(Collections.singletonList(absorbed)), 0, 0, 1),
				"discarded face must be rejected");

		// UNKNOWN face produced from an edge source conversion
		List<int[]> edges = new ArrayList<>();
		for (int[] face : faces) {
			for (int i = 0; i < face.length; i++) {
				edges.add(new int[] { face[i], face[(i + 1) % face.length] });
			}
		}
		HMesh3D edgeMesh = new EdgeSource(edges).toHMesh(new Coord3DSource(
				coords)).orElseThrow();
		HFace unknown = edgeMesh.faces().findFirst().orElseThrow();
		check(unknown.status() == HFace.Status.UNKNOWN,
				"test setup: edge source builds UNKNOWN faces");
		expectIAE(
				() -> RegionExtrusion.extrude(edgeMesh,
						new HashSet<>(Collections.singletonList(unknown)), 0, 0,
						1),
				"UNKNOWN face must be rejected");

		// Null arguments
		expectNPE(() -> RegionExtrusion.extrude(null, new HashSet<>(), 0, 0, 1),
				"null source must raise NPE");
		expectNPE(() -> RegionExtrusion.extrude(source, null, 0, 0, 1),
				"null selection must raise NPE");
	}

	/**
	 * Source and result can be edited independently; the maps keep pointing at
	 * elements of their own mesh. The result also supports the existing
	 * splitEdge / splitFace / trim edits
	 */
	static void testIndependentEdits() {
		int[][] faces = { { 0, 1, 4, 3 }, { 1, 2, 5, 4 } };
		double[][] coords = { { 0, 0, 0 }, { 1, 0, 0 }, { 2, 0, 0 },
				{ 0, 1, 0 }, { 1, 1, 0 }, { 2, 1, 0 } };
		HMesh3D source = mesh(faces, coords);

		// User data registered on the source must stay untouched and must not
		// leak into the result
		HDData<HVertex> sourceWeights = source.createVertexDoubleData();
		sourceWeights.setAll(v -> source.vertexX(v) + 100);
		HData<HFace, String> sourceLabels = source.createFaceData();
		source.faces().forEach(f -> sourceLabels.set(f, "source:" + f.status()));

		ExtrusionResult result = RegionExtrusion.extrude(source,
				select(source, 0, 1), 0, 0, 1);
		HMesh3D extruded = result.mesh();

		check(source.vertices().allMatch(
				v -> sourceWeights.get(v) == source.vertexX(v) + 100),
				"source vertex data altered");
		check(source.faces().allMatch(f -> sourceLabels.get(f) != null
				&& sourceLabels.get(f).startsWith("source:")),
				"source face data altered");
		check(source.vertices().count() == 6, "source topology altered");

		// Result edits: split a top face, split a side edge, then trim. The top
		// face is split first so the later edge split cannot change its cycle
		HFace topFace = result.originalFaces().values().iterator().next();
		List<HVertex> topVertices = topFace.vertices().toList();
		Optional<HFace> splitFace = extruded.splitFace(topFace,
				topVertices.get(0), topVertices.get(2));
		check(splitFace.isPresent(), "splitFace must work on result");

		HEdge sideEdge = result.sideFaces().values().iterator().next().edge();
		double[] tail = extruded.vertexXYZ(sideEdge.tail());
		double[] head = extruded.vertexXYZ(sideEdge.head());
		HVertex newVertex = extruded.splitEdge(sideEdge,
				(tail[0] + head[0]) / 2, (tail[1] + head[1]) / 2,
				(tail[2] + head[2]) / 2);
		check(!newVertex.isDiscarded(), "splitEdge must work on result");
		assertConsistent(extruded);

		long facesBeforeTrim = extruded.faces().count();
		extruded.trim();
		check(extruded.faces().count() == facesBeforeTrim,
				"trim must not remove live faces");
		assertConsistent(extruded);
		assertMapsStayInMeshes(source, result);

		// Editing the source after the extrusion does not invalidate the keys
		HEdge sourceEdge = source.edges()
				.filter(e -> e.face().status() == HFace.Status.INTERIOR)
				.findFirst().orElseThrow();
		double[] p = source.vertexXYZ(sourceEdge.tail());
		double[] q = source.vertexXYZ(sourceEdge.head());
		source.splitEdge(sourceEdge, (p[0] + q[0]) / 2, (p[1] + q[1]) / 2,
				(p[2] + q[2]) / 2);
		assertMapsStayInMeshes(source, result);
	}

	/**
	 * The values of originalFaces + sideFaces are exactly the INTERIOR faces of
	 * the result and every mapping value is distinct
	 */
	static void assertInteriorFacesAccounted(HMesh3D result,
			ExtrusionResult extrusion) {
		Set<HFace> mapped = new HashSet<>(extrusion.originalFaces().values());
		mapped.addAll(extrusion.sideFaces().values());
		Set<HFace> interiors = result.faces()
				.filter(f -> f.status() == HFace.Status.INTERIOR)
				.collect(java.util.stream.Collectors.toSet());
		check(mapped.equals(interiors),
				"face maps do not account for every INTERIOR face");
		check(mapped.size() == extrusion.originalFaces().size()
				+ extrusion.sideFaces().size(),
				"face mapping values must be distinct faces");
		check(new HashSet<>(extrusion.baseVertices().values()).size() == extrusion
				.baseVertices().size(), "base vertex values must be distinct");
		check(new HashSet<>(extrusion.liftedVertices().values()).size() == extrusion
				.liftedVertices().size(),
				"lifted vertex values must be distinct");

		// A boundary vertex exists both in place and lifted, as two distinct
		// result vertices
		Set<HVertex> baseValues = new HashSet<>(
				extrusion.baseVertices().values());
		check(Collections.disjoint(baseValues,
				new HashSet<>(extrusion.liftedVertices().values())),
				"base and lifted vertices must be distinct result elements");
	}

	/**
	 * The winding of every source INTERIOR face is preserved: its result face
	 * visits the matching base or lifted vertices in the same order. A source
	 * face is either entirely in place or entirely lifted
	 */
	static void assertWindingsPreserved(HMesh3D source,
			ExtrusionResult extrusion) {
		HMesh3D result = extrusion.mesh();

		extrusion.originalFaces().forEach((sourceFace, resultFace) -> {
			List<HVertex> sourceVertices = sourceFace.vertices().toList();
			List<HVertex> resultVertices = resultFace.vertices().toList();
			check(sourceVertices.size() == resultVertices.size(),
					"mapped face changed size");

			// A shared vertex may exist in both maps; the first result vertex
			// tells which representation this face uses
			HVertex firstResult = resultVertices.get(0);
			boolean lifted = firstResult == extrusion.liftedVertices()
					.get(sourceVertices.get(0));

			for (int i = 0; i < sourceVertices.size(); i++) {
				HVertex expected = lifted
						? extrusion.liftedVertices().get(sourceVertices.get(i))
						: extrusion.baseVertices().get(sourceVertices.get(i));
				check(expected == resultVertices.get(i),
						"mapped face winding not preserved");
			}
		});
	}

	/**
	 * Side face orientation: its geometric normal is parallel to the cross
	 * product of the oriented base border edge and the displacement and points
	 * away from the lifted region
	 */
	static void assertSideFaceGeometry(HMesh3D source,
			ExtrusionResult extrusion, double dx, double dy, double dz) {
		HMesh3D result = extrusion.mesh();

		extrusion.sideFaces().forEach((sourceEdge, sideFace) -> {
			double[] tail = source.vertexXYZ(sourceEdge.tail());
			double[] head = source.vertexXYZ(sourceEdge.head());
			double ex = head[0] - tail[0];
			double ey = head[1] - tail[1];
			double ez = head[2] - tail[2];

			// Expected outward normal: oriented border edge x displacement
			double nx = ey * dz - ez * dy;
			double ny = ez * dx - ex * dz;
			double nz = ex * dy - ey * dx;
			check(nx != 0 || ny != 0 || nz != 0,
					"degenerate side face normal");

			double[] actual = newellNormal(sideFace, result);
			double dot = nx * actual[0] + ny * actual[1] + nz * actual[2];
			double normProduct = Math.sqrt(
					(nx * nx + ny * ny + nz * nz) * (actual[0] * actual[0]
							+ actual[1] * actual[1] + actual[2] * actual[2]));
			check(Math.abs(dot - normProduct) < 1e-9 * normProduct + 1e-12,
					"side face has the wrong orientation");

			HFace topFace = extrusion.originalFaces().get(sourceEdge.face());
			double[] sideCenter = center(sideFace, result);
			double[] topCenter = center(topFace, result);
			double outward = actual[0] * (sideCenter[0] - topCenter[0])
					+ actual[1] * (sideCenter[1] - topCenter[1])
					+ actual[2] * (sideCenter[2] - topCenter[2]);
			check(outward >= -1e-9, "side face normal must face outward");
		});
	}

	/**
	 * The key of a side face mapping is the selected face half-edge, oriented
	 * from a base vertex to another base vertex. The corresponding side face
	 * must contain that exact oriented base edge and the two lifted vertices
	 */
	static void assertSideFaceKeyDirection(HMesh3D source,
			ExtrusionResult extrusion) {
		HMesh3D result = extrusion.mesh();

		extrusion.sideFaces().forEach((sourceEdge, sideFace) -> {
			HVertex baseTail = extrusion.baseVertices().get(sourceEdge.tail());
			HVertex baseHead = extrusion.baseVertices().get(sourceEdge.head());
			HVertex liftedTail = extrusion.liftedVertices()
					.get(sourceEdge.tail());
			HVertex liftedHead = extrusion.liftedVertices()
					.get(sourceEdge.head());
			check(baseTail != null && baseHead != null && liftedTail != null
					&& liftedHead != null,
					"side face border vertices missing from the maps");

			Set<HVertex> expected = new HashSet<>(Arrays.asList(baseTail,
					baseHead, liftedHead, liftedTail));
			Set<HVertex> actual = sideFace.vertices()
					.collect(java.util.stream.Collectors.toSet());
			check(expected.equals(actual),
					"side face does not connect the right base/lifted vertices");

			HEdge baseEdge = sideFace.edges()
					.filter(e -> e.tail() == baseTail && e.head() == baseHead)
					.findFirst().orElse(null);
			check(baseEdge != null,
					"side face must contain the oriented base border edge");

			HEdge liftedEdge = sideFace.edges()
					.filter(e -> e.tail() == liftedTail
							&& e.head() == liftedHead)
					.findFirst().orElse(null);
			check(liftedEdge == null,
					"lifted rim must run in the reverse direction");
			check(sideFace.edges().anyMatch(e -> e.tail() == liftedHead
					&& e.head() == liftedTail),
					"reversed lifted rim edge missing");
		});
	}

	static HMesh3D boxMesh() {
		int[][] faces = { { 0, 3, 2, 1 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 },
				{ 3, 7, 6, 2 }, { 0, 4, 7, 3 }, { 1, 2, 6, 5 } };
		double[][] coords = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 },
				{ 0, 1, 0 }, { 0, 0, 1 }, { 1, 0, 1 }, { 1, 1, 1 },
				{ 0, 1, 1 } };
		return mesh(faces, coords);
	}

	interface ThrowingRunnable {
		void run();
	}

	static void expectIAE(ThrowingRunnable runnable, String message) {
		boolean thrown = false;
		try {
			runnable.run();
		} catch (IllegalArgumentException e) {
			thrown = true;
		}
		check(thrown, message);
	}

	static void expectNPE(ThrowingRunnable runnable, String message) {
		boolean thrown = false;
		try {
			runnable.run();
		} catch (NullPointerException e) {
			thrown = true;
		}
		check(thrown, message);
	}

	public static void main(String[] args) {
		assertZsConsistent(mesh(new int[][] { { 0, 1, 2, 3 } },
				new double[][] { { 0, 0, 7 }, { 1, 0, 7 }, { 1, 1, 7 },
						{ 0, 1, 7 } }));
		testTwoAdjacentQuads();
		testQuadRingWithHole();
		testClosedBoxSingleFace();
		testTwoSeparatedRegions();
		testEmptySelection();
		testEntireClosedComponent();
		testButterflyVertexRejected();
		testInvalidDisplacements();
		testInvalidFaces();
		testIndependentEdits();
		System.out.println("region extrusion PASS");
	}
}
