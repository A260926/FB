package regression;

import java.util.ArrayList;
import java.util.Arrays;
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
 * Regression checks for the open-surface support of {@link DooSabinModifier}
 * and the fan traversal offered by {@link TraverseHelper}.
 *
 * <p>The checks are organized into the four groups {@code fans},
 * {@code boundary}, {@code repeat} and {@code compat}. They cover single
 * faces, faces sharing an edge, rings with an inner hole, the closed cube,
 * spatially coinciding but disconnected components, illegal face fans and
 * the rejection of a bivalent interior vertex created by inserting a
 * midpoint into a cube edge.
 */
public final class OpenDooSabinChecks {

    private OpenDooSabinChecks() {
    }

    /**
     * Registers all regression cases with the given suite.
     *
     * @param suite the suite to register the cases with.
     */
    public static void register(Suite suite) {
        registerFanCases(suite);
        registerBoundaryCases(suite);
        registerRepeatCases(suite);
        registerCompatibilityCases(suite);
    }

    // ------------------------------------------------------------------
    // fans
    // ------------------------------------------------------------------

    private static void registerFanCases(Suite suite) {
        suite.add("fans", "closedFanOfCube", () -> {
            Mesh3D mesh = new CubeCreator().create();
            TraverseHelper helper = new TraverseHelper(mesh);
            for (int vertex = 0; vertex < mesh.getVertexCount(); vertex++) {
                List<Edge3D> fan = helper.getVertexFan(vertex);
                suite.check(fan.size() == 3,
                        "cube vertex is incident to three faces");
                suite.check(!helper.isBoundaryVertex(vertex),
                        "cube vertex is interior");
                int minimumEnd = fan.stream().mapToInt(Edge3D::getToIndex)
                        .min().orElse(-1);
                suite.check(fan.get(0).getToIndex() == minimumEnd,
                        "closed fan starts at the outgoing edge with the "
                                + "smallest end index");
                for (int i = 0; i < fan.size(); i++) {
                    Edge3D expected = helper.getPairNext(
                            fan.get(i).getFromIndex(),
                            fan.get(i).getToIndex());
                    suite.check(expected.equals(fan.get((i + 1) % fan.size())),
                            "fan edges follow the pair-next direction");
                }
            }
        });

        suite.add("fans", "openFanOfSheet", () -> {
            Mesh3D mesh = twoQuadStrip();
            TraverseHelper helper = new TraverseHelper(mesh);
            for (int vertex = 0; vertex < 6; vertex++) {
                suite.check(helper.isBoundaryVertex(vertex),
                        "strip vertex " + vertex + " is on the boundary");
            }
            // The two middle vertices are each shared by both faces.
            for (int vertex : new int[] {1, 4}) {
                List<Edge3D> fan = helper.getVertexFan(vertex);
                suite.check(fan.size() == 2,
                        "shared boundary vertex has a two-edge fan");
                Edge3D first = fan.get(0);
                Edge3D last = fan.get(fan.size() - 1);
                Face3D firstFace = helper.getFaceByEdge(first.getFromIndex(),
                        first.getToIndex());
                int incoming = previousVertex(firstFace, vertex);
                suite.check(helper.getFaceByEdge(vertex, incoming) == null
                        && helper.getFaceByEdge(incoming, vertex) != null,
                        "open fan starts at the face whose incoming edge is "
                                + "on the boundary");
                suite.check(helper.getFaceByEdge(last.getToIndex(),
                        last.getFromIndex()) == null
                        && helper.getFaceByEdge(last.getFromIndex(),
                                last.getToIndex()) != null,
                        "open fan ends at the face whose outgoing edge is "
                                + "on the boundary");
            }
            // A single-face corner still has a one-edge open fan.
            TraverseHelper single = new TraverseHelper(singleQuad());
            suite.check(single.getVertexFan(0).size() == 1
                    && single.isBoundaryVertex(0),
                    "single-face corner has a one-edge boundary fan");
        });

        suite.add("fans", "isolatedVertex", () -> {
            Mesh3D mesh = singleQuad();
            mesh.addVertex(9, 9, 9);
            TraverseHelper helper = new TraverseHelper(mesh);
            suite.check(helper.getVertexFan(4).isEmpty(),
                    "isolated vertex has an empty fan");
            suite.check(!helper.isBoundaryVertex(4),
                    "isolated vertex is not a boundary vertex");
            suite.check(helper.getOutgoing(4) == null,
                    "isolated vertex has no outgoing edge");
        });

        suite.add("fans", "illegalFansAreRejected", () -> {
            // Two faces claim the same directed edge 0 -> 1.
            Mesh3D duplicateDirected = new Mesh3D();
            duplicateDirected.addVertex(0, 0, 0);
            duplicateDirected.addVertex(1, 0, 0);
            duplicateDirected.addVertex(2, 0, 0);
            duplicateDirected.addVertex(0, 1, 0);
            duplicateDirected.addVertex(2, 1, 0);
            duplicateDirected.addFace(0, 1, 2);
            duplicateDirected.addFace(0, 1, 4);
            expectIllegalArgument(suite,
                    () -> new TraverseHelper(duplicateDirected),
                    "duplicate directed edge is rejected");

            // Three faces share the undirected edge (0, 1).
            Mesh3D threeFaces = new Mesh3D();
            threeFaces.addVertex(0, 0, 0);
            threeFaces.addVertex(1, 0, 0);
            threeFaces.addVertex(0, 1, 0);
            threeFaces.addVertex(1, 1, 1);
            threeFaces.addVertex(0, -1, 1);
            threeFaces.addFace(0, 1, 2);
            threeFaces.addFace(1, 0, 3);
            threeFaces.addFace(0, 1, 4);
            expectIllegalArgument(suite,
                    () -> new TraverseHelper(threeFaces),
                    "edge shared by three faces is rejected");

            // Two triangles touch only at vertex 0, forming two
            // disconnected fans around that vertex.
            Mesh3D disconnectedFans = new Mesh3D();
            disconnectedFans.addVertex(0, 0, 0);
            disconnectedFans.addVertex(1, 0, 0);
            disconnectedFans.addVertex(0, 1, 0);
            disconnectedFans.addVertex(-1, 0, 0);
            disconnectedFans.addVertex(0, -1, 0);
            disconnectedFans.addFace(0, 1, 2);
            disconnectedFans.addFace(0, 3, 4);
            expectIllegalArgument(suite,
                    () -> new TraverseHelper(disconnectedFans),
                    "multiple disconnected fans at a vertex are rejected");
        });

        suite.add("fans", "coincidingComponentsStaySeparate", () -> {
            Mesh3D mesh = singleQuad();
            int offset = mesh.getVertexCount();
            Mesh3D second = singleQuad();
            for (Vector3f vertex : second.getVertices()) {
                mesh.addVertex(vertex.x, vertex.y, vertex.z);
            }
            for (Face3D face : second.getFaces()) {
                int[] indices = Arrays.copyOf(face.indices,
                        face.indices.length);
                for (int i = 0; i < indices.length; i++) {
                    indices[i] += offset;
                }
                mesh.addFace(indices);
            }
            TraverseHelper helper = new TraverseHelper(mesh);
            for (int vertex = 0; vertex < 4; vertex++) {
                List<Edge3D> fan = helper.getVertexFan(vertex);
                suite.check(fan.size() == 1
                        && fan.get(0).getFromIndex() == vertex
                        && fan.get(0).getToIndex() < 4,
                        "fan only references the own component by index");
                List<Edge3D> other = helper.getVertexFan(vertex + offset);
                suite.check(other.size() == 1
                        && other.get(0).getFromIndex() == vertex + offset,
                        "coinciding component keeps its own fan");
            }
        });
    }

    // ------------------------------------------------------------------
    // boundary
    // ------------------------------------------------------------------

    private static void registerBoundaryCases(Suite suite) {
        suite.add("boundary", "singleQuad", () -> {
            Mesh3D mesh = singleQuad();
            new DooSabinModifier(1).modify(mesh);
            suite.check(mesh.getVertexCount() == 8,
                    "single quad produces four corner and four boundary "
                            + "vertices");
            suite.check(mesh.getFaceCount() == 5,
                    "single quad produces one corner polygon and four "
                            + "connection strips");
            suite.check(allFacesHaveThreeDistinctVertices(mesh),
                    "no output face has fewer than three distinct vertices");
            float[][] corners = quadCoordinates();
            for (int i = 0; i < 4; i++) {
                suite.check(hasDirectedEdge(mesh, corners[i],
                        corners[(i + 1) % 4]),
                        "original boundary edge is retained with its "
                                + "direction");
            }
            // Expected Doo-Sabin corner points, in generation slot order.
            float[][] expectedCorners = {
                    {1.5f, 0.5f, 0}, {1.5f, 1.5f, 0},
                    {0.5f, 1.5f, 0}, {0.5f, 0.5f, 0}};
            Face3D cornerPolygon = mesh.getFaceAt(0);
            suite.check(cornerPolygon.getVertexCount() == 4,
                    "the original face becomes a quad corner polygon");
            for (int i = 0; i < 4; i++) {
                Vector3f point = mesh.getVertexAt(cornerPolygon.getIndexAt(i));
                suite.check(samePoint(point, expectedCorners[i]),
                        "corner point " + i + " keeps the original Doo-Sabin "
                                + "geometry");
            }
        });

        suite.add("boundary", "twoFacesSharingAnEdge", () -> {
            Mesh3D before = twoQuadStrip();
            Mesh3D mesh = before.copy();
            new DooSabinModifier(1).modify(mesh);
            suite.check(allFacesHaveThreeDistinctVertices(mesh),
                    "strip output faces are valid");
            suite.check(countBoundaryEdges(mesh) == 6,
                    "the strip keeps its six boundary edges");
            suite.check(internalEdgesHaveOppositeReferences(mesh),
                    "the shared edge is referenced twice in opposite "
                            + "directions");
            for (int vertex = 0; vertex < before.getVertexCount(); vertex++) {
                suite.check(containsPoint(mesh,
                        before.getVertexAt(vertex)),
                        "boundary vertex is retained at its original "
                                + "position");
            }
            // The two shared boundary vertices each connect the ordered
            // corner fan with one triangle per adjacent face pair.
            suite.check(mesh.getFaceCount() == 11,
                    "two corner polygons, six boundary strips, two internal "
                            + "edge quads and one boundary-vertex triangle");
        });

        suite.add("boundary", "ringWithInnerHole", () -> {
            Mesh3D mesh = annulus();
            new DooSabinModifier(1).modify(mesh);
            suite.check(allFacesHaveThreeDistinctVertices(mesh),
                    "annulus output faces are valid");
            suite.check(countBoundaryEdges(mesh) == 8,
                    "outer and inner loop contribute eight boundary edges");
            float[][] outer = annulusOuterCoordinates();
            float[][] inner = annulusInnerCoordinates();
            int[][] outerEdges = {{0, 1}, {1, 2}, {2, 3}, {3, 0}};
            int[][] innerEdges = {{1, 0}, {2, 1}, {3, 2}, {0, 3}};
            for (int[] edge : outerEdges) {
                suite.check(hasDirectedEdge(mesh, outer[edge[0]],
                        outer[edge[1]]),
                        "outer boundary edge keeps its direction");
            }
            for (int[] edge : innerEdges) {
                suite.check(hasDirectedEdge(mesh, inner[edge[0]],
                        inner[edge[1]]),
                        "inner hole boundary edge keeps its direction");
            }
            new DooSabinModifier(1).modify(mesh);
            suite.check(allFacesHaveThreeDistinctVertices(mesh)
                    && countBoundaryEdges(mesh) == 8,
                    "the second round preserves both boundary loops");
        });

        suite.add("boundary", "bivalentInteriorVertexIsRejected", () -> {
            Mesh3D mesh = cubeWithEdgeMidpoint();
            int vertexCount = mesh.getVertexCount();
            int faceCount = mesh.getFaceCount();
            expectIllegalArgument(suite,
                    () -> new DooSabinModifier(1).modify(mesh),
                    "a bivalent interior vertex is rejected");
            suite.check(mesh.getVertexCount() == vertexCount
                    && mesh.getFaceCount() == faceCount,
                    "the mesh is not modified when validation fails");
        });

        suite.add("boundary", "degenerateFaceIsRejected", () -> {
            Mesh3D mesh = new Mesh3D();
            mesh.addVertex(0, 0, 0);
            mesh.addVertex(1, 0, 0);
            mesh.addVertex(1, 1, 0);
            mesh.addFace(0, 1, 0);
            int vertexCount = mesh.getVertexCount();
            int faceCount = mesh.getFaceCount();
            expectIllegalArgument(suite,
                    () -> new DooSabinModifier(1).modify(mesh),
                    "a face with fewer than three distinct vertices is "
                            + "rejected");
            suite.check(mesh.getVertexCount() == vertexCount
                    && mesh.getFaceCount() == faceCount
                    && mesh.getFaceAt(0).indices.length == 3,
                    "the rejected mesh is left untouched");
        });
    }

    // ------------------------------------------------------------------
    // repeat
    // ------------------------------------------------------------------

    private static void registerRepeatCases(Suite suite) {
        suite.add("repeat", "twoRoundsEqualOneRoundTwice", () -> {
            Mesh3D once = annulus();
            new DooSabinModifier(2).modify(once);
            Mesh3D twice = annulus();
            new DooSabinModifier(1).modify(twice);
            new DooSabinModifier(1).modify(twice);
            suite.check(directedMeshDump(once).equals(directedMeshDump(twice)),
                    "one two-round subdivision equals two one-round "
                            + "subdivisions as directed meshes");
        });

        suite.add("repeat", "openSheetCanBeSubdividedRepeatedly", () -> {
            Mesh3D mesh = singleQuad();
            for (int round = 1; round <= 3; round++) {
                new DooSabinModifier(1).modify(mesh);
                suite.check(allFacesHaveThreeDistinctVertices(mesh),
                        "round " + round + " produces only valid faces");
                suite.check(countBoundaryEdges(mesh) == 4,
                        "round " + round + " keeps the four boundary edges");
                suite.check(internalEdgesHaveOppositeReferences(mesh),
                        "round " + round + " keeps opposite internal edge "
                                + "references");
            }
        });

        suite.add("repeat", "coincidingComponentsAreKept", () -> {
            Mesh3D mesh = singleQuad();
            int offset = mesh.getVertexCount();
            Mesh3D second = singleQuad();
            for (Vector3f vertex : second.getVertices()) {
                mesh.addVertex(vertex.x, vertex.y, vertex.z);
            }
            for (Face3D face : second.getFaces()) {
                int[] indices = Arrays.copyOf(face.indices,
                        face.indices.length);
                for (int i = 0; i < indices.length; i++) {
                    indices[i] += offset;
                }
                mesh.addFace(indices);
            }
            new DooSabinModifier(1).modify(mesh);
            suite.check(mesh.getVertexCount() == 16,
                    "coinciding components are not welded together");
            suite.check(mesh.getFaceCount() == 10,
                    "coinciding components keep their independent faces");
            new DooSabinModifier(1).modify(mesh);
            suite.check(allFacesHaveThreeDistinctVertices(mesh),
                    "coinciding components remain subdividable");
        });

        suite.add("repeat", "zeroRoundsLeavesMeshUntouched", () -> {
            Mesh3D mesh = singleQuad();
            String before = directedMeshDump(mesh);
            Mesh3D returned = new DooSabinModifier(0).modify(mesh);
            suite.check(returned == mesh,
                    "modify returns the same mesh instance");
            suite.check(directedMeshDump(mesh).equals(before),
                    "zero rounds do not change the mesh content");
        });
    }

    // ------------------------------------------------------------------
    // compat
    // ------------------------------------------------------------------

    private static void registerCompatibilityCases(Suite suite) {
        suite.add("compat", "closedCubeRoundOne", () -> {
            Mesh3D mesh = new CubeCreator().create();
            new DooSabinModifier(1).modify(mesh);
            suite.check(mesh.getVertexCount() == 24,
                    "cube round one keeps 24 vertices");
            suite.check(mesh.getFaceCount() == 26,
                    "cube round one keeps 26 faces");
            suite.check(allFacesHaveThreeDistinctVertices(mesh),
                    "all cube output faces are valid");
            suite.check(countBoundaryEdges(mesh) == 0,
                    "the closed cube has no boundary edges");
        });

        suite.add("compat", "closedCubeRoundTwo", () -> {
            Mesh3D mesh = new CubeCreator().create();
            new DooSabinModifier(2).modify(mesh);
            suite.check(mesh.getVertexCount() == 96,
                    "cube round two keeps 96 vertices");
            suite.check(mesh.getFaceCount() == 98,
                    "cube round two keeps 98 faces");
            suite.check(allFacesHaveThreeDistinctVertices(mesh)
                    && countBoundaryEdges(mesh) == 0,
                    "cube round two stays a closed valid mesh");
        });

        suite.add("compat", "legacySingleStepTraversal", () -> {
            Mesh3D mesh = new CubeCreator().create();
            TraverseHelper helper = new TraverseHelper(mesh);
            suite.check(helper.getAllEdges().size() == 12,
                    "the cube still exposes its twelve unique edges");
            Edge3D outgoing = helper.getOutgoing(0);
            Edge3D next = helper.getNext(outgoing.getFromIndex(),
                    outgoing.getToIndex());
            Edge3D pair = helper.getPair(outgoing.getFromIndex(),
                    outgoing.getToIndex());
            suite.check(pair.getFromIndex() == outgoing.getToIndex()
                    && pair.getToIndex() == outgoing.getFromIndex(),
                    "getPair still reverses an edge");
            suite.check(helper.getFaceByEdge(outgoing.getFromIndex(),
                    outgoing.getToIndex()) != null,
                    "getFaceByEdge still resolves the owning face");
            Edge3D edge = outgoing;
            int steps = 0;
            do {
                edge = helper.getPairNext(edge.getFromIndex(),
                        edge.getToIndex());
                steps++;
            } while (!outgoing.equals(edge) && steps <= 12);
            suite.check(steps == 3 && outgoing.equals(edge),
                    "the pair-next loop still closes after the vertex "
                            + "valency");
            suite.check(next.getFromIndex() == outgoing.getToIndex(),
                    "getNext still walks within the face");
        });

        suite.add("compat", "mixedTriangleAndQuadSheet", () -> {
            Mesh3D mesh = new Mesh3D();
            mesh.addVertex(0, 0, 0);
            mesh.addVertex(1, 0, 0);
            mesh.addVertex(0, 1, 0);
            mesh.addVertex(1, 1, 0);
            mesh.addVertex(2, 0, 0);
            mesh.addFace(0, 1, 2);
            mesh.addFace(1, 4, 3, 2);
            new DooSabinModifier(1).modify(mesh);
            suite.check(allFacesHaveThreeDistinctVertices(mesh),
                    "mixed triangle/quad output faces are valid");
            suite.check(countBoundaryEdges(mesh) == 5,
                    "the mixed sheet keeps its five boundary edges");
            new DooSabinModifier(1).modify(mesh);
            suite.check(allFacesHaveThreeDistinctVertices(mesh),
                    "the mixed sheet can be subdivided again");
        });
    }

    // ------------------------------------------------------------------
    // fixture builders
    // ------------------------------------------------------------------

    private static Mesh3D singleQuad() {
        Mesh3D mesh = new Mesh3D();
        for (float[] point : quadCoordinates()) {
            mesh.addVertex(point[0], point[1], point[2]);
        }
        mesh.addFace(0, 1, 2, 3);
        return mesh;
    }

    private static float[][] quadCoordinates() {
        return new float[][] {
                {0, 0, 0}, {2, 0, 0}, {2, 2, 0}, {0, 2, 0}};
    }

    private static Mesh3D twoQuadStrip() {
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

    private static Mesh3D annulus() {
        Mesh3D mesh = new Mesh3D();
        for (float[] point : annulusOuterCoordinates()) {
            mesh.addVertex(point[0], point[1], point[2]);
        }
        for (float[] point : annulusInnerCoordinates()) {
            mesh.addVertex(point[0], point[1], point[2]);
        }
        mesh.addFace(0, 1, 5, 4);
        mesh.addFace(1, 2, 6, 5);
        mesh.addFace(2, 3, 7, 6);
        mesh.addFace(3, 0, 4, 7);
        return mesh;
    }

    private static float[][] annulusOuterCoordinates() {
        return new float[][] {
                {0, 0, 0}, {4, 0, 0}, {4, 4, 0}, {0, 4, 0}};
    }

    private static float[][] annulusInnerCoordinates() {
        return new float[][] {
                {1, 1, 0}, {3, 1, 0}, {3, 3, 0}, {1, 3, 0}};
    }

    private static Mesh3D cubeWithEdgeMidpoint() {
        Mesh3D mesh = new CubeCreator().create();
        Vector3f first = mesh.getVertexAt(0);
        Vector3f second = mesh.getVertexAt(1);
        int midpoint = mesh.getVertexCount();
        mesh.addVertex((first.x + second.x) / 2f,
                (first.y + second.y) / 2f,
                (first.z + second.z) / 2f);
        List<Face3D> originalFaces = mesh.getFaces();
        mesh.clearFaces();
        for (Face3D face : originalFaces) {
            List<Integer> indices = new ArrayList<>();
            for (int index : face.indices) {
                indices.add(index);
            }
            if (indices.equals(List.of(3, 0, 1, 2))) {
                mesh.addFace(3, 0, midpoint, 1, 2);
            } else if (indices.equals(List.of(1, 0, 4, 5))) {
                mesh.addFace(1, midpoint, 0, 4, 5);
            } else {
                mesh.addFace(face.indices);
            }
        }
        return mesh;
    }

    // ------------------------------------------------------------------
    // assertion and mesh analysis helpers
    // ------------------------------------------------------------------

    private static void expectIllegalArgument(Suite suite,
            Suite.Body body, String message) throws Exception {
        boolean thrown = false;
        try {
            body.run();
        } catch (IllegalArgumentException expected) {
            thrown = true;
        }
        suite.check(thrown, message);
    }

    /**
     * Returns the vertex immediately preceding the given vertex within the
     * face's directed loop.
     */
    private static int previousVertex(Face3D face, int vertexIndex) {
        for (int i = 0; i < face.getVertexCount(); i++) {
            if (face.getIndexAt(i) == vertexIndex) {
                return face.getIndexAt(i + face.getVertexCount() - 1);
            }
        }
        throw new IllegalArgumentException(
                "Vertex " + vertexIndex + " is not part of " + face);
    }

    private static boolean allFacesHaveThreeDistinctVertices(Mesh3D mesh) {
        for (Face3D face : mesh.getFaces()) {
            Set<Integer> distinct = new HashSet<>();
            for (int index : face.indices) {
                distinct.add(index);
            }
            if (distinct.size() < 3) {
                return false;
            }
        }
        return true;
    }

    private static List<int[]> directedEdges(Mesh3D mesh) {
        List<int[]> edges = new ArrayList<>();
        for (Face3D face : mesh.getFaces()) {
            for (int i = 0; i < face.indices.length; i++) {
                edges.add(new int[] {
                        face.indices[i],
                        face.indices[(i + 1) % face.indices.length]});
            }
        }
        return edges;
    }

    private static int findVertex(Mesh3D mesh, float[] coordinates) {
        for (int i = 0; i < mesh.getVertexCount(); i++) {
            if (samePoint(mesh.getVertexAt(i), coordinates)) {
                return i;
            }
        }
        return -1;
    }

    private static boolean hasDirectedEdge(Mesh3D mesh, float[] from,
            float[] to) {
        int fromIndex = findVertex(mesh, from);
        int toIndex = findVertex(mesh, to);
        if (fromIndex < 0 || toIndex < 0) {
            return false;
        }
        for (int[] edge : directedEdges(mesh)) {
            if (edge[0] == fromIndex && edge[1] == toIndex) {
                return true;
            }
        }
        return false;
    }

    private static int countBoundaryEdges(Mesh3D mesh) {
        Map<String, Integer> directedCount = new HashMap<>();
        for (int[] edge : directedEdges(mesh)) {
            directedCount.merge(edge[0] + "->" + edge[1], 1, Integer::sum);
        }
        Set<String> counted = new HashSet<>();
        int boundaryEdges = 0;
        for (int[] edge : directedEdges(mesh)) {
            String undirected = Math.min(edge[0], edge[1]) + "-"
                    + Math.max(edge[0], edge[1]);
            if (!counted.add(undirected)) {
                continue;
            }
            int total = directedCount.getOrDefault(
                    edge[0] + "->" + edge[1], 0)
                    + directedCount.getOrDefault(
                            edge[1] + "->" + edge[0], 0);
            if (total == 1) {
                boundaryEdges++;
            }
        }
        return boundaryEdges;
    }

    private static boolean internalEdgesHaveOppositeReferences(
            Mesh3D mesh) {
        Map<String, Integer> directedCount = new HashMap<>();
        for (int[] edge : directedEdges(mesh)) {
            directedCount.merge(edge[0] + "->" + edge[1], 1, Integer::sum);
        }
        for (Map.Entry<String, Integer> entry : directedCount.entrySet()) {
            String[] endpoints = entry.getKey().split("->");
            Integer reverseCount = directedCount
                    .get(endpoints[1] + "->" + endpoints[0]);
            if (reverseCount != null
                    && !reverseCount.equals(entry.getValue())) {
                return false;
            }
        }
        return true;
    }

    private static boolean containsPoint(Mesh3D mesh, Vector3f point) {
        for (int i = 0; i < mesh.getVertexCount(); i++) {
            if (samePoint(mesh.getVertexAt(i),
                    new float[] {point.x, point.y, point.z})) {
                return true;
            }
        }
        return false;
    }

    private static boolean samePoint(Vector3f point, float[] expected) {
        return Float.compare(point.x, expected[0]) == 0
                && Float.compare(point.y, expected[1]) == 0
                && Float.compare(point.z, expected[2]) == 0;
    }

    private static String directedMeshDump(Mesh3D mesh) {
        StringBuilder builder = new StringBuilder();
        for (int i = 0; i < mesh.getVertexCount(); i++) {
            Vector3f vertex = mesh.getVertexAt(i);
            builder.append(Float.floatToRawIntBits(vertex.x)).append(',')
                    .append(Float.floatToRawIntBits(vertex.y)).append(',')
                    .append(Float.floatToRawIntBits(vertex.z)).append(';');
        }
        builder.append('|');
        for (Face3D face : mesh.getFaces()) {
            builder.append(Arrays.toString(face.indices)).append(';');
        }
        return builder.toString();
    }
}
