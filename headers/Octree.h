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

    class Octree
    {
        public:
        /*
        * A hit produced by the read-only ray query. The color is a copy of
        * the raw voxel color, in the same channel layout as Voxel::color.
        */
        struct RayHit
        {
            float depth;
            Color color;
        };

        /*
        * Input of the read-only ray query. All values are copied before the
        * query runs, so the caller's storage is never retained or modified.
        */
        struct RayQuery
        {
            Vec3<float> origin;
            Vec3<float> direction;
            float maxDistance;
            bool transparency;
            float minVoxelsProximity;
            bool avoidScanGlitches;
        };

        private:
        struct RayState
        {
            Vec3<float> position;
            Vec3<float> direction;
            Vec3<float> normalizer;
            float step;
        };

        struct TraceHit
        {
            float depth;
            Voxel* voxel;
        };

        private:
        Octant* root;
        Mat3f* basis;

        Vec3<float>* ray;
        Vec3<float>* kbase;
        Vec3<float> _kbase;
        Vec3<float> kNormalizer;

        SI raylength;
        float colordepthstep;

        SUI rootSize;
        float rootHalf;

        bool initialized;
        bool accelerated;
        Octant**** accelerator;

        public:
        /*
        * constructor
        */
        Octree();


        /*
        * destructor
        */
        ~Octree();

        Octree(const Octree&) = delete;
        Octree& operator=(const Octree&) = delete;


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
        * Read-only/concurrent query entry point. Both the accelerator and the
        * plain octree traversal are reached through the same implementation.
        * Returns hits ordered by the ray's stepping order (nearest first).
        */
        vector<RayHit> queryRay(const RayQuery& query) const;
        void queryRay(const RayQuery& query, vector<RayHit>* const hits) const;


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
        * Read-only accessors of the instance's own tree geometry.
        */
        float getRayLength() const;
        SUI getRootSize() const;


        /*
        * initAccelerator
        */
        void initAccelerator();


        private:
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

        template<typename T>
        void initBit(T voxel);

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
        * Stepping rules shared by the legacy and the read-only entry points.
        * They only touch the supplied RayState, never the instance fields.
        */
        Octant* getBitAt(const Vec3<float>& position) const;
        Octant* getNextBitAt(RayState& state, const int axis, const float side) const;
        void avoidScanGlitchesAt(RayState& state, Octant** const curbit,
        const bool enabled) const;
        void rayToBorderAt(const float a, const float b, const float c,
        float* const step) const;
        void getNextEntryDotAt(RayState& state, Octant* const octant) const;
        void traceHits(RayState& state, const float maxDistance,
        const bool transparency, const float minVoxelsProximity,
        const bool avoidGlitches, vector<TraceHit>* const hits) const;
    };

#endif
