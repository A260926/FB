package mesh.util;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

import mesh.Edge3D;
import mesh.Face3D;
import mesh.Mesh3D;

/**
 * A helper class for traversing and manipulating 3D meshes.
 *
 * This class provides efficient methods for accessing and modifying mesh
 * elements, such as edges and faces. By pre-processing the mesh and storing
 * edge relationships, it enables efficient traversal and modification
 * algorithms.
 *
 * <p>All adjacency is established by vertex <b>index</b>. Independent vertices
 * that happen to share the same coordinates are never welded together.
 *
 * <p>The half-edge relations built for every face are:
 *
 * <ul>
 *   <li>the face a directed edge belongs to,</li>
 *   <li>the next directed edge inside that face,</li>
 *   <li>the {@link #getPairNext(int, int) pair-next} edge, i.e. the next edge
 *       of the reversed edge, which rotates around the edge's start vertex to
 *       the adjacent face.</li>
 * </ul>
 *
 * <p>The outgoing edges of a vertex form at most one <em>fan</em>. A fan is
 * either closed (the vertex is surrounded by faces) or open (the vertex lies
 * on a boundary). The constructor rejects meshes where the same directed edge
 * is claimed more than once, an undirected edge is shared by more than two
 * faces, or a vertex carries several disconnected fans.
 */
public class TraverseHelper {

    /**
     * The mesh to traverse.
     */
    private Mesh3D mesh;

    /**
     * Maps edges to the faces they belong to.
     */
    private HashMap<Edge3D, Face3D> edgeFaceMap;

    /**
     * Maps an edge to the next edge in a face.
     */
    private HashMap<Edge3D, Edge3D> edgeNextMap;

    /**
     * Maps vertex indices to their last encountered outgoing edge.
     */
    private HashMap<Integer, Edge3D> outgoingEdgeMap;

    /**
     * A set of unique edges in the mesh.
     */
    private HashSet<Edge3D> edges;

    /**
     * Maps a vertex index to its ordered vertex fan.
     */
    private HashMap<Integer, List<Edge3D>> fanMap;

    /**
     * Contains the indices of vertices with an open (boundary) fan.
     */
    private HashSet<Integer> boundaryVertices;

    /**
     * Constructs a new TraverseHelper for the given mesh.
     *
     * @param mesh The 3D mesh to be traversed.
     * @throws IllegalArgumentException if the mesh contains a duplicate
     *         directed edge, an edge shared by more than two faces, or a
     *         vertex with several disconnected fans.
     */
    public TraverseHelper(Mesh3D mesh) {
        this.mesh = mesh;
        initialize();
        map();
        buildFans();
    }

    /**
     * Initializes the internal data structures used for mesh traversal.
     */
    private void initialize() {
        edgeFaceMap = new HashMap<>();
        edgeNextMap = new HashMap<>();
        outgoingEdgeMap = new HashMap<>();
        edges = new HashSet<>();
        fanMap = new HashMap<>();
        boundaryVertices = new HashSet<>();
    }

    /**
     * Populates the internal data structures with edge and face relationships.
     */
    private void map() {
        for (Face3D face : mesh.getFaces()) {
            mapFace(face);
        }
    }

    /**
     * Populates the internal data structures for a specific face.
     *
     * For each edge of the face, it creates an {@code Edge3D} object
     * representing the edge, and adds it to the appropriate maps.
     *
     * @param face The face to process.
     */
    private void mapFace(Face3D face) {
        int vertexCount = face.getVertexCount();
        for (int i = 0; i < vertexCount; i++) {
            int current = face.getIndexAt(i);
            int next = face.getIndexAt(i + 1);
            int nextNext = face.getIndexAt(i + 2);
            validateVertexIndex(current, face);
            validateVertexIndex(next, face);
            validateVertexIndex(nextNext, face);

            Edge3D edge = new Edge3D(current, next);
            Edge3D nextEdge = new Edge3D(next, nextNext);

            addEdgeFaceMapping(edge, face);
            addNextEdgeMapping(edge, nextEdge);
            addOutgoingEdgeMapping(current, edge);

            collectUniqueEdge(edge);
        }
    }

    /**
     * Adds a mapping from the given edge to the next edge in a face.
     *
     * @param edge The current edge.
     * @param next The next edge in the face.
     */
    private void addNextEdgeMapping(Edge3D edge, Edge3D next) {
        if (edgeNextMap.containsKey(edge)) {
            throw new IllegalArgumentException(
                    "Duplicate directed edge " + edge
                            + " is claimed by more than one face.");
        }
        edgeNextMap.put(edge, next);
    }

    /**
     * Records an outgoing edge of a vertex. Keeping the legacy overwrite
     * semantics, this returns the last encountered outgoing edge, which the
     * single-step traversal loop relies on as its starting edge.
     *
     * @param from The index of the vertex.
     * @param edge The outgoing edge from the vertex.
     */
    private void addOutgoingEdgeMapping(int from, Edge3D edge) {
        outgoingEdgeMap.put(from, edge);
    }

    /**
     * Adds a mapping from an edge to the face it belongs to.
     *
     * @param edge The edge.
     * @param face The face that the edge belongs to.
     */
    private void addEdgeFaceMapping(Edge3D edge, Face3D face) {
        if (edgeFaceMap.containsKey(edge)) {
            throw new IllegalArgumentException(
                    "Duplicate directed edge " + edge
                            + " is claimed by more than one face.");
        }
        edgeFaceMap.put(edge, face);
    }

    /**
     * Collects a unique edge, ensuring it's not already present in the
     * {@code edges} set.
     *
     * @param edge The edge to be collected.
     */
    private void collectUniqueEdge(Edge3D edge) {
        if (edges.contains(edge.createPair())) {
            return;
        }
        edges.add(edge);
    }

    /**
     * Builds the ordered vertex fan of every referenced vertex and verifies
     * that the outgoing edges of a vertex form a single connected fan.
     */
    private void buildFans() {
        HashMap<Integer, List<Edge3D>> outgoing = new HashMap<>();
        for (Edge3D edge : edgeFaceMap.keySet()) {
            outgoing.computeIfAbsent(edge.getFromIndex(),
                    key -> new ArrayList<>()).add(edge);
        }

        for (int vertex = 0; vertex < mesh.getVertexCount(); vertex++) {
            fanMap.put(vertex, new ArrayList<>());
        }

        for (Map.Entry<Integer, List<Edge3D>> entry : outgoing.entrySet()) {
            int vertex = entry.getKey();
            List<Edge3D> fan = traceFan(vertex, entry.getValue());
            fanMap.put(vertex, fan);
        }
    }

    /**
     * Traces the single fan of the given vertex by following the
     * {@link #getPairNext(int, int) pair-next} relation.
     *
     * @param vertex the vertex index.
     * @param outgoing the outgoing edges of the vertex.
     * @return the ordered fan, rotated to the edge with the smallest end
     *         index for a closed fan.
     */
    private List<Edge3D> traceFan(int vertex, List<Edge3D> outgoing) {
        Set<Edge3D> remaining = new HashSet<>(outgoing);

        // Resolve the pair-next edge for every outgoing edge of this vertex
        // once; a null value points at a boundary.
        Map<Edge3D, Edge3D> pairNext = new HashMap<>();
        Set<Edge3D> hasPredecessor = new HashSet<>();
        for (Edge3D edge : outgoing) {
            Edge3D next = pairNextOrNull(edge);
            pairNext.put(edge, next);
            if (next != null) {
                hasPredecessor.add(next);
            }
        }

        Edge3D start = null;
        for (Edge3D edge : outgoing) {
            if (!hasPredecessor.contains(edge)) {
                if (start != null) {
                    throw new IllegalArgumentException(
                            "Vertex " + vertex
                                    + " carries multiple disconnected fans.");
                }
                start = edge;
            }
        }

        List<Edge3D> fan = new ArrayList<>();
        if (start != null) {
            Edge3D current = start;
            while (current != null) {
                if (!remaining.remove(current)) {
                    throw new IllegalArgumentException(
                            "Vertex " + vertex
                                    + " carries multiple disconnected fans.");
                }
                fan.add(current);
                current = pairNext.get(current);
            }
            boundaryVertices.add(vertex);
        } else {
            Edge3D first = outgoing.get(0);
            Edge3D current = first;
            do {
                if (!remaining.remove(current)) {
                    throw new IllegalArgumentException(
                            "Vertex " + vertex
                                    + " carries multiple disconnected fans.");
                }
                fan.add(current);
                current = pairNext.get(current);
            } while (current != null && !current.equals(first));

            if (current == null || !remaining.isEmpty()) {
                throw new IllegalArgumentException(
                        "Vertex " + vertex
                                + " carries multiple disconnected fans.");
            }

            int minimum = 0;
            for (int i = 1; i < fan.size(); i++) {
                if (fan.get(i).getToIndex() < fan.get(minimum).getToIndex()) {
                    minimum = i;
                }
            }
            List<Edge3D> rotated = new ArrayList<>(fan.size());
            rotated.addAll(fan.subList(minimum, fan.size()));
            rotated.addAll(fan.subList(0, minimum));
            fan = rotated;
        }

        if (!remaining.isEmpty()) {
            throw new IllegalArgumentException(
                    "Vertex " + vertex
                            + " carries multiple disconnected fans.");
        }
        return fan;
    }

    /**
     * Returns the pair-next edge of the given edge, or {@code null} if the
     * reversed edge does not belong to any face.
     */
    private Edge3D pairNextOrNull(Edge3D edge) {
        return edgeNextMap.get(edge.createPair());
    }

    /**
     * Checks that a face index references an existing vertex.
     */
    private void validateVertexIndex(int index, Face3D face) {
        if (index < 0 || index >= mesh.getVertexCount()) {
            throw new IllegalArgumentException(
                    "Face " + face + " references vertex index " + index
                            + " outside of the mesh vertex list.");
        }
    }

    /**
     * Returns the outgoing edge from the given vertex index.
     *
     * @param index The index of the vertex.
     * @return The last encountered outgoing edge, or {@code null} for an
     *         isolated vertex.
     */
    public Edge3D getOutgoing(int index) {
        return outgoingEdgeMap.get(index);
    }

    /**
     * Returns the face associated with the given edge.
     *
     * @param fromIndex The index of the first vertex of the edge.
     * @param toIndex   The index of the second vertex of the edge.
     * @return The face associated with the edge.
     */
    public Face3D getFaceByEdge(int fromIndex, int toIndex) {
        Edge3D edge = new Edge3D(fromIndex, toIndex);
        return edgeFaceMap.get(edge);
    }

    /**
     * Returns the next edge in the face, following the given edge.
     *
     * @param fromIndex The index of the first vertex of the edge.
     * @param toIndex   The index of the second vertex of the edge.
     * @return The next edge in the face.
     */
    public Edge3D getNext(int fromIndex, int toIndex) {
        Edge3D edge = new Edge3D(fromIndex, toIndex);
        return edgeNextMap.get(edge);
    }

    /**
     * Returns the reverse edge of the given edge.
     *
     * @param fromIndex The index of the first vertex of the edge.
     * @param toIndex   The index of the second vertex of the edge.
     * @return The reverse edge.
     */
    public Edge3D getPair(int fromIndex, int toIndex) {
        return new Edge3D(toIndex, fromIndex);
    }

    /**
     * Returns the next edge of the reverse edge.
     *
     * @param fromIndex The index of the first vertex of the edge.
     * @param toIndex   The index of the second vertex of the edge.
     * @return The next edge of the reverse edge.
     */
    public Edge3D getPairNext(int fromIndex, int toIndex) {
        Edge3D edge = new Edge3D(toIndex, fromIndex);
        return edgeNextMap.get(edge);
    }

    /**
     * Returns the ordered vertex fan of the given vertex.
     *
     * <p>The fan contains one outgoing edge per adjacent face and is ordered
     * according to {@link #getPairNext(int, int)}. A closed fan starts with
     * the outgoing edge whose end vertex index is the smallest. An open fan
     * starts at the face whose incoming edge lies on the boundary and ends at
     * the face whose outgoing edge lies on the boundary.
     *
     * <p>A valid isolated vertex has an empty fan.
     *
     * @param vertexIndex the index of the vertex.
     * @return a newly allocated list with the ordered outgoing edges.
     */
    public List<Edge3D> getVertexFan(int vertexIndex) {
        List<Edge3D> fan = fanMap.get(vertexIndex);
        if (fan == null) {
            throw new IndexOutOfBoundsException(
                    "No vertex with index " + vertexIndex);
        }
        return new ArrayList<>(fan);
    }

    /**
     * Returns whether the given vertex lies on a boundary.
     *
     * <p>A vertex is a boundary vertex if its fan is open, i.e. if it is
     * incident to at least one boundary edge. Valid isolated vertices are not
     * considered boundary vertices.
     *
     * @param vertexIndex the index of the vertex.
     * @return {@code true} if the vertex fan is open.
     */
    public boolean isBoundaryVertex(int vertexIndex) {
        if (!fanMap.containsKey(vertexIndex)) {
            throw new IndexOutOfBoundsException(
                    "No vertex with index " + vertexIndex);
        }
        return boundaryVertices.contains(vertexIndex);
    }

    /**
     * Returns a list of all unique edges in the mesh.
     *
     * @return A list of unique edges.
     */
    public List<Edge3D> getAllEdges() {
        return new ArrayList<Edge3D>(edges);
    }

    /**
     * Clears the internal data structures.
     */
    public void clear() {
        edgeFaceMap.clear();
        edgeNextMap.clear();
        outgoingEdgeMap.clear();
        edges.clear();
        fanMap.clear();
        boundaryVertices.clear();
    }

}
