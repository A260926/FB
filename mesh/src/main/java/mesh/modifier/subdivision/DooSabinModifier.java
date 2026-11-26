package mesh.modifier.subdivision;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

import math.Vector3f;
import mesh.Edge3D;
import mesh.Face3D;
import mesh.Mesh3D;
import mesh.geometry.MeshGeometryUtil;
import mesh.modifier.IMeshModifier;
import mesh.util.TraverseHelper;

/**
 * An implementation of the Doo-Sabin subdivision surface. This type of
 * subdivision surface is based on a generalization of bi-quadratic uniform
 * B-splines and was developed in 1978 by Daniel Doo and Malcolm Sabin.
 *
 * <p>In addition to closed meshes, this implementation supports open
 * surfaces, including sheets and face rings with inner holes. Every
 * subdivision round produces:
 *
 * <ul>
 *   <li>one <b>face corner polygon</b> per original face, connecting the new
 *       corner points generated for the face vertices;</li>
 *   <li>one quad per original edge: an internal edge connects the corner
 *       points of the two adjacent faces, while a boundary edge keeps the
 *       original boundary vertices and only adds a connection strip to the
 *       corner polygon of its single adjacent face;</li>
 *   <li>one closed <b>vertex face</b> per interior vertex, or a fan of
 *       triangles around a boundary vertex with more than one adjacent face
 *       (boundary vertices with a single adjacent face produce no digon).</li>
 * </ul>
 *
 * <p>Boundary vertices and boundary edges are retained verbatim, so the
 * boundary positions and the connectivity of inner and outer loops stay
 * unchanged across rounds. Every output face has at least three distinct
 * vertices, which allows the result to be subdivided again directly.
 *
 * <p>Subdividable inputs must satisfy:
 *
 * <ul>
 *   <li>every face references at least three distinct vertex indices, and</li>
 *   <li>every non-isolated interior vertex is incident to at least three
 *       faces.</li>
 * </ul>
 *
 * Invalid inputs throw an {@link IllegalArgumentException} before the mesh is
 * modified. Adjacency is always resolved by vertex index, so spatially
 * coinciding but disconnected components are never welded together.
 *
 * @see http://graphics.cs.ucdavis.edu/education/
 *      CAGDNotes/Doo-Sabin/Doo-Sabin.html
 */
public class DooSabinModifier implements IMeshModifier {

    private int subdivisions;

    private Mesh3D source;

    private Mesh3D target;

    private TraverseHelper helper;

    private HashSet<Edge3D> edges;

    private HashMap<Edge3D, Face3D> edgeToFaceMap;

    /**
     * Maps the pair (source face, source vertex) to the target index of the
     * corner point generated for that pair.
     */
    private IdentityHashMap<Face3D, HashMap<Integer, Integer>>
            cornerByFaceVertex;

    /**
     * Maps retained boundary/isolated source vertex indices to target indices.
     */
    private Map<Integer, Integer> retainedVertexIndex;

    /**
     * Constructs a new instance of this modifier with one subdivision
     * iteration.
     */
    public DooSabinModifier() {
        this(1);
    }

    /**
     * Constructs a new instance of this modifier with the specified number of
     * subdivision iterations.
     *
     * @param subdivisions the number of iterations, zero leaves the mesh
     *                     untouched.
     * @throws IllegalArgumentException if {@code subdivisions} is negative.
     */
    public DooSabinModifier(int subdivisions) {
        if (subdivisions < 0) {
            throw new IllegalArgumentException(
                    "subdivisions must not be negative (was " + subdivisions
                            + ")");
        }
        this.subdivisions = subdivisions;
    }

    /**
     * Applies the Doo-Sabin subdivision in-place and returns the same mesh
     * instance. With zero subdivisions the mesh is returned unchanged.
     */
    @Override
    public Mesh3D modify(Mesh3D mesh) {
        if (mesh == null) {
            throw new IllegalArgumentException("mesh must not be null");
        }
        for (int i = 0; i < subdivisions; i++) {
            subdivide(mesh);
        }
        return mesh;
    }

    /**
     * Validates the input before any mutation takes place.
     */
    private void validate() {
        for (Face3D face : source.getFaces()) {
            Set<Integer> distinct = new HashSet<>();
            for (int index : face.indices) {
                if (index < 0 || index >= source.getVertexCount()) {
                    throw new IllegalArgumentException(
                            "Face references vertex index " + index
                                    + " outside of the mesh vertex list.");
                }
                distinct.add(index);
            }
            if (distinct.size() < 3) {
                throw new IllegalArgumentException(
                        "Every face must reference at least three distinct "
                                + "vertices, but found " + distinct.size()
                                + " in " + face + ".");
            }
        }

        for (int vertex = 0; vertex < source.getVertexCount(); vertex++) {
            List<Edge3D> fan = helper.getVertexFan(vertex);
            if (fan.isEmpty() || helper.isBoundaryVertex(vertex)) {
                continue;
            }
            if (fan.size() < 3) {
                throw new IllegalArgumentException(
                        "Non-isolated interior vertex " + vertex
                                + " is incident to only " + fan.size()
                                + " faces; at least three are required.");
            }
        }
    }

    private void subdivide(Mesh3D mesh) {
        source = mesh;
        target = new Mesh3D();
        edges = new HashSet<>();
        edgeToFaceMap = new HashMap<>();
        cornerByFaceVertex = new IdentityHashMap<>();
        retainedVertexIndex = new HashMap<>();

        helper = new TraverseHelper(source);
        validate();

        mapEdgesToFaces();
        createAndConnectNewVertices();
        retainVertices();
        createFacesFromAdjacentVertices();
        createFacesAdjacentToEdge();

        source.clearFaces();
        source.clearVertices();
        source.addFaces(target.getFaces());
        source.addVertices(target.getVertices());
    }

    /**
     * For each original face, create new vertices and connect the new vertices
     * that have been generated for each original vertex of the face.
     */
    private void createAndConnectNewVertices() {
        for (Face3D face : source.getFaces()) {
            Vector3f center = MeshGeometryUtil.calculateFaceCenter(source,
                    face);
            int[] indices = new int[face.indices.length];
            HashMap<Integer, Integer> cornerForVertex = new HashMap<>();
            for (int i = 0; i < face.indices.length; i++) {
                Vector3f res = calculateNewPoint(face, center, i);
                int index = target.getVertexCount();
                target.add(res);
                indices[i] = index;
                // Generation slot i stores the point for the vertex at
                // face position i + 1.
                cornerForVertex.put(face.getIndexAt(i + 1), index);
            }
            cornerByFaceVertex.put(face, cornerForVertex);
            target.add(new Face3D(indices));
        }
    }

    /**
     * Copies boundary vertices and isolated vertices into the target mesh and
     * records their new indices. Interior vertices are replaced by corner
     * points and are not retained.
     */
    private void retainVertices() {
        for (int vertex = 0; vertex < source.getVertexCount(); vertex++) {
            List<Edge3D> fan = helper.getVertexFan(vertex);
            if (fan.isEmpty() || helper.isBoundaryVertex(vertex)) {
                Vector3f point = source.getVertexAt(vertex);
                retainedVertexIndex.put(vertex, target.getVertexCount());
                target.add(point);
            }
        }
    }

    /**
     * For each vertex of the original mesh, connect the new points that have
     * been generated for the faces adjacent to this vertex.
     *
     * <p>Interior vertices keep the original closed vertex face. Boundary
     * vertices receive a fan of triangles between consecutive adjacent faces
     * (unless they have only a single adjacent face, in which case emitting a
     * face would create a digon).
     */
    private void createFacesFromAdjacentVertices() {
        for (int i = 0; i < source.getVertexCount(); i++) {
            List<Edge3D> fan = helper.getVertexFan(i);
            if (fan.isEmpty()) {
                // Valid isolated vertex: nothing connects to it.
                continue;
            }

            if (helper.isBoundaryVertex(i)) {
                if (fan.size() == 1) {
                    continue;
                }
                for (int j = 0; j < fan.size() - 1; j++) {
                    Face3D first = helper.getFaceByEdge(
                            fan.get(j).getFromIndex(),
                            fan.get(j).getToIndex());
                    Face3D second = helper.getFaceByEdge(
                            fan.get(j + 1).getFromIndex(),
                            fan.get(j + 1).getToIndex());
                    target.addFace(retainedVertexIndex.get(i),
                            cornerIndexOf(second, i),
                            cornerIndexOf(first, i));
                }
                continue;
            }

            Edge3D outgoingEdge = helper.getOutgoing(i);
            Edge3D edge = outgoingEdge;
            List<Integer> indices = new ArrayList<Integer>();
            do {
                Face3D face = helper.getFaceByEdge(edge.getFromIndex(),
                        edge.getToIndex());
                indices.add(cornerIndexOf(face, i));
                edge = helper.getPairNext(edge.getFromIndex(),
                        edge.getToIndex());
            } while (!outgoingEdge.equals(edge));
            addReversedFace(indices);
        }
    }

    /**
     * For each edge of the original mesh, connect the new points that have
     * been generated for the faces adjacent to the respective edge.
     *
     * <p>Internal edges produce the original quad between the two adjacent
     * faces. Boundary edges retain the original boundary vertices and add a
     * connection strip to the single adjacent face.
     */
    private void createFacesAdjacentToEdge() {
        for (Edge3D edge : edges) {
            Face3D face0 = edgeToFaceMap.get(edge);
            Face3D face1 = edgeToFaceMap.get(edge.createPair());
            int from = edge.getFromIndex();
            int to = edge.getToIndex();

            if (face1 == null) {
                // Boundary edge: retain the original boundary edge (keeping
                // its from->to direction) and connect its endpoints to the
                // corner polygon of the single adjacent face.
                target.addFace(cornerIndexOf(face0, to),
                        cornerIndexOf(face0, from),
                        retainedVertexIndex.get(from),
                        retainedVertexIndex.get(to));
                continue;
            }

            target.addFace(cornerIndexOf(face0, to),
                    cornerIndexOf(face0, from),
                    cornerIndexOf(face1, from),
                    cornerIndexOf(face1, to));
        }
    }

    /**
     * Returns the target index of the corner point generated for the given
     * source vertex within the given source face.
     */
    private int cornerIndexOf(Face3D face, int vertexIndex) {
        Integer index = cornerByFaceVertex.get(face).get(vertexIndex);
        if (index == null) {
            throw new IllegalArgumentException(
                    "Vertex " + vertexIndex + " is not part of " + face + ".");
        }
        return index;
    }

    /**
     * The average of four particular points taken in a polygon.
     *
     * <ul>
     * <li>the vertex for which the new point is being defined</li>
     * <li>the two edge points (the midpoints of the edges that are adjacent to
     * this vertex in the polygon)</li>
     * <li>and the face point (average of the vertices of the polygon)</li>
     * </ul>
     *
     * @param face   the face (polygon) the original vertex belongs to
     * @param center the face point (average of the vertices of the polygon)
     * @param i      the index of the first adjacent vertex (references to the
     *               vertex list of the mesh)
     * @return the average of the four particular points as new {@link Vector3f}
     */
    private Vector3f calculateNewPoint(Face3D face, Vector3f center, int i) {
        int n = face.indices.length;
        Vector3f v0 = source.getVertexAt(face.indices[i % n]);
        Vector3f v1 = source.getVertexAt(face.indices[(i + 1) % n]);
        Vector3f v2 = source.getVertexAt(face.indices[(i + 2) % n]);
        Vector3f e1 = v0.add(v1).mult(0.5f);
        Vector3f e2 = v1.add(v2).mult(0.5f);
        return center.add(v1).add(e1).add(e2).divide(4);
    }

    private void addReversedFace(List<Integer> indices) {
        int[] a = new int[indices.size()];
        for (int j = 0; j < a.length; j++) {
            a[a.length - j - 1] = indices.get(j);
        }
        target.add(new Face3D(a));
    }

    private void mapEdgesToFaces() {
        for (Face3D face : source.getFaces()) {
            for (int i = 0; i < face.indices.length; i++) {
                int a = face.indices[i];
                int b = face.indices[(i + 1) % face.indices.length];
                Edge3D pair0 = new Edge3D(a, b);
                Edge3D pair1 = new Edge3D(b, a);
                if (!edges.contains(pair1)) {
                    edges.add(pair0);
                }
                edgeToFaceMap.put(pair0, face);
            }
        }
    }

}
