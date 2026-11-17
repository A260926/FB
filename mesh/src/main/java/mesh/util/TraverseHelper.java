package mesh.util;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;

import mesh.Edge3D;
import mesh.Face3D;
import mesh.Mesh3D;

/**
 * A helper class for traversing and manipulating 3D meshes.
 *
 * <p>This class provides efficient methods for accessing and modifying mesh
 * elements, such as edges and faces. By pre-processing the mesh and storing
 * edge relationships, it enables efficient traversal and modification
 * algorithms.
 *
 * <p>Adjacency is identified by vertex indices only. Vertices that merely
 * share the same coordinates are treated as distinct and are never welded
 * together.
 *
 * <p>Besides the single-step traversal methods, the helper exposes the
 * ordered fan of outgoing edges around a vertex via {@link #getVertexFan(int)}
 * and classifies vertices as boundary vertices via
 * {@link #isBoundaryVertex(int)}. A vertex fan follows the
 * {@link #getPairNext(int, int)} direction and contains one outgoing edge per
 * adjacent face. The fan of an interior vertex forms a closed loop and starts
 * with the outgoing edge whose end vertex index is smallest. The fan of a
 * boundary vertex is open: it starts with the outgoing edge whose face has
 * its incoming edge on the boundary and ends with the outgoing edge that lies
 * on the boundary itself. Isolated vertices have an empty fan and are not
 * boundary vertices.
 *
 * <p>The mesh is validated while it is mapped. The constructor throws an
 * {@link IllegalArgumentException} when the same directed edge is used by
 * more than one face, when an edge is shared by three or more faces, or when
 * the outgoing edges of a vertex form several fans that are not connected to
 * each other.
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
     * Maps an edge to the previous edge in a face.
     */
    private HashMap<Edge3D, Edge3D> edgePreviousMap;

    /**
     * Maps vertex indices to their outgoing edges.
     */
    private HashMap<Integer, Edge3D> outgoingEdgeMap;

    /**
     * Maps vertex indices to all of their outgoing edges, one per adjacent
     * face, in the order the faces were mapped.
     */
    private HashMap<Integer, List<Edge3D>> outgoingEdgesMap;

    /**
     * Counts how many faces reference an undirected edge.
     */
    private HashMap<Long, Integer> edgeUseCounts;

    /**
     * Maps vertex indices to their ordered fan of outgoing edges.
     */
    private HashMap<Integer, List<Edge3D>> vertexFanMap;

    /**
     * Indices of the vertices whose fan does not form a closed loop.
     */
    private HashSet<Integer> boundaryVertices;

    /**
     * A set of unique edges in the mesh.
     */
    private HashSet<Edge3D> edges;

    /**
     * Constructs a new TraverseHelper for the given mesh.
     *
     * @param mesh The 3D mesh to be traversed.
     * @throws IllegalArgumentException if the mesh contains a directed edge
     *         that is used by more than one face, an edge that is shared by
     *         three or more faces, or a vertex whose adjacent faces form
     *         multiple fans that are not connected to each other
     */
    public TraverseHelper(Mesh3D mesh) {
        setMesh(mesh);
        initialize();
        map();
        buildVertexFans();
    }

    /**
     * Initializes the internal data structures used for mesh traversal.
     */
    private void initialize() {
        edgeFaceMap = new HashMap<>();
        edgeNextMap = new HashMap<>();
        edgePreviousMap = new HashMap<>();
        outgoingEdgeMap = new HashMap<>();
        outgoingEdgesMap = new HashMap<>();
        edgeUseCounts = new HashMap<>();
        vertexFanMap = new HashMap<>();
        boundaryVertices = new HashSet<>();
        edges = new HashSet<>();
    }

    /**
     * Populates the internal data structures with edge and face relationships.
     */
    private void map() {
        for (Face3D face : mesh.getFaces())
            mapFace(face);
    }

    /**
     * Populates the internal data structures for a specific face.
     *
     * For each edge of the face, it creates an `Edge3D` object representing
     * the edge, and adds it to the appropriate maps.
     *
     * @param face The face to process.
     */
    private void mapFace(Face3D face) {
        int vertexCount = face.getVertexCount();
        for (int i = 0; i < vertexCount; i++) {
            int current = face.getIndexAt(i);
            int next = face.getIndexAt(i + 1);
            int nextNext = face.getIndexAt(i + 2);

            Edge3D edge = new Edge3D(current, next);
            Edge3D nextEdge = new Edge3D(next, nextNext);

            addEdgeFaceMapping(edge, face);
            addNextEdgeMapping(edge, nextEdge);
            addPreviousEdgeMapping(nextEdge, edge);
            addOutgoingEdgeMapping(current, edge);

            countEdgeUse(edge);
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
        edgeNextMap.put(edge, next);
    }

    /**
     * Adds a mapping from the given edge to the previous edge in a face.
     *
     * @param edge The edge to map.
     * @param previous The previous edge in the face.
     */
    private void addPreviousEdgeMapping(Edge3D edge, Edge3D previous) {
        edgePreviousMap.put(edge, previous);
    }

    /**
     * Adds a mapping from a vertex index to its outgoing edge.
     *
     * @param from The index of the vertex.
     * @param edge The outgoing edge from the vertex.
     */
    private void addOutgoingEdgeMapping(int from, Edge3D edge) {
        outgoingEdgeMap.put(from, edge);
        outgoingEdgesMap.computeIfAbsent(from, key -> new ArrayList<>())
                .add(edge);
    }

    /**
     * Adds a mapping from an edge to the face it belongs to.
     *
     * @param edge The edge.
     * @param face The face that the edge belongs to.
     * @throws IllegalArgumentException if the directed edge is already used
     *         by another face
     */
    private void addEdgeFaceMapping(Edge3D edge, Face3D face) {
        if (edgeFaceMap.containsKey(edge))
            throw new IllegalArgumentException(
                    "Directed edge " + edge + " is used by more than one face");
        edgeFaceMap.put(edge, face);
    }

    /**
     * Counts one more face reference for the undirected edge.
     *
     * @param edge The edge to count.
     * @throws IllegalArgumentException if the edge is shared by three or more
     *         faces
     */
    private void countEdgeUse(Edge3D edge) {
        long key = undirectedKey(edge);
        int count = edgeUseCounts.getOrDefault(key, 0) + 1;
        if (count > 2)
            throw new IllegalArgumentException(
                    "Edge " + edge + " is shared by three or more faces");
        edgeUseCounts.put(key, count);
    }

    /**
     * Returns a direction independent key for the given edge.
     *
     * @param edge The edge to key.
     * @return The key of the undirected edge.
     */
    private long undirectedKey(Edge3D edge) {
        int a = Math.min(edge.getFromIndex(), edge.getToIndex());
        int b = Math.max(edge.getFromIndex(), edge.getToIndex());
        return ((long) a << 32) | (b & 0xffffffffL);
    }

    /**
     * Collects a unique edge, ensuring it's not already present in the `edges`
     * set.
     *
     * @param edge The edge to be collected.
     */
    private void collectUniqueEdge(Edge3D edge) {
        if (edges.contains(edge.createPair()))
            return;
        edges.add(edge);
    }

    /**
     * Computes the ordered fan of outgoing edges for every vertex and
     * validates that the outgoing edges of each vertex form a single fan.
     *
     * @throws IllegalArgumentException if the outgoing edges of a vertex form
     *         multiple fans that are not connected to each other
     */
    private void buildVertexFans() {
        for (HashMap.Entry<Integer, List<Edge3D>> entry : outgoingEdgesMap
                .entrySet()) {
            int index = entry.getKey();
            List<Edge3D> outgoing = entry.getValue();
            List<Edge3D> fan = traceFan(index, outgoing);
            vertexFanMap.put(index, fan);
            if (!isClosedFan(fan))
                boundaryVertices.add(index);
        }
    }

    /**
     * Traces the fan of outgoing edges around a vertex in the
     * {@link #getPairNext(int, int)} direction.
     *
     * <p>The trace first walks backwards to the first edge of an open fan. A
     * closed loop is rotated so that it starts with the outgoing edge whose
     * end vertex index is smallest.
     *
     * @param index The index of the vertex.
     * @param outgoing All outgoing edges of the vertex.
     * @return The ordered fan of outgoing edges.
     * @throws IllegalArgumentException if the outgoing edges form multiple
     *         fans that are not connected to each other
     */
    private List<Edge3D> traceFan(int index, List<Edge3D> outgoing) {
        int size = outgoing.size();
        Edge3D first = outgoing.get(0);

        Edge3D start = first;
        boolean closed = false;
        Edge3D current = first;
        for (int steps = 0; steps <= size; steps++) {
            Edge3D previous = getFanPrevious(current);
            if (previous == null) {
                start = current;
                break;
            }
            if (previous.equals(first)) {
                closed = true;
                break;
            }
            current = previous;
            if (steps == size)
                throw new IllegalArgumentException(
                        "Vertex " + index + " has a malformed fan");
        }

        List<Edge3D> fan = new ArrayList<>();
        current = start;
        for (int steps = 0; steps <= size; steps++) {
            fan.add(current);
            Edge3D next = getPairNext(current.getFromIndex(),
                    current.getToIndex());
            if (next == null || next.equals(start))
                break;
            current = next;
            if (steps == size)
                throw new IllegalArgumentException(
                        "Vertex " + index + " has a malformed fan");
        }

        if (fan.size() != size)
            throw new IllegalArgumentException("Vertex " + index
                    + " has multiple fans that are not connected to each other");

        if (closed)
            rotateToSmallestToIndex(fan);
        return fan;
    }

    /**
     * Returns the outgoing edge that precedes the given edge in the fan around
     * its start vertex, or {@code null} if there is none.
     *
     * @param edge The outgoing edge to follow backwards.
     * @return The preceding outgoing edge or {@code null}.
     */
    private Edge3D getFanPrevious(Edge3D edge) {
        Edge3D incoming = edgePreviousMap.get(edge);
        if (incoming == null)
            return null;
        Edge3D previous = incoming.createPair();
        return edgeFaceMap.containsKey(previous) ? previous : null;
    }

    /**
     * Checks whether the given fan forms a closed loop.
     *
     * @param fan The fan to check.
     * @return {@code true} if the fan forms a closed loop.
     */
    private boolean isClosedFan(List<Edge3D> fan) {
        Edge3D last = fan.get(fan.size() - 1);
        Edge3D next = getPairNext(last.getFromIndex(), last.getToIndex());
        return next != null && next.equals(fan.get(0));
    }

    /**
     * Rotates a closed fan so that it starts with the outgoing edge whose end
     * vertex index is smallest.
     *
     * @param fan The fan to rotate in place.
     */
    private void rotateToSmallestToIndex(List<Edge3D> fan) {
        int smallest = 0;
        for (int i = 1; i < fan.size(); i++)
            if (fan.get(i).getToIndex() < fan.get(smallest).getToIndex())
                smallest = i;
        if (smallest > 0) {
            List<Edge3D> head = new ArrayList<>(fan.subList(0, smallest));
            fan.subList(0, smallest).clear();
            fan.addAll(head);
        }
    }

    /**
     * Returns the outgoing edge from the given vertex index.
     *
     * @param index The index of the vertex.
     * @return The outgoing edge.
     */
    public Edge3D getOutgoing(int index) {
        return outgoingEdgeMap.get(index);
    }

    /**
     * Returns the ordered fan of outgoing edges around the given vertex. The
     * fan follows the {@link #getPairNext(int, int)} direction and contains
     * one outgoing edge per adjacent face.
     *
     * <p>The fan of an interior vertex forms a closed loop and starts with
     * the outgoing edge whose end vertex index is smallest. The fan of a
     * boundary vertex starts with the outgoing edge whose face has its
     * incoming edge on the boundary and ends with the outgoing edge that lies
     * on the boundary itself. An isolated vertex has an empty fan.
     *
     * @param index The index of the vertex.
     * @return The ordered fan of outgoing edges, possibly empty.
     */
    public List<Edge3D> getVertexFan(int index) {
        return new ArrayList<>(
                vertexFanMap.getOrDefault(index, List.of()));
    }

    /**
     * Returns whether the given vertex lies on the boundary of the mesh. A
     * vertex is a boundary vertex if its fan of outgoing edges does not form
     * a closed loop. Isolated vertices are not boundary vertices.
     *
     * @param index The index of the vertex.
     * @return {@code true} if the vertex is a boundary vertex.
     */
    public boolean isBoundaryVertex(int index) {
        return boundaryVertices.contains(index);
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
        edgePreviousMap.clear();
        outgoingEdgeMap.clear();
        outgoingEdgesMap.clear();
        edgeUseCounts.clear();
        vertexFanMap.clear();
        boundaryVertices.clear();
        edges.clear();
    }

    /**
     * Sets the mesh to traverse to the specified mesh.
     *
     * @param mesh The mesh to traverse.
     */
    private void setMesh(Mesh3D mesh) {
        this.mesh = mesh;
    }

}
