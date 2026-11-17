package mesh.modifier.subdivision;

import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Vector;

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
 * <p>Besides closed meshes, this modifier also subdivides meshes with open
 * boundaries, such as sheets with an outer boundary or inner holes. The
 * boundary of the mesh is preserved by every iteration: boundary vertices and
 * boundary edges keep their positions and stay the boundary of the refined
 * mesh. New geometry is only inserted between the boundary and the face
 * corner polygons, so the result of an iteration can be subdivided again.
 *
 * <p>A mesh can be subdivided if every face references at least three
 * distinct vertex indices and every non-isolated interior vertex is adjacent
 * to at least three faces. If a positive number of iterations is requested
 * and the mesh does not meet these requirements, an
 * {@link IllegalArgumentException} is thrown before the mesh is modified.
 * Requesting zero iterations returns the mesh unchanged.
 *
 * @see http://graphics.cs.ucdavis.edu/education/
 *      CAGDNotes/Doo-Sabin/Doo-Sabin.html
 *
 * @version 0.1, 4 December 2017
 */
public class DooSabinModifier implements IMeshModifier {

    private int subdivisions;

    private Mesh3D source;

    private Mesh3D target;

    private HashSet<Edge3D> edges;

    private HashMap<Edge3D, Face3D> edgeToFaceMap;

    private HashMap<VertexFacePair, Integer> vertexFaceMap;

    private HashMap<Integer, Integer> boundaryVertexMap;

    /**
     * Constructs a new instance of this modifier.
     */
    public DooSabinModifier() {
        this(1);
    }

    /**
     * Constructs a new instance of this modifier with the specified number of
     * subdivision iterations.
     *
     * @param subdivisions the number of iterations
     */
    public DooSabinModifier(int subdivisions) {
        this.subdivisions = subdivisions;
        edgeToFaceMap = new HashMap<>();
        edges = new HashSet<>();
        vertexFaceMap = new HashMap<VertexFacePair, Integer>();
        boundaryVertexMap = new HashMap<>();
    }

    /**
     * For each original face, create new vertices and connect the new vertices
     * that have been generated for each original vertex of the face.
     */
    private void createAndConnectNewVertices() {
        List<Face3D> faces = source.getFaces();
        for (Face3D face : faces) {
            Vector3f center = MeshGeometryUtil.calculateFaceCenter(source, face);
            int[] indices = new int[face.indices.length];
            int n = indices.length;
            for (int i = 0; i < face.indices.length; i++) {
                Vector3f res = calculateNewPoint(face, center, i);
                int index = target.getVertexCount();
                target.add(res);
                vertexFaceMap.put(
                        new VertexFacePair(face.indices[(i + 1) % n], face),
                        index);
                indices[i] = index;
            }
            target.add(new Face3D(indices));
        }
    }

    /**
     * Copies the boundary vertices of the original mesh to the target mesh.
     * Boundary vertices keep their position and stay part of the boundary of
     * the refined mesh.
     */
    private void copyBoundaryVertices(TraverseHelper helper) {
        for (int i = 0; i < source.getVertexCount(); i++) {
            if (helper.isBoundaryVertex(i)) {
                boundaryVertexMap.put(i, target.getVertexCount());
                target.add(new Vector3f(source.getVertexAt(i)));
            }
        }
    }

    /**
     * For each vertex of the original mesh, connect the new points that have
     * been generated for the faces that are adjacent to this vertex.
     *
     * <p>An interior vertex yields a closed vertex face. A boundary vertex
     * with at least two adjacent faces yields a vertex face that connects the
     * original vertex with the ordered fan of its corner points. A boundary
     * vertex with a single adjacent face would only produce a degenerate
     * two-corner face, so no face is created for it.
     */
    private void createFacesFromAdjacentVertices(TraverseHelper helper) {
        for (int i = 0; i < source.getVertexCount(); i++) {
            List<Edge3D> fan = helper.getVertexFan(i);
            if (fan.isEmpty())
                continue;
            boolean boundary = helper.isBoundaryVertex(i);
            if (boundary && fan.size() < 2)
                continue;
            Vector<Integer> indices = new Vector<Integer>();
            if (boundary)
                indices.add(boundaryVertexMap.get(i));
            for (Edge3D edge : fan) {
                Face3D face = helper.getFaceByEdge(edge.getFromIndex(),
                        edge.getToIndex());
                indices.add(vertexFaceMap.get(new VertexFacePair(i, face)));
            }
            addFace(indices);
        }
    }

    /**
     * For each edge of the original mesh, connect the new points that have
     * been generated for the faces that are adjacent to the respective edge.
     *
     * <p>An interior edge yields a quad that connects the corner points of
     * the two adjacent faces. A boundary edge keeps the original edge and
     * yields a strip quad that connects it to the corner points of its only
     * adjacent face.
     */
    private void createFacesAdjacentToEdge() {
        for (Edge3D edge : edges) {
            Face3D face0 = edgeToFaceMap.get(edge);
            Face3D face1 = edgeToFaceMap.get(edge.createPair());
            int from = edge.getFromIndex();
            int to = edge.getToIndex();
            if (face1 == null) {
                int fromIndex = boundaryVertexMap.get(from);
                int toIndex = boundaryVertexMap.get(to);
                int cornerFrom = vertexFaceMap
                        .get(new VertexFacePair(from, face0));
                int cornerTo = vertexFaceMap
                        .get(new VertexFacePair(to, face0));
                target.addFace(fromIndex, toIndex, cornerTo, cornerFrom);
            } else {
                int idx0 = vertexFaceMap.get(new VertexFacePair(to, face0));
                int idx1 = vertexFaceMap.get(new VertexFacePair(from, face0));
                int idx2 = vertexFaceMap.get(new VertexFacePair(from, face1));
                int idx3 = vertexFaceMap.get(new VertexFacePair(to, face1));
                target.addFace(idx0, idx1, idx2, idx3);
            }
        }
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

    private void addFace(Vector<Integer> indices) {
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

    /**
     * Validates that every face references at least three distinct vertex
     * indices.
     *
     * @throws IllegalArgumentException if a face references less than three
     *         distinct vertex indices
     */
    private void validateFaces() {
        HashSet<Integer> distinct = new HashSet<>();
        for (Face3D face : source.getFaces()) {
            distinct.clear();
            for (int index : face.indices)
                distinct.add(index);
            if (distinct.size() < 3)
                throw new IllegalArgumentException(
                        "Face references less than three distinct vertex"
                                + " indices: " + face);
        }
    }

    /**
     * Validates that every non-isolated interior vertex is adjacent to at
     * least three faces.
     *
     * @param helper the traverse helper for the source mesh
     * @throws IllegalArgumentException if an interior vertex is adjacent to
     *         less than three faces
     */
    private void validateVertices(TraverseHelper helper) {
        for (int i = 0; i < source.getVertexCount(); i++) {
            int faceCount = helper.getVertexFan(i).size();
            if (faceCount == 0 || helper.isBoundaryVertex(i))
                continue;
            if (faceCount < 3)
                throw new IllegalArgumentException("Interior vertex " + i
                        + " is adjacent to less than three faces");
        }
    }

    private void subdivide(Mesh3D mesh) {
        source = mesh;
        target = new Mesh3D();

        clear();

        validateFaces();
        TraverseHelper helper = new TraverseHelper(source);
        validateVertices(helper);

        mapEdgesToFaces();
        createAndConnectNewVertices();
        copyBoundaryVertices(helper);
        createFacesFromAdjacentVertices(helper);
        createFacesAdjacentToEdge();

        this.source.clearFaces();
        this.source.clearVertices();
        this.source.addFaces(target.getFaces());
        this.source.addVertices(target.getVertices());
    }

    private void clear() {
        edges.clear();
        edgeToFaceMap.clear();
        vertexFaceMap.clear();
        boundaryVertexMap.clear();
    }

    @Override
    public Mesh3D modify(Mesh3D mesh) {
        for (int i = 0; i < subdivisions; i++) {
            subdivide(mesh);
        }
        return mesh;
    }

    /**
     * A pair of a vertex index and a face. Identifies the corner point that
     * has been generated for the vertex of the face. Adjacency is identified
     * by vertex index, so coincident vertices stay separate.
     */
    private static class VertexFacePair {

        private final int vertexIndex;

        private final Face3D face;

        private VertexFacePair(int vertexIndex, Face3D face) {
            this.vertexIndex = vertexIndex;
            this.face = face;
        }

        @Override
        public int hashCode() {
            final int prime = 31;
            int result = 1;
            result = prime * result + vertexIndex;
            result = prime * result + ((face == null) ? 0 : face.hashCode());
            return result;
        }

        @Override
        public boolean equals(Object obj) {
            if (this == obj)
                return true;
            if (obj == null)
                return false;
            if (getClass() != obj.getClass())
                return false;
            VertexFacePair other = (VertexFacePair) obj;
            return vertexIndex == other.vertexIndex && face == other.face;
        }

    }

}
