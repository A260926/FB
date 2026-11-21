/*
 * Static light tree for position-dependent direct light selection.
 *
 * The snapshot built here is read-only after Build() succeeds and can be
 * shared (via const reference) by any number of renderers.  Light sub-path
 * emission is intentionally NOT driven by this tree: emitters keep their
 * uniform qE(light) = 1/N distribution.  The tree only provides the direct
 * illumination selection probability qD(x, light) used by next event
 * estimation and by the MIS bookkeeping of the path tracer and VertexCM.
 *
 * All weighting math is performed in double precision.
 */

#ifndef __LIGHTTREE_HXX__
#define __LIGHTTREE_HXX__

#include <vector>
#include <cmath>
#include <limits>
#include <algorithm>
#include "math.hxx"
#include "frame.hxx"
#include "utils.hxx"
#include "lights.hxx"

class Scene;

class LightTree
{
public:

    // Fraction of the selection mixture that stays uniform.  The remaining
    // 15/16 follow the tree scores, guaranteeing every light keeps a strictly
    // positive selection probability everywhere.
    static const float kUniformFraction; // 1.f / 16.f

    struct NodeInfo
    {
        Vec3f bboxMin;
        Vec3f bboxMax;
        double finiteWeight;
        double infiniteWeight;
        int    leafCount;
        int    leafStart;    // first index into the internal leaf array
        int    leftChild;    // -1 for leaves
        int    rightChild;   // -1 for leaves
        int    lightID;      // original scene light ID, leaves only
        int    depth;
        bool   hasFinite;    // subtree contains at least one finite light
    };

    LightTree() :
        mRadius(0.0),
        mTotalWeight(0.0),
        mAllBlack(true),
        mSampleQueries(0),
        mPdfQueries(0),
        mLastSampleVisits(0),
        mLastPdfVisits(0),
        mMaxSampleVisits(0),
        mMaxPdfVisits(0)
    {}

    //////////////////////////////////////////////////////////////////////////
    // Build
    //////////////////////////////////////////////////////////////////////////

    // Builds the snapshot from the scene's light table.  The scene bounding
    // sphere must already be known.  On any invalid input (non-finite/negative
    // light intensity, or invalid scene radius) the call fails and the
    // previously held snapshot is retained.  Defined in scene.hxx, once the
    // Scene class is complete.
    bool Build(const Scene &aScene);

    // Same as above, but lets tests assemble arbitrary light tables.
    bool Build(
        const std::vector<AbstractLight*> &aLights,
        const SceneSphere                 &aSceneSphere)
    {
        const double radius = double(aSceneSphere.mSceneRadius);
        if(!std::isfinite(radius) || radius <= 0.0)
            return false;

        const int n = int(aLights.size());
        if(n == 0)
        {
            // Empty table: replace whatever was held with an empty snapshot.
            mNodes.clear();
            mLeaves.clear();
            mLightToLeaf.clear();
            mRadius = radius;
            mTotalWeight = 0.0;
            mAllBlack = true;
            ResetCounters();
            return true;
        }

        std::vector<Leaf> leaves;
        leaves.reserve(n);
        const double r2 = radius * radius;

        for(int i = 0; i < n; ++i)
        {
            const AbstractLight *light = aLights[i];
            if(light == NULL)
                return false;

            Leaf leaf;
            leaf.lightID = i;
            leaf.finite  = light->IsFinite();
            leaf.weight  = 0.0;
            for(int a = 0; a < 3; ++a)
            {
                leaf.center[a] = 0.0;
                leaf.bboxMin[a] = 0.0;
                leaf.bboxMax[a] = 0.0;
            }

            if(const AreaLight *area = dynamic_cast<const AreaLight*>(light))
            {
                if(!ValidRadiance(area->mIntensity))
                    return false;
                const double areaVal = 1.0 / double(area->mInvArea);
                leaf.weight = double(Luminance(area->mIntensity)) *
                    Pi() * areaVal;
                SetTriangleBBox(leaf, area->p0,
                    area->p0 + area->e1, area->p0 + area->e2);
            }
            else if(const PointLight *point =
                dynamic_cast<const PointLight*>(light))
            {
                if(!ValidRadiance(point->mIntensity))
                    return false;
                leaf.weight = double(Luminance(point->mIntensity)) *
                    4.0 * Pi();
                SetPointBBox(leaf, point->mPosition);
            }
            else if(const DirectionalLight *dir =
                dynamic_cast<const DirectionalLight*>(light))
            {
                if(!ValidRadiance(dir->mIntensity))
                    return false;
                leaf.weight = double(Luminance(dir->mIntensity)) *
                    Pi() * r2;
            }
            else if(const BackgroundLight *bg =
                dynamic_cast<const BackgroundLight*>(light))
            {
                const Vec3f radiance = bg->mBackgroundColor * bg->mScale;
                if(!ValidRadiance(radiance))
                    return false;
                leaf.weight = double(Luminance(radiance)) *
                    4.0 * Pi() * Pi() * r2;
            }
            else
            {
                // Unknown light type: refuse rather than silently dropping it.
                return false;
            }

            if(!std::isfinite(leaf.weight) || leaf.weight < 0.0)
                return false;

            leaves.push_back(leaf);
        }

        // Build the hierarchy into temporaries first so the old snapshot stays
        // intact until everything has succeeded.
        std::vector<Node> nodes;
        nodes.reserve(size_t(2 * n - 1));
        BuildRecursive(leaves, 0, n, 0, nodes);

        // leaves is now permuted into subtree-contiguous order; remember where
        // every original light ID ended up.
        std::vector<int> lightToLeaf(n);
        for(int i = 0; i < n; ++i)
            lightToLeaf[leaves[i].lightID] = i;

        double totalWeight = 0.0;
        for(int i = 0; i < n; ++i)
            totalWeight += leaves[i].weight;

        // Commit.
        mNodes.swap(nodes);
        mLeaves.swap(leaves);
        mLightToLeaf.swap(lightToLeaf);
        mRadius = radius;
        mTotalWeight = totalWeight;
        mAllBlack = (totalWeight == 0.0);
        ResetCounters();
        return true;
    }

    //////////////////////////////////////////////////////////////////////////
    // Sampling
    //////////////////////////////////////////////////////////////////////////

    // Returns the original light ID, or -1 for an empty snapshot.
    // The single [0,1) input drives both the uniform/tree mixture choice and
    // all hierarchical branch choices (with remapping at every level).
    int Sample(
        const Vec3f &aReceivingPosition,
        float        aRnd) const
    {
        if(IsEmpty())
            return -1;

        const double u = ClampUnit(double(aRnd));
        ++mSampleQueries;
        mLastSampleVisits = 0;

        const int n = int(mLeaves.size());

        // All-black tree: the score distribution is undefined, fall back to
        // uniform selection.
        if(mAllBlack)
        {
            int idx = int(u * n);
            if(idx >= n) idx = n - 1;
            return mLeaves[idx].lightID;
        }

        const double c = double(kUniformFraction);

        // Uniform branch of the mixture.
        if(u < c)
        {
            const double uu = u / c;
            int idx = int(uu * n);
            if(idx >= n) idx = n - 1;
            return mLeaves[idx].lightID;
        }

        // Tree branch, remap the number into [0,1).
        double uu = (u - c) / (1.0 - c);
        int nodeIdx = 0;
        CountSampleVisit();

        while(!IsLeaf(mNodes[nodeIdx]))
        {
            const Node &node = mNodes[nodeIdx];
            const double pLeft = LeftProbability(node, aReceivingPosition);

            if(uu < pLeft)
            {
                nodeIdx = node.left;
                if(pLeft > 0.0)
                    uu /= pLeft;
            }
            else
            {
                nodeIdx = node.right;
                if(pLeft < 1.0)
                    uu = (uu - pLeft) / (1.0 - pLeft);
            }
            CountSampleVisit();
        }

        return mNodes[nodeIdx].lightID;
    }

    // Discrete probability (including the uniform mixture) of selecting the
    // given original light ID from the given receiving position.
    double Pdf(
        const Vec3f &aReceivingPosition,
        int          aLightID) const
    {
        if(IsEmpty() || aLightID < 0 ||
            aLightID >= int(mLightToLeaf.size()))
        {
            return 0.0;
        }

        ++mPdfQueries;
        mLastPdfVisits = 0;

        const int n = int(mLeaves.size());
        if(mAllBlack)
            return 1.0 / n;

        const double treePdf = TreePdfInternal(
            aReceivingPosition, aLightID, true);
        const double c = double(kUniformFraction);
        return c * (1.0 / n) + (1.0 - c) * treePdf;
    }

    // Probability of the tree component alone (before mixing with the uniform
    // component).  Exposed for testing.
    double TreePdf(
        const Vec3f &aReceivingPosition,
        int          aLightID) const
    {
        if(IsEmpty() || aLightID < 0 ||
            aLightID >= int(mLightToLeaf.size()))
        {
            return 0.0;
        }
        if(mAllBlack)
            return 1.0 / int(mLeaves.size());
        return TreePdfInternal(aReceivingPosition, aLightID, false);
    }

    //////////////////////////////////////////////////////////////////////////
    // Inspection
    //////////////////////////////////////////////////////////////////////////

    bool IsEmpty() const         { return mLeaves.empty(); }
    int  LightCount() const      { return int(mLeaves.size()); }
    int  NodeCount() const       { return int(mNodes.size()); }
    double Radius() const        { return mRadius; }
    double TotalWeight() const   { return mTotalWeight; }
    bool   AllBlack() const      { return mAllBlack; }

    // Maximum root-to-leaf depth, 0 for a single-leaf tree.
    int Depth() const
    {
        int depth = 0;
        for(size_t i = 0; i < mNodes.size(); ++i)
            depth = std::max(depth, mNodes[i].depth);
        return depth;
    }

    bool GetNodeInfo(int aNodeIndex, NodeInfo &oInfo) const
    {
        if(aNodeIndex < 0 || aNodeIndex >= int(mNodes.size()))
            return false;
        const Node &node = mNodes[aNodeIndex];
        oInfo.bboxMin = Vec3f(float(node.bboxMin[0]),
            float(node.bboxMin[1]), float(node.bboxMin[2]));
        oInfo.bboxMax = Vec3f(float(node.bboxMax[0]),
            float(node.bboxMax[1]), float(node.bboxMax[2]));
        oInfo.finiteWeight   = node.finiteWeight;
        oInfo.infiniteWeight = node.infiniteWeight;
        oInfo.leafCount      = node.leafCount;
        oInfo.leafStart      = node.leafStart;
        oInfo.leftChild      = node.left;
        oInfo.rightChild     = node.right;
        oInfo.lightID        = node.lightID;
        oInfo.depth          = node.depth;
        oInfo.hasFinite      = node.hasFinite;
        return true;
    }

    int RootNodeIndex() const { return IsEmpty() ? -1 : 0; }

    // Node records touched by the most recent query.
    int LastSampleNodeAccesses() const { return mLastSampleVisits; }
    int LastPdfNodeAccesses() const    { return mLastPdfVisits; }
    int MaxSampleNodeAccesses() const  { return mMaxSampleVisits; }
    int MaxPdfNodeAccesses() const     { return mMaxPdfVisits; }
    long long SampleQueryCount() const { return mSampleQueries; }
    long long PdfQueryCount() const    { return mPdfQueries; }

    // Score of a node at x, exposed for independent test enumeration.
    double NodeScore(int aNodeIndex, const Vec3f &aReceivingPosition) const
    {
        if(aNodeIndex < 0 || aNodeIndex >= int(mNodes.size()))
            return 0.0;
        return Score(mNodes[aNodeIndex], aReceivingPosition);
    }

    // Upper bound on node visits required by the task specification.
    static int VisitBound(int aLightCount)
    {
        if(aLightCount <= 1)
            return 16;
        int lg = 0;
        int v = 1;
        while(v < aLightCount)
        {
            v <<= 1;
            ++lg;
        }
        return 8 * lg + 16;
    }

private:

    static double Pi() { return 3.14159265358979323846; }
    static double EpsRadiusFactor() { return 1e-6; }

    struct Node
    {
        double bboxMin[3];
        double bboxMax[3];
        double finiteWeight;
        double infiniteWeight;
        int    leafStart;
        int    leafCount;
        int    left;
        int    right;
        int    lightID;
        int    depth;
        bool   hasFinite;

        Node() :
            finiteWeight(0.0),
            infiniteWeight(0.0),
            leafStart(0),
            leafCount(0),
            left(-1),
            right(-1),
            lightID(-1),
            depth(0),
            hasFinite(false)
        {
            for(int i = 0; i < 3; ++i)
            {
                bboxMin[i] = 0.0;
                bboxMax[i] = 0.0;
            }
        }
    };

    struct Leaf
    {
        double bboxMin[3];
        double bboxMax[3];
        double center[3];
        double weight;
        int    lightID;
        bool   finite;

        Leaf() : weight(0.0), lightID(-1), finite(false)
        {
            for(int i = 0; i < 3; ++i)
            {
                bboxMin[i] = 0.0;
                bboxMax[i] = 0.0;
                center[i] = 0.0;
            }
        }
    };

    static bool ValidRadiance(const Vec3f &aRadiance)
    {
        for(int i = 0; i < 3; ++i)
        {
            const float v = aRadiance.Get(i);
            if(!std::isfinite(v) || v < 0.f)
                return false;
        }
        return true;
    }

    static double ClampUnit(double aValue)
    {
        if(aValue <= 0.0)
            return 0.0;
        if(aValue >= 1.0)
            return 1.0 - std::numeric_limits<double>::epsilon();
        return aValue;
    }

    static void SetPointBBox(Leaf &aLeaf, const Vec3f &aPoint)
    {
        for(int i = 0; i < 3; ++i)
        {
            const double v = double(aPoint.Get(i));
            aLeaf.bboxMin[i] = v;
            aLeaf.bboxMax[i] = v;
            aLeaf.center[i] = v;
        }
    }

    static void SetTriangleBBox(
        Leaf        &aLeaf,
        const Vec3f &aP0,
        const Vec3f &aP1,
        const Vec3f &aP2)
    {
        const Vec3f pts[3] = { aP0, aP1, aP2 };
        for(int axis = 0; axis < 3; ++axis)
        {
            double lo = double(pts[0].Get(axis));
            double hi = lo;
            double sum = lo;
            for(int p = 1; p < 3; ++p)
            {
                const double v = double(pts[p].Get(axis));
                lo = std::min(lo, v);
                hi = std::max(hi, v);
                sum += v;
            }
            aLeaf.bboxMin[axis] = lo;
            aLeaf.bboxMax[axis] = hi;
            aLeaf.center[axis] = sum / 3.0;
        }
    }

    static bool IsLeaf(const Node &aNode)
    {
        return aNode.left < 0 && aNode.right < 0;
    }

    // Splits [aBegin, aEnd) into two as-equal-as-possible sets, recursively
    // building an almost complete binary hierarchy of depth O(log N).
    void BuildRecursive(
        std::vector<Leaf> &aLeaves,
        int                aBegin,
        int                aEnd,
        int                aDepth,
        std::vector<Node> &oNodes)
    {
        Node node;
        node.depth = aDepth;
        node.leafCount = aEnd - aBegin;
        node.leafStart = aBegin;

        bool hasFinite = false;
        double bboxMin[3] = { 0.0, 0.0, 0.0 };
        double bboxMax[3] = { 0.0, 0.0, 0.0 };
        bool bboxInit = false;

        for(int i = aBegin; i < aEnd; ++i)
        {
            const Leaf &leaf = aLeaves[i];
            node.finiteWeight   += leaf.finite ? leaf.weight : 0.0;
            node.infiniteWeight += leaf.finite ? 0.0 : leaf.weight;

            if(leaf.finite)
            {
                hasFinite = true;
                if(!bboxInit)
                {
                    for(int a = 0; a < 3; ++a)
                    {
                        bboxMin[a] = leaf.bboxMin[a];
                        bboxMax[a] = leaf.bboxMax[a];
                    }
                    bboxInit = true;
                }
                else
                {
                    for(int a = 0; a < 3; ++a)
                    {
                        bboxMin[a] = std::min(bboxMin[a], leaf.bboxMin[a]);
                        bboxMax[a] = std::max(bboxMax[a], leaf.bboxMax[a]);
                    }
                }
            }
        }

        node.hasFinite = hasFinite;
        if(hasFinite)
        {
            for(int a = 0; a < 3; ++a)
            {
                node.bboxMin[a] = bboxMin[a];
                node.bboxMax[a] = bboxMax[a];
            }
        }

        const int nodeIdx = int(oNodes.size());
        oNodes.push_back(node);

        if(aEnd - aBegin == 1)
        {
            oNodes[nodeIdx].lightID = aLeaves[aBegin].lightID;
            return;
        }

        // Split along the axis of the largest centroid extent.
        double centroidMin[3], centroidMax[3];
        for(int a = 0; a < 3; ++a)
        {
            centroidMin[a] = aLeaves[aBegin].center[a];
            centroidMax[a] = centroidMin[a];
        }
        for(int i = aBegin + 1; i < aEnd; ++i)
        {
            for(int a = 0; a < 3; ++a)
            {
                centroidMin[a] = std::min(centroidMin[a],
                    aLeaves[i].center[a]);
                centroidMax[a] = std::max(centroidMax[a],
                    aLeaves[i].center[a]);
            }
        }
        int axis = 0;
        if(centroidMax[1] - centroidMin[1] > centroidMax[0] - centroidMin[0])
            axis = 1;
        if(centroidMax[2] - centroidMin[2] >
            centroidMax[axis] - centroidMin[axis])
            axis = 2;

        const int mid = aBegin + (aEnd - aBegin) / 2;
        std::nth_element(aLeaves.begin() + aBegin,
            aLeaves.begin() + mid, aLeaves.begin() + aEnd,
            CmpCentroid(axis));

        BuildRecursive(aLeaves, aBegin, mid, aDepth + 1, oNodes);
        // The right child starts at the first node the second recursion adds.
        const int rightChild = int(oNodes.size());
        BuildRecursive(aLeaves, mid, aEnd, aDepth + 1, oNodes);
        oNodes[nodeIdx].left = nodeIdx + 1;
        oNodes[nodeIdx].right = rightChild;
    }

    struct CmpCentroid
    {
        explicit CmpCentroid(int aAxis) : mAxis(aAxis) {}
        bool operator()(const Leaf &aA, const Leaf &aB) const
        {
            return aA.center[mAxis] < aB.center[mAxis];
        }
        int mAxis;
    };

    // Squared distance from x to the node's finite-light bounding box.
    double DistanceSqrToBox(
        const Node  &aNode,
        const Vec3f &aX) const
    {
        double dist2 = 0.0;
        for(int axis = 0; axis < 3; ++axis)
        {
            const double x = double(aX.Get(axis));
            if(x < aNode.bboxMin[axis])
            {
                const double d = aNode.bboxMin[axis] - x;
                dist2 += d * d;
            }
            else if(x > aNode.bboxMax[axis])
            {
                const double d = x - aNode.bboxMax[axis];
                dist2 += d * d;
            }
        }
        return dist2;
    }

    // Node score at x.  The finite term vanishes for subtrees without
    // finite lights.
    double Score(
        const Node  &aNode,
        const Vec3f &aX) const
    {
        const double r2 = mRadius * mRadius;
        double score = aNode.infiniteWeight / r2;
        if(aNode.hasFinite)
        {
            const double dist2 = DistanceSqrToBox(aNode, aX);
            score += aNode.finiteWeight /
                (dist2 + EpsRadiusFactor() * r2);
        }
        return score;
    }

    // Probability of descending into the left child.  On a double-zero score
    // the branch follows the subtree leaf counts.
    double LeftProbability(
        const Node  &aNode,
        const Vec3f &aX) const
    {
        const Node &left  = mNodes[aNode.left];
        const Node &right = mNodes[aNode.right];

        const double scoreLeft  = Score(left, aX);
        const double scoreRight = Score(right, aX);
        const double sum = scoreLeft + scoreRight;

        if(!(sum > 0.0))
        {
            return double(left.leafCount) /
                double(left.leafCount + right.leafCount);
        }
        return scoreLeft / sum;
    }

    double TreePdfInternal(
        const Vec3f &aReceivingPosition,
        int          aLightID,
        bool         aCountVisits) const
    {
        const int targetLeaf = mLightToLeaf[aLightID];
        int nodeIdx = 0;
        if(aCountVisits) CountPdfVisit();
        double probability = 1.0;

        while(!IsLeaf(mNodes[nodeIdx]))
        {
            const Node &node = mNodes[nodeIdx];
            const Node &left = mNodes[node.left];
            const Node &right = mNodes[node.right];

            double pLeft;
            const double scoreLeft  = Score(left, aReceivingPosition);
            const double scoreRight = Score(right, aReceivingPosition);
            const double sum = scoreLeft + scoreRight;
            if(!(sum > 0.0))
            {
                pLeft = double(left.leafCount) /
                    double(left.leafCount + right.leafCount);
            }
            else
            {
                pLeft = scoreLeft / sum;
            }

            const bool goLeft =
                targetLeaf >= left.leafStart &&
                targetLeaf < left.leafStart + left.leafCount;
            if(goLeft)
            {
                probability *= pLeft;
                nodeIdx = node.left;
            }
            else
            {
                probability *= (1.0 - pLeft);
                nodeIdx = node.right;
            }
            if(aCountVisits) CountPdfVisit();
        }
        return probability;
    }

    void CountSampleVisit() const
    {
        ++mLastSampleVisits;
        mMaxSampleVisits = std::max(mMaxSampleVisits, mLastSampleVisits);
    }

    void CountPdfVisit() const
    {
        ++mLastPdfVisits;
        mMaxPdfVisits = std::max(mMaxPdfVisits, mLastPdfVisits);
    }

    void ResetCounters()
    {
        mSampleQueries = 0;
        mPdfQueries = 0;
        mLastSampleVisits = 0;
        mLastPdfVisits = 0;
        mMaxSampleVisits = 0;
        mMaxPdfVisits = 0;
    }

private:

    std::vector<Node> mNodes;
    // Permuted so that every subtree occupies a contiguous index range;
    // node.leafStart/node.leafCount describe those ranges.
    std::vector<Leaf> mLeaves;
    std::vector<int>  mLightToLeaf; // original light ID -> index in mLeaves
    double            mRadius;
    double            mTotalWeight;
    bool              mAllBlack;

    // Query statistics.  Mutable so const/read-only snapshots shared between
    // renderers can still record node access counts.
    mutable long long mSampleQueries;
    mutable long long mPdfQueries;
    mutable int       mLastSampleVisits;
    mutable int       mLastPdfVisits;
    mutable int       mMaxSampleVisits;
    mutable int       mMaxPdfVisits;
};

const float LightTree::kUniformFraction = 1.f / 16.f;

#endif //__LIGHTTREE_HXX__
