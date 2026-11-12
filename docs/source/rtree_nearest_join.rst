.. _rtree_nearest_neighbor_join:

============================================================================
Batch nearest-neighbor join between two R-Trees (``RTree::nearestNeighborJoin``)
============================================================================

``RTree::nearestNeighborJoin`` associates every record of a *left* R-Tree
with the ``k`` nearest records of a *right* R-Tree in a single shared tree
traversal. It is a library-level alternative to issuing one
``nearestNeighborQuery`` per left record from the application: node MBRs of
the right tree are read once and reused for every left record of a leaf.

Distance and semantics
======================

* The distance of a pair is the minimum Euclidean distance between the two
  stored MBRs, computed axis by axis. Projections that overlap or touch
  contribute zero on that axis, so intersecting or contacting MBRs have
  distance zero. Geometry stored inside the payload is never interpreted.
* Each left record independently receives its own nearest right records.
  Ties at the ``k``-th distance are all reported, so a left record may
  receive more than ``k`` partners. If the right tree holds fewer than ``k``
  records, all of them are reported.
* The association is directed. Passing the same tree for both arguments is
  legal and pairs every record with itself as well; ``(a, b)`` and
  ``(b, a)`` are distinct ordered pairs.
* Identifiers are only unique inside one index. The same numeric identifier
  in the left and right trees does not mean the records are related.
* An empty tree on either side produces no pairs.

Validation happens before any node is read and before any callback, so a
rejected call never invokes the visitor. The function throws
``Tools::IllegalArgumentException`` when:

* either argument is not a plain ``RTree`` (an MVRTree/TPRTree is rejected),
* the two trees have different dimensionalities, or
* ``k`` is zero.

The two trees are expected to use the same coordinate units.

Example
=======

.. code-block:: cpp

    #include <spatialindex/SpatialIndex.h>
    #include <memory>
    #include <cstdint>

    using namespace SpatialIndex;

    class ETLJoinVisitor : public RTree::IJoinVisitor
    {
    public:
        void visitPair(const IData& left, const IData& right,
                       double distance) override
        {
            // Copy everything that must survive the call. The IData
            // references (and any bytes returned by getData) are only valid
            // until this method returns.
            const id_type leftId = left.getIdentifier();
            const id_type rightId = right.getIdentifier();

            uint32_t len = 0;
            uint8_t* bytes = nullptr;
            right.getData(len, &bytes);
            // ... store (leftId, rightId, distance, copied payload) ...
            delete[] bytes;
        }
    };

    void joinTrees(ISpatialIndex& leftTree, ISpatialIndex& rightTree)
    {
        ETLJoinVisitor visitor;
        const uint32_t k = 4;
        RTree::nearestNeighborJoin(leftTree, rightTree, k, visitor);
    }

Lifecycle and threading notes
=============================

* The ``IJoinVisitor`` object is owned by the caller and must outlive the
  ``nearestNeighborJoin`` call. The trees and their storage managers must
  likewise stay alive for the whole call.
* Inside ``visitPair`` the two ``IData`` references, their shapes and their
  payload bytes are valid only for the duration of that call. Retain
  identifiers, copy the shape (for example with ``getShape``) and copy the
  payload bytes (``getData`` returns a freshly allocated buffer that the
  caller frees) if the information is needed later, for example in a
  downstream ETL stage.
* Every ordered pair is reported exactly once. Callback order follows the
  internal traversal (currently ascending distance per left record); do not
  rely on a specific global ordering.
* Pruning uses a separate upper bound per left record; there is no single
  global distance cutoff, so a record whose nearest neighbors are far away
  is never starved by a record whose neighbors are close by.
* If ``visitPair`` throws, the exception propagates out of
  ``nearestNeighborJoin`` after every temporary object created by the join
  has been released. Both trees remain valid and queryable afterwards.
* The join only reads the two trees; it never inserts or deletes. As with
  the other query entry points, a tree must not be modified concurrently
  from another thread while a join is running. Two joins over disjoint sets
  of trees may run concurrently.
* Memory and disk storage managers are both supported. Each tree owns its
  storage manager; page numbers are only meaningful inside one storage
  instance, so never open two trees against each other's storage or share a
  single page id across storage managers.
