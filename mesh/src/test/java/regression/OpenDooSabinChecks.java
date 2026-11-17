package regression;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

import math.Vector3f;
import mesh.Edge3D;
import mesh.Face3D;
import mesh.Mesh3D;
import mesh.creator.primitives.CubeCreator;
import mesh.modifier.subdivision.DooSabinModifier;
import mesh.util.TraverseHelper;
import task_support.Suite;

/**
 * Regression checks for Doo-Sabin subdivision of meshes with open boundaries
 * and for the vertex fan traversal of {@link TraverseHelper}.
 *
 * <p>The checks cover single faces, patches of faces that share edges, rings
 * with inner holes, the closed cube, spatially coincident but disconnected
 * components, invalid vertex fans, and the rejection of meshes with interior
 * vertices of valence two. Both the geometry (vertex positions) and the
 * directed edge associations of the results are verified.
 */
public final class OpenDooSabinChecks {

    private OpenDooSabinChecks() {
    }

    /**
     * Registers all checks with the given suite.
     *
     * @param suite the suite to register the checks with
     */
    public static void register(Suite suite) {
        registerFans(suite);
        registerBoundary(suite);
        registerRepeat(suite);
        registerCompat(suite);
    }

    // ------------------------------------------------------------------
    // fans: vertex fans and boundary classification of the TraverseHelper
    // ------------------------------------------------------------------

    private static void registerFans(Suite suite) {
        suite.add("fans", "single-face-open-fans", () -> {
            Mesh3D mesh = sheet();
            TraverseHelper helper = new TraverseHelper(mesh);
            for (int i = 0; i < 4; i++) {
                List<Edge3D> fan = helper.getVertexFan(i);
                suite.check(fan.size() == 1, "fan size of vertex " + i);
                Edge3D edge = fan.get(0);
                suite.check(edge.getFromIndex() == i
                        && edge.getToIndex() == (i + 1) % 4,
                        "fan edge of vertex " + i);
                suite.check(helper.isBoundaryVertex(i),
                        "vertex " + i + " is on the boundary");
            }
        });

        suite.add("fans", "isolated-vertex-has-empty-fan", () -> {
            Mesh3D mesh = sheet();
            mesh.addVertex(5, 5, 5);
            TraverseHelper helper = new TraverseHelper(mesh);
            suite.check(helper.getVertexFan(4).isEmpty(),
                    "isolated vertex has an empty fan");
            suite.check(!helper.isBoundaryVertex(4),
                    "isolated vertex is not a boundary vertex");
        });

        suite.add("fans", "shared-edge-patch-fan-order", () -> {
            Mesh3D mesh = twoQuadPatch();
            TraverseHelper helper = new TraverseHelper(mesh);
            List<Edge3D> fan1 = helper.getVertexFan(1);
            suite.check(fan1.size() == 2, "fan size of vertex 1");
            suite.check(fan1.get(0).equals(new Edge3D(1, 4)),
                    "fan of vertex 1 starts at the boundary incoming edge");
            suite.check(fan1.get(1).equals(new Edge3D(1, 2)),
                    "fan of vertex 1 ends at the boundary outgoing edge");
            List<Edge3D> fan4 = helper.getVertexFan(4);
            suite.check(fan4.size() == 2, "fan size of vertex 4");
            suite.check(fan4.get(0).equals(new Edge3D(4, 1)),
                    "fan of vertex 4 start");
            suite.check(fan4.get(1).equals(new Edge3D(4, 3)),
                    "fan of vertex 4 end");
            suite.check(helper.getPairNext(1, 4).equals(new Edge3D(1, 2)),
                    "fan follows the pair-next direction");
            for (int i = 0; i < 6; i++)
                suite.check(helper.isBoundaryVertex(i),
                        "vertex " + i + " is on the boundary");
        });

        suite.add("fans", "closed-cube-fans-are-closed-loops", () -> {
            Mesh3D cube = new CubeCreator().create();
            TraverseHelper helper = new TraverseHelper(cube);
            for (int i = 0; i < cube.getVertexCount(); i++) {
                List<Edge3D> fan = helper.getVertexFan(i);
                suite.check(fan.size() == 3, "fan size of vertex " + i);
                suite.check(!helper.isBoundaryVertex(i),
                        "vertex " + i + " is interior");
                Edge3D first = fan.get(0);
                Edge3D last = fan.get(fan.size() - 1);
                suite.check(helper
                        .getPairNext(last.getFromIndex(), last.getToIndex())
                        .equals(first), "fan of vertex " + i + " is closed");
                int smallest = Integer.MAX_VALUE;
                for (Edge3D edge : fan)
                    smallest = Math.min(smallest, edge.getToIndex());
                suite.check(first.getToIndex() == smallest,
                        "fan of vertex " + i + " starts at smallest to-index");
                for (int j = 0; j + 1 < fan.size(); j++) {
                    Edge3D edge = fan.get(j);
                    suite.check(helper
                            .getPairNext(edge.getFromIndex(),
                                    edge.getToIndex())
                            .equals(fan.get(j + 1)),
                            "fan of vertex " + i + " follows pair-next");
                }
            }
        });

        suite.add("fans", "ring-with-hole-fans", () -> {
            Mesh3D ring = ringWithHole();
            TraverseHelper helper = new TraverseHelper(ring);
            for (int i = 0; i < 8; i++) {
                suite.check(helper.getVertexFan(i).size() == 2,
                        "fan size of vertex " + i);
                suite.check(helper.isBoundaryVertex(i),
                        "vertex " + i + " is on the boundary");
            }
            List<Edge3D> fan0 = helper.getVertexFan(0);
            suite.check(fan0.get(0).equals(new Edge3D(0, 4)),
                    "outer vertex fan starts at boundary incoming edge");
            suite.check(fan0.get(1).equals(new Edge3D(0, 1)),
                    "outer vertex fan ends at boundary outgoing edge");
            List<Edge3D> fan4 = helper.getVertexFan(4);
            suite.check(fan4.get(0).equals(new Edge3D(4, 0)),
                    "inner vertex fan start");
            suite.check(fan4.get(1).equals(new Edge3D(4, 7)),
                    "inner vertex fan end");
        });

        suite.add("fans", "coincident-components-are-not-welded", () -> {
            Mesh3D mesh = coincidentComponents();
            TraverseHelper helper = new TraverseHelper(mesh);
            suite.check(helper.getVertexFan(0).size() == 1,
                    "fan of vertex 0 stays separate");
            suite.check(helper.getVertexFan(4).size() == 1,
                    "fan of coincident vertex 4 stays separate");
            suite.check(helper.getVertexFan(0).get(0)
                    .equals(new Edge3D(0, 1)), "fan edge of vertex 0");
            suite.check(helper.getVertexFan(4).get(0)
                    .equals(new Edge3D(4, 5)), "fan edge of vertex 4");
            suite.check(helper.getFaceByEdge(0, 1) != helper
                    .getFaceByEdge(4, 5), "components keep their own faces");
        });

        suite.add("fans", "two-valent-interior-loop-is-a-valid-fan", () -> {
            Mesh3D mesh = cubeWithEdgeMidpoint();
            TraverseHelper helper = new TraverseHelper(mesh);
            List<Edge3D> fan = helper.getVertexFan(8);
            suite.check(fan.size() == 2, "two-valent fan size");
            suite.check(!helper.isBoundaryVertex(8),
                    "two-valent vertex is interior");
            suite.check(fan.get(0).equals(new Edge3D(8, 0)),
                    "two-valent fan starts at smallest to-index");
            suite.check(fan.get(1).equals(new Edge3D(8, 1)),
                    "two-valent fan second edge");
            suite.check(helper.getPairNext(8, 1).equals(new Edge3D(8, 0)),
                    "two-valent fan is closed");
        });

        suite.add("fans", "duplicate-directed-edge-is-rejected", () -> {
            Mesh3D mesh = new Mesh3D();
            mesh.addVertex(0, 0, 0);
            mesh.addVertex(1, 0, 0);
            mesh.addVertex(1, 1, 0);
            mesh.addVertex(0, 1, 0);
            mesh.addFace(0, 1, 2);
            mesh.addFace(0, 1, 3);
            expectRejection(suite, () -> new TraverseHelper(mesh),
                    "duplicate directed edge");
        });

        suite.add("fans", "three-faces-on-one-edge-are-rejected", () -> {
            Mesh3D mesh = new Mesh3D();
            mesh.addVertex(0, 0, 0);
            mesh.addVertex(1, 0, 0);
            mesh.addVertex(1, 1, 0);
            mesh.addVertex(0, 1, 0);
            mesh.addVertex(0.5f, -1, 0);
            mesh.addFace(0, 1, 2);
            mesh.addFace(1, 0, 3);
            mesh.addFace(0, 1, 4);
            expectRejection(suite, () -> new TraverseHelper(mesh),
                    "three faces on one edge");
        });

        suite.add("fans", "disconnected-fans-are-rejected", () -> {
            Mesh3D mesh = new Mesh3D();
            mesh.addVertex(0, 0, 0);
            mesh.addVertex(1, 0, 0);
            mesh.addVertex(1, 1, 0);
            mesh.addVertex(0, 1, 0);
            mesh.addVertex(2, 0, 0);
            mesh.addVertex(3, 1, 0);
            mesh.addVertex(2, 2, 0);
            mesh.addFace(0, 1, 2, 3);
            mesh.addFace(0, 4, 5, 6);
            expectRejection(suite, () -> new TraverseHelper(mesh),
                    "disconnected fans at one vertex");
        });
    }

    // ------------------------------------------------------------------
    // boundary: subdivision of meshes with open boundaries
    // ------------------------------------------------------------------

    private static void registerBoundary(Suite suite) {
        suite.add("boundary", "single-face-subdivision", () -> {
            Mesh3D mesh = sheet();
            Mesh3D result = new DooSabinModifier(1).modify(mesh);
            suite.check(result == mesh, "modify returns the same mesh");
            checkVertexPositions(suite, mesh, SHEET_VERTICES, "sheet");
            checkFaces(suite, mesh, SHEET_FACES, "sheet");
            checkBoundaryEdges(suite, mesh, SHEET_BOUNDARY, "sheet");
            checkEdgePairing(suite, mesh, "sheet");
            checkDistinctFaceVertices(suite, mesh, "sheet");
        });

        suite.add("boundary", "shared-edge-patch-subdivision", () -> {
            Mesh3D mesh = twoQuadPatch();
            new DooSabinModifier(1).modify(mesh);
            checkVertexPositions(suite, mesh, PATCH_VERTICES, "patch");
            checkFaces(suite, mesh, PATCH_FACES, "patch");
            checkBoundaryEdges(suite, mesh, PATCH_BOUNDARY, "patch");
            checkEdgePairing(suite, mesh, "patch");
            checkDistinctFaceVertices(suite, mesh, "patch");
        });

        suite.add("boundary", "ring-with-hole-keeps-both-loops", () -> {
            Mesh3D mesh = ringWithHole();
            new DooSabinModifier(1).modify(mesh);
            suite.check(mesh.getVertexCount() == 24, "ring vertex count");
            suite.check(mesh.getFaceCount() == 24, "ring face count");
            for (int i = 0; i < 8; i++) {
                Vector3f v = mesh.getVertexAt(16 + i);
                Vector3f original = RING_VERTICES[i];
                suite.check(close(v, original), "ring boundary vertex " + i);
            }
            checkBoundaryEdgePositions(suite, mesh, RING_BOUNDARY, "ring");
            checkEdgePairing(suite, mesh, "ring");
            checkDistinctFaceVertices(suite, mesh, "ring");
        });

        suite.add("boundary", "mixed-triangle-quad-patch", () -> {
            Mesh3D mesh = mixedPatch();
            new DooSabinModifier(1).modify(mesh);
            suite.check(mesh.getVertexCount() == 12, "mixed vertex count");
            suite.check(mesh.getFaceCount() == 10, "mixed face count");
            checkBoundaryEdgePositions(suite, mesh, MIXED_BOUNDARY, "mixed");
            checkEdgePairing(suite, mesh, "mixed");
            checkDistinctFaceVertices(suite, mesh, "mixed");
        });

        suite.add("boundary", "coincident-components-subdivide-separately",
                () -> {
                    Mesh3D mesh = coincidentComponents();
                    new DooSabinModifier(1).modify(mesh);
                    suite.check(mesh.getVertexCount() == 16,
                            "components vertex count");
                    suite.check(mesh.getFaceCount() == 10,
                            "components face count");
                    Set<Integer> first = Set.of(0, 1, 2, 3, 8, 9, 10, 11);
                    Set<Integer> second = Set.of(4, 5, 6, 7, 12, 13, 14, 15);
                    for (Face3D face : mesh.getFaces()) {
                        boolean inFirst = true;
                        boolean inSecond = true;
                        for (int index : face.indices) {
                            inFirst &= first.contains(index);
                            inSecond &= second.contains(index);
                        }
                        suite.check(inFirst != inSecond,
                                "face does not mix components");
                    }
                    checkEdgePairing(suite, mesh, "components");
                    checkDistinctFaceVertices(suite, mesh, "components");
                });

        suite.add("boundary", "face-with-two-distinct-indices-is-rejected",
                () -> {
                    Mesh3D mesh = new Mesh3D();
                    mesh.addVertex(0, 0, 0);
                    mesh.addVertex(1, 0, 0);
                    mesh.addVertex(0, 1, 0);
                    mesh.addFace(0, 1, 0);
                    List<String> facesBefore = canonicalFaces(mesh);
                    int verticesBefore = mesh.getVertexCount();
                    expectRejection(suite,
                            () -> new DooSabinModifier(1).modify(mesh),
                            "face with two distinct indices");
                    suite.check(mesh.getVertexCount() == verticesBefore,
                            "mesh vertices unchanged after rejection");
                    suite.check(canonicalFaces(mesh).equals(facesBefore),
                            "mesh faces unchanged after rejection");
                });

        suite.add("boundary", "two-valent-interior-vertex-is-rejected", () -> {
            Mesh3D mesh = cubeWithEdgeMidpoint();
            List<String> facesBefore = canonicalFaces(mesh);
            int verticesBefore = mesh.getVertexCount();
            expectRejection(suite, () -> new DooSabinModifier(1).modify(mesh),
                    "two-valent interior vertex");
            suite.check(mesh.getVertexCount() == verticesBefore,
                    "mesh vertices unchanged after rejection");
            suite.check(canonicalFaces(mesh).equals(facesBefore),
                    "mesh faces unchanged after rejection");
        });
    }

    // ------------------------------------------------------------------
    // repeat: repeated and continued subdivision
    // ------------------------------------------------------------------

    private static void registerRepeat(Suite suite) {
        suite.add("repeat", "zero-rounds-returns-input-unchanged", () -> {
            Mesh3D mesh = sheet();
            List<String> facesBefore = canonicalFaces(mesh);
            List<String> verticesBefore = vertexPositions(mesh);
            Mesh3D result = new DooSabinModifier(0).modify(mesh);
            suite.check(result == mesh, "zero rounds returns the same mesh");
            suite.check(canonicalFaces(mesh).equals(facesBefore),
                    "zero rounds keeps the faces");
            suite.check(vertexPositions(mesh).equals(verticesBefore),
                    "zero rounds keeps the vertices");
            Mesh3D invalid = new Mesh3D();
            invalid.addVertex(0, 0, 0);
            invalid.addVertex(1, 0, 0);
            invalid.addVertex(0, 1, 0);
            invalid.addFace(0, 1, 0);
            Mesh3D returned = new DooSabinModifier(0).modify(invalid);
            suite.check(returned == invalid,
                    "zero rounds returns invalid input without validation");
            suite.check(invalid.getFaceCount() == 1
                    && invalid.getVertexCount() == 3,
                    "zero rounds keeps invalid input unchanged");
        });

        suite.add("repeat", "two-rounds-equal-two-sequential-rounds", () -> {
            Mesh3D batched = sheet();
            new DooSabinModifier(2).modify(batched);
            Mesh3D sequential = sheet();
            new DooSabinModifier(1).modify(sequential);
            new DooSabinModifier(1).modify(sequential);
            checkSameDirectedMesh(suite, batched, sequential,
                    "batched vs sequential");
        });

        suite.add("repeat", "boundary-is-stable-across-rounds", () -> {
            Mesh3D mesh = sheet();
            new DooSabinModifier(3).modify(mesh);
            checkBoundaryEdgePositions(suite, mesh, SHEET_BOUNDARY_POSITIONS,
                    "sheet");
            TraverseHelper helper = new TraverseHelper(mesh);
            Set<String> boundaryVertices = new HashSet<>();
            for (int i = 0; i < mesh.getVertexCount(); i++)
                if (helper.isBoundaryVertex(i))
                    boundaryVertices.add(position(mesh.getVertexAt(i)));
            suite.check(boundaryVertices.equals(Set.of("0,0,0", "2,0,0",
                    "2,2,0", "0,2,0")), "boundary corners stay in place");
            checkEdgePairing(suite, mesh, "sheet");
            checkDistinctFaceVertices(suite, mesh, "sheet");
        });

        suite.add("repeat", "ring-with-hole-subdivides-continuously", () -> {
            Mesh3D mesh = ringWithHole();
            for (int round = 1; round <= 3; round++) {
                new DooSabinModifier(1).modify(mesh);
                checkBoundaryEdgePositions(suite, mesh, RING_BOUNDARY,
                        "ring round " + round);
                checkEdgePairing(suite, mesh, "ring round " + round);
                checkDistinctFaceVertices(suite, mesh, "ring round " + round);
                new TraverseHelper(mesh);
                suite.check(true, "ring round " + round
                        + " output is traversable");
            }
        });
    }

    // ------------------------------------------------------------------
    // compat: closed meshes keep the legacy results
    // ------------------------------------------------------------------

    private static void registerCompat(Suite suite) {
        suite.add("compat", "closed-cube-matches-legacy-result", () -> {
            Mesh3D cube = new CubeCreator().create();
            new DooSabinModifier(1).modify(cube);
            suite.check(cube.getVertexCount() == 24, "cube vertex count");
            suite.check(cube.getFaceCount() == 26, "cube face count");
            checkVertexPositions(suite, cube, CUBE_VERTICES, "cube");
            checkFaces(suite, cube, CUBE_FACES, "cube");
            suite.check(boundaryEdges(cube).isEmpty(),
                    "closed cube has no boundary");
            checkEdgePairing(suite, cube, "cube");
        });

        suite.add("compat", "modify-modifies-in-place-and-returns-same", () -> {
            Mesh3D cube = new CubeCreator().create();
            Mesh3D result = new DooSabinModifier(1).modify(cube);
            suite.check(result == cube, "modify returns the same instance");
            suite.check(cube.getFaceCount() == 26,
                    "the same instance was refined");
            Mesh3D defaultModifier = new CubeCreator().create();
            new DooSabinModifier().modify(defaultModifier);
            Mesh3D explicit = new CubeCreator().create();
            new DooSabinModifier(1).modify(explicit);
            checkSameDirectedMesh(suite, defaultModifier, explicit,
                    "default constructor");
        });

        suite.add("compat", "closed-cube-two-rounds", () -> {
            Mesh3D cube = new CubeCreator().create();
            new DooSabinModifier(2).modify(cube);
            suite.check(cube.getVertexCount() == 96,
                    "cube two rounds vertex count");
            suite.check(cube.getFaceCount() == 98,
                    "cube two rounds face count");
            suite.check(boundaryEdges(cube).isEmpty(),
                    "two rounds keep the mesh closed");
            checkEdgePairing(suite, cube, "cube two rounds");
            checkDistinctFaceVertices(suite, cube, "cube two rounds");
        });

        suite.add("compat", "legacy-single-step-traversal-still-works", () -> {
            Mesh3D mesh = twoQuadPatch();
            TraverseHelper helper = new TraverseHelper(mesh);
            suite.check(helper.getOutgoing(1).equals(new Edge3D(1, 2)),
                    "getOutgoing returns the last mapped outgoing edge");
            suite.check(helper.getNext(0, 1).equals(new Edge3D(1, 4)),
                    "getNext follows the face");
            suite.check(helper.getPair(1, 4).equals(new Edge3D(4, 1)),
                    "getPair reverses the edge");
            suite.check(helper.getPairNext(1, 4).equals(new Edge3D(1, 2)),
                    "getPairNext crosses to the next face");
            suite.check(helper.getFaceByEdge(1, 4) == mesh.getFaceAt(0),
                    "getFaceByEdge returns the adjacent face");
            suite.check(helper.getAllEdges().size() == 7,
                    "getAllEdges lists each undirected edge once");
        });
    }

    // ------------------------------------------------------------------
    // mesh builders
    // ------------------------------------------------------------------

    /**
     * A single quad in the XY plane.
     */
    private static Mesh3D sheet() {
        Mesh3D mesh = new Mesh3D();
        mesh.addVertex(0, 0, 0);
        mesh.addVertex(2, 0, 0);
        mesh.addVertex(2, 2, 0);
        mesh.addVertex(0, 2, 0);
        mesh.addFace(0, 1, 2, 3);
        return mesh;
    }

    /**
     * Two quads that share the edge between vertices 1 and 4.
     */
    private static Mesh3D twoQuadPatch() {
        Mesh3D mesh = new Mesh3D();
        mesh.addVertex(0, 0, 0);
        mesh.addVertex(1, 0, 0);
        mesh.addVertex(2, 0, 0);
        mesh.addVertex(0, 1, 0);
        mesh.addVertex(1, 1, 0);
        mesh.addVertex(2, 1, 0);
        mesh.addFace(0, 1, 4, 3);
        mesh.addFace(1, 2, 5, 4);
        return mesh;
    }

    /**
     * Four quads that form a square ring around a square hole.
     */
    private static Mesh3D ringWithHole() {
        Mesh3D mesh = new Mesh3D();
        for (Vector3f v : RING_VERTICES)
            mesh.addVertex(v.x, v.y, v.z);
        mesh.addFace(0, 1, 5, 4);
        mesh.addFace(1, 2, 6, 5);
        mesh.addFace(2, 3, 7, 6);
        mesh.addFace(3, 0, 4, 7);
        return mesh;
    }

    /**
     * A triangle and a quad that share the edge between vertices 1 and 2.
     */
    private static Mesh3D mixedPatch() {
        Mesh3D mesh = new Mesh3D();
        mesh.addVertex(0, 0, 0);
        mesh.addVertex(1, 0, 0);
        mesh.addVertex(0.5f, 1, 0);
        mesh.addVertex(2, 0, 0);
        mesh.addVertex(1.5f, 1, 0);
        mesh.addFace(0, 1, 2);
        mesh.addFace(1, 3, 4, 2);
        return mesh;
    }

    /**
     * Two quads with identical coordinates but separate vertices.
     */
    private static Mesh3D coincidentComponents() {
        Mesh3D mesh = new Mesh3D();
        for (int i = 0; i < 2; i++) {
            mesh.addVertex(0, 0, 0);
            mesh.addVertex(2, 0, 0);
            mesh.addVertex(2, 2, 0);
            mesh.addVertex(0, 2, 0);
        }
        mesh.addFace(0, 1, 2, 3);
        mesh.addFace(4, 5, 6, 7);
        return mesh;
    }

    /**
     * A closed cube with a midpoint inserted on one edge. The midpoint is an
     * interior vertex of valence two.
     */
    private static Mesh3D cubeWithEdgeMidpoint() {
        Mesh3D mesh = new CubeCreator().create();
        mesh.addVertex(1, -1, 0);
        mesh.getFaceAt(0).indices = new int[] { 3, 0, 8, 1, 2 };
        mesh.getFaceAt(2).indices = new int[] { 1, 8, 0, 4, 5 };
        return mesh;
    }

    // ------------------------------------------------------------------
    // check helpers
    // ------------------------------------------------------------------

    private static void expectRejection(Suite suite, Runnable action,
            String label) {
        boolean rejected = false;
        try {
            action.run();
        } catch (IllegalArgumentException expected) {
            rejected = true;
        }
        suite.check(rejected, "expected IllegalArgumentException: " + label);
    }

    private static void checkVertexPositions(Suite suite, Mesh3D mesh,
            float[][] expected, String label) {
        suite.check(mesh.getVertexCount() == expected.length,
                label + ": vertex count");
        int count = Math.min(mesh.getVertexCount(), expected.length);
        for (int i = 0; i < count; i++) {
            Vector3f v = mesh.getVertexAt(i);
            suite.check(close(v, expected[i]),
                    label + ": vertex " + i + " at (" + expected[i][0] + ","
                            + expected[i][1] + "," + expected[i][2]
                            + ") but was " + position(v));
        }
    }

    private static void checkFaces(Suite suite, Mesh3D mesh, int[][] expected,
            String label) {
        suite.check(mesh.getFaceCount() == expected.length,
                label + ": face count");
        suite.check(canonicalFaces(mesh).equals(canonical(expected)),
                label + ": directed faces");
    }

    private static void checkBoundaryEdges(Suite suite, Mesh3D mesh,
            String[] expected, String label) {
        Set<String> boundary = boundaryEdges(mesh);
        suite.check(boundary.equals(new HashSet<>(Arrays.asList(expected))),
                label + ": directed boundary edges " + boundary);
    }

    private static void checkBoundaryEdgePositions(Suite suite, Mesh3D mesh,
            String[] expected, String label) {
        Set<String> boundary = boundaryEdgePositions(mesh);
        suite.check(boundary.equals(new HashSet<>(Arrays.asList(expected))),
                label + ": boundary edge positions " + boundary);
    }

    /**
     * Every directed edge of a well-formed result is referenced exactly once,
     * and every edge is either paired with its opposite direction (interior)
     * or unpaired (boundary).
     */
    private static void checkEdgePairing(Suite suite, Mesh3D mesh,
            String label) {
        Map<String, Integer> counts = new HashMap<>();
        for (Face3D face : mesh.getFaces()) {
            int n = face.indices.length;
            for (int i = 0; i < n; i++) {
                String edge = face.indices[i] + ">"
                        + face.indices[(i + 1) % n];
                counts.merge(edge, 1, Integer::sum);
            }
        }
        for (Map.Entry<String, Integer> entry : counts.entrySet())
            suite.check(entry.getValue() == 1,
                    label + ": directed edge " + entry.getKey()
                            + " is referenced " + entry.getValue() + " times");
    }

    private static void checkDistinctFaceVertices(Suite suite, Mesh3D mesh,
            String label) {
        for (Face3D face : mesh.getFaces()) {
            Set<Integer> distinct = new HashSet<>();
            for (int index : face.indices)
                distinct.add(index);
            suite.check(distinct.size() >= 3,
                    label + ": face " + face + " has at least three distinct"
                            + " vertices");
        }
    }

    private static void checkSameDirectedMesh(Suite suite, Mesh3D a, Mesh3D b,
            String label) {
        suite.check(a.getVertexCount() == b.getVertexCount(),
                label + ": vertex count");
        suite.check(a.getFaceCount() == b.getFaceCount(),
                label + ": face count");
        int vertices = Math.min(a.getVertexCount(), b.getVertexCount());
        for (int i = 0; i < vertices; i++) {
            Vector3f va = a.getVertexAt(i);
            Vector3f vb = b.getVertexAt(i);
            suite.check(va.x == vb.x && va.y == vb.y && va.z == vb.z,
                    label + ": vertex " + i);
        }
        int faces = Math.min(a.getFaceCount(), b.getFaceCount());
        for (int i = 0; i < faces; i++)
            suite.check(Arrays.equals(a.getFaceAt(i).indices,
                    b.getFaceAt(i).indices), label + ": face " + i);
    }

    // ------------------------------------------------------------------
    // mesh inspection helpers
    // ------------------------------------------------------------------

    private static String canonical(int[] indices) {
        int n = indices.length;
        int smallest = 0;
        for (int i = 1; i < n; i++)
            if (indices[i] < indices[smallest])
                smallest = i;
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < n; i++) {
            if (i > 0)
                sb.append(',');
            sb.append(indices[(smallest + i) % n]);
        }
        return sb.toString();
    }

    private static List<String> canonicalFaces(Mesh3D mesh) {
        List<String> faces = new ArrayList<>();
        for (Face3D face : mesh.getFaces())
            faces.add(canonical(face.indices));
        Collections.sort(faces);
        return faces;
    }

    private static List<String> canonical(int[][] faces) {
        List<String> list = new ArrayList<>();
        for (int[] face : faces)
            list.add(canonical(face));
        Collections.sort(list);
        return list;
    }

    private static Set<String> boundaryEdges(Mesh3D mesh) {
        Set<String> all = directedEdges(mesh);
        Set<String> boundary = new HashSet<>();
        for (String edge : all) {
            String[] parts = edge.split(">");
            if (!all.contains(parts[1] + ">" + parts[0]))
                boundary.add(edge);
        }
        return boundary;
    }

    private static Set<String> boundaryEdgePositions(Mesh3D mesh) {
        Set<String> positions = new HashSet<>();
        for (String edge : boundaryEdges(mesh)) {
            String[] parts = edge.split(">");
            Vector3f from = mesh.getVertexAt(Integer.parseInt(parts[0]));
            Vector3f to = mesh.getVertexAt(Integer.parseInt(parts[1]));
            positions.add(position(from) + ">" + position(to));
        }
        return positions;
    }

    private static Set<String> directedEdges(Mesh3D mesh) {
        Set<String> edges = new HashSet<>();
        for (Face3D face : mesh.getFaces()) {
            int n = face.indices.length;
            for (int i = 0; i < n; i++)
                edges.add(face.indices[i] + ">" + face.indices[(i + 1) % n]);
        }
        return edges;
    }

    private static List<String> vertexPositions(Mesh3D mesh) {
        List<String> positions = new ArrayList<>();
        for (int i = 0; i < mesh.getVertexCount(); i++)
            positions.add(position(mesh.getVertexAt(i)));
        return positions;
    }

    private static String position(Vector3f v) {
        return trim(v.x) + "," + trim(v.y) + "," + trim(v.z);
    }

    private static String trim(float value) {
        if (value == Math.rint(value) && Math.abs(value) < 1e7f)
            return Integer.toString((int) value);
        return Float.toString(value);
    }

    private static boolean close(Vector3f v, float[] expected) {
        return Math.abs(v.x - expected[0]) < 1e-5f
                && Math.abs(v.y - expected[1]) < 1e-5f
                && Math.abs(v.z - expected[2]) < 1e-5f;
    }

    private static boolean close(Vector3f v, Vector3f expected) {
        return v.distance(expected) < 1e-5f;
    }

    // ------------------------------------------------------------------
    // expected data
    // ------------------------------------------------------------------

    private static final Vector3f[] RING_VERTICES = {
            new Vector3f(0, 0, 0), new Vector3f(3, 0, 0),
            new Vector3f(3, 3, 0), new Vector3f(0, 3, 0),
            new Vector3f(1, 1, 0), new Vector3f(2, 1, 0),
            new Vector3f(2, 2, 0), new Vector3f(1, 2, 0) };

    private static final String[] SHEET_BOUNDARY = { "4>5", "5>6", "6>7",
            "7>4" };

    private static final String[] SHEET_BOUNDARY_POSITIONS = { "0,0,0>2,0,0",
            "2,0,0>2,2,0", "2,2,0>0,2,0", "0,2,0>0,0,0" };

    private static final String[] PATCH_BOUNDARY = { "8>9", "9>10", "10>13",
            "13>12", "12>11", "11>8" };

    private static final String[] RING_BOUNDARY = { "0,0,0>3,0,0",
            "3,0,0>3,3,0", "3,3,0>0,3,0", "0,3,0>0,0,0", "2,1,0>1,1,0",
            "2,2,0>2,1,0", "1,2,0>2,2,0", "1,1,0>1,2,0" };

    private static final String[] MIXED_BOUNDARY = { "0,0,0>1,0,0",
            "1,0,0>2,0,0", "2,0,0>1.5,1,0", "1.5,1,0>0.5,1,0",
            "0.5,1,0>0,0,0" };

    private static final float[][] SHEET_VERTICES = {
            { 1.5f, 0.5f, 0 }, { 1.5f, 1.5f, 0 }, { 0.5f, 1.5f, 0 },
            { 0.5f, 0.5f, 0 }, { 0, 0, 0 }, { 2, 0, 0 }, { 2, 2, 0 },
            { 0, 2, 0 } };

    private static final int[][] SHEET_FACES = { { 0, 1, 2, 3 },
            { 4, 5, 0, 3 }, { 5, 6, 1, 0 }, { 6, 7, 2, 1 }, { 7, 4, 3, 2 } };

    private static final float[][] PATCH_VERTICES = {
            { 0.75f, 0.25f, 0 }, { 0.75f, 0.75f, 0 }, { 0.25f, 0.75f, 0 },
            { 0.25f, 0.25f, 0 }, { 1.75f, 0.25f, 0 }, { 1.75f, 0.75f, 0 },
            { 1.25f, 0.75f, 0 }, { 1.25f, 0.25f, 0 }, { 0, 0, 0 },
            { 1, 0, 0 }, { 2, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 }, { 2, 1, 0 } };

    private static final int[][] PATCH_FACES = { { 0, 1, 2, 3 },
            { 4, 5, 6, 7 }, { 7, 0, 9 }, { 1, 6, 12 }, { 1, 0, 7, 6 },
            { 8, 9, 0, 3 }, { 12, 11, 2, 1 }, { 11, 8, 3, 2 },
            { 9, 10, 4, 7 }, { 10, 13, 5, 4 }, { 13, 12, 6, 5 } };

    private static final float[][] CUBE_VERTICES = {
            { 0.5f, -1, -0.5f }, { 0.5f, -1, 0.5f }, { -0.5f, -1, 0.5f },
            { -0.5f, -1, -0.5f }, { 0.5f, 1, 0.5f }, { 0.5f, 1, -0.5f },
            { -0.5f, 1, -0.5f }, { -0.5f, 1, 0.5f }, { 1, -0.5f, -0.5f },
            { 1, 0.5f, -0.5f }, { 1, 0.5f, 0.5f }, { 1, -0.5f, 0.5f },
            { 0.5f, 0.5f, 1 }, { -0.5f, 0.5f, 1 }, { -0.5f, -0.5f, 1 },
            { 0.5f, -0.5f, 1 }, { -1, 0.5f, -0.5f }, { -1, -0.5f, -0.5f },
            { -1, -0.5f, 0.5f }, { -1, 0.5f, 0.5f }, { -0.5f, 0.5f, -1 },
            { 0.5f, 0.5f, -1 }, { 0.5f, -0.5f, -1 }, { -0.5f, -0.5f, -1 } };

    private static final int[][] CUBE_FACES = { { 0, 1, 2, 3 },
            { 4, 5, 6, 7 }, { 8, 9, 10, 11 }, { 12, 13, 14, 15 },
            { 16, 17, 18, 19 }, { 20, 21, 22, 23 }, { 8, 0, 22 },
            { 1, 11, 15 }, { 2, 14, 18 }, { 3, 17, 23 }, { 5, 9, 21 },
            { 10, 4, 12 }, { 13, 7, 19 }, { 16, 6, 20 }, { 4, 7, 13, 12 },
            { 5, 4, 10, 9 }, { 7, 6, 16, 19 }, { 1, 0, 8, 11 },
            { 2, 1, 15, 14 }, { 3, 2, 18, 17 }, { 6, 5, 21, 20 },
            { 9, 8, 22, 21 }, { 11, 10, 12, 15 }, { 14, 13, 19, 18 },
            { 17, 16, 20, 23 }, { 0, 3, 23, 22 } };

}
