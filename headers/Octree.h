/*
 * Cloud of Voxels (CoV) project
 * Author: m-c/d, mcidclan
 * Creation Date: 2011
 * Modification Date: 2025
 */

#ifndef OCTREE_H
#define OCTREE_H

    #include "Octant.h"
    #define RAYSTEP_MIN_VALUE 1.0f

    /*
    * Immutable result of a single ray query, returned by the read-only
    * traceRay entry. Depth and color are copies ordered by the original
    * stepping sequence; nothing here references the tree's internals.
    */
    struct RayHit
    {
        float depth;
        unsigned int color;
    };

    /*
    * Parameters of a read-only ray query. They are copied by value so the
    * caller's storage is never touched, and no query scratch is left behind.
    */
    struct RayQuery
    {
        Vec3<float> origin;
        Vec3<float> direction;
        float maxDistance;
        bool transparency;
        SUI minVoxelProximity;
        bool avoidGlitches;
        /*
        * When useNormalizer is false the border-step normalizer is derived
        * from direction, which is the documented behavior. When true the
        * supplied normalizer is used instead; this mirrors the legacy
        * perspective path, where setRay derives the normalizer from the
        * unperturbed camera k axis before setPerspectiveRay bends it.
        */
        bool useNormalizer = false;
        Vec3<float> normalizer = {0.0f, 0.0f, 0.0f};
    };

    class Octree
    {
        public:
        /*
        * Legacy shared bounds, kept for the old window entry points.
        * New code must rely on the per-instance bounds below.
        */
        static float size;
        static float half;

        private:
        Octant* root;

        /*
        * Per-instance ownership: root bounds, ray length, color scaling and
        * the accelerator all belong to the tree that built them, so trees of
        * different sizes can coexist and be queried at the same time.
        */
        float treeSize;
        float treeHalf;
        SI raylength;
        float colordepthstep;
        SUI acceleratorSize;
        bool accelerated;
        Octant**** accelerator;

        /*
        * Legacy mutable query scratch, only used by the old setRay/rayTrace
        * entry points. The read-only traceRay path keeps no shared state.
        */
        Vec3<float> kNormalizer;
        Vec3<float>* ray;
        Mat3f* basis;
        Vec3<float>* kbase;
        Vec3<float> _kbase;
        float raystep;

        public:
        /*
        * constructor
        */
        Octree();


        /*
        * destructor
        */
        ~Octree();


        /*
        * setRay
        */
        void setRay(Vec3<float>* const ray);


        /*
        * initRoot
        */
        void initRoot(SUI rootsize, SUI maxdepth, const SI raylength);


        /*
        * initBasis
        */
        void initBasis(Mat3f* const basis);

        void setPerspectiveRay(const float screenX, const float screenY, const float invDist);

        /*
        * rayTrace
        */
        bool rayTrace(vector<DynamicVoxel>* const voxels);


        /*
        * addVoxels
        */
        void addVoxels(Voxel* voxels, const UI nvoxel);


        /*
        * getColorDepth
        */
        Color getColorDepth(const DynamicVoxel* const dynamic);

        Color getColorDepth(const UC r, const UC g, const UC b, const float depth);

        float getDarkness(const float depth) const;


        /*
        * initAccelerator
        */
        void initAccelerator();


        /*
        * Read-only, thread-safe ray query. The tree is not modified and no
        * per-instance scratch is used, so several threads may trace rays
        * through the same const Octree concurrently. The caller's inputs are
        * copied and never altered. Hits are returned in stepping order.
        */
        vector<RayHit> traceRay(const RayQuery& query) const;


        /*
        * Read-only bounds and configuration owned by this tree instance.
        */
        float getSize() const;
        float getHalf() const;
        SI getRayLength() const;
        bool isAccelerated() const;


        private:
        /*
        * getNextEntryDot
        */
        void getNextEntryDot(Octant* octant);


        template<typename T>
        void initBit(T voxel);


        /*
        * add a single voxel
        */
        void addSingleVoxel(Voxel* const voxel);


        /*
        * addSiblings
        */
        void addSmooths(Voxel* const voxel);


        /*
        * addShellXL
        */
        void addShellXL(const Voxel voxel);


        /*
        * addShellLite
        */
        void addShellLite(const Voxel voxel);


        /*
        * addShell
        */
        void addShell(const Voxel voxel);


        /*
        * rayToBorder
        */
        void rayToBorder(const float a, const float b, const float c);


        /*
        * fillAccelerator
        */
        void fillAccelerator();


        /*
        * destroyAccelerator
        */
        void destroyAccelerator();


        /*
        * getAccelerator
        */
        void createAccelerator();


        /*
        * getBit
        */
        Octant* getBit();


        /*
        * getNextBit
        */
        Octant* getNextBit(float* axis, const float side);


        /*
        * avoidScanGlitches
        */
        void avoidScanGlitches(Octant** const curbit);


        /*
        * Stateless equivalents of the legacy stepping helpers, shared by the
        * old rayTrace path and the read-only traceRay path. They implement
        * the same getBit / avoidScanGlitches / getNextEntryDot rules.
        */
        Octant* lookupBit(const Vec3<float>& position) const;
        void avoidScanGlitchesState(Octant** const curbit,
        Vec3<float>* const position, const bool avoid) const;
        float nextEntryDot(Octant* const octant, const Vec3<float>& direction,
        const Vec3<float>& normalizer, Vec3<float>* const position) const;
        Vec3<float> directionNormalizer(const Vec3<float>& direction) const;
    };

#endif
