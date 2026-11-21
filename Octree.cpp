/*
 * Cloud of Voxels (CoV) project
 * Author: mcidclan, m [.D'O'T.] cid [.D'O'T.] clan [.A'T.] gmail [.D'O'T.] com
 * Creation Date: 2011
 * Modification Date: 2020
 */

#include <cmath>

#include "./headers/Octree.h"

extern Vec3<SI> atom[18];
extern Vec3<SI> shell[54];
extern Vec3<SI> shellxl[150];


Octree::Octree()
{
    this->root = NULL;
    this->basis = NULL;
    this->ray = NULL;
    this->kbase = NULL;
    this->accelerated = false;
    this->accelerator = NULL;
    this->initialized = false;
    this->raylength = 0;
    this->colordepthstep = 0.0f;
    this->rootSize = 0;
    this->rootHalf = 0.0f;
    this->_kbase.x = this->_kbase.y = this->_kbase.z = 0.0f;
    this->kNormalizer.x = this->kNormalizer.y = this->kNormalizer.z = 0.0f;
}


Octree::~Octree()
{
    this->destroyAccelerator();
    if(this->root != NULL)
    {
        OctantManager::destroy(this->root);
        this->root = NULL;
    }
}


/*
 *
 */
void Octree::destroyAccelerator()
{
    if(this->accelerator == NULL)
    {
        return;
    }
    UI i = 0, j = 0;
    while(i < this->rootSize)
    {
        j = 0;
        while(j < this->rootSize)
        {
            delete [] this->accelerator[i][j];
            j++;
        }
        delete [] this->accelerator[i];
        i++;
    }
    delete [] this->accelerator;
    this->accelerator = NULL;
    this->accelerated = false;
}


/*
 *
 */
void Octree::createAccelerator()
{
    double msize = (pow((double)this->rootSize, 3.0)*4.0) / pow(1000.0, 3);
    if(!Options::nologs)
    {
        printf("Trying to allocate %f GB for accelerator\n", msize);
    }
    this->accelerator = new Octant***[this->rootSize];
    UI i = 0, j = 0;
    while(i < this->rootSize)
    {
        this->accelerator[i] = new Octant**[this->rootSize];
        j = 0;
        while(j < this->rootSize)
        {
            this->accelerator[i][j] = new Octant*[this->rootSize];
            j++;
        }
        i++;
    }
    if(!Options::nologs)
    {
        printf("Accelerator created\n");
    }
}


/*
 * fillAccelerator
 */
void Octree::fillAccelerator()
{
    if(!Options::nologs)
    {
        printf("Filling the accelerator, please wait...\n");
    }
    UI i = 0, j = 0, k = 0;
    const SI OCTREE_HALF_SIZE = this->rootSize / 2;
    while(i < this->rootSize)
    {
        j = 0;
        const SI x = i - OCTREE_HALF_SIZE;
        while(j < this->rootSize)
        {
            k = 0;
            const SI y = j - OCTREE_HALF_SIZE;
            while(k < this->rootSize)
            {
                const SI z = k - OCTREE_HALF_SIZE;
                this->accelerator[i][j][k] =
                OctantManager::getBit(this->root, {x, y, z});
                k++;
            }
            j++;
        }
        i++;
    }
    if(!Options::nologs)
    {
        printf("Accelerator filled\n");
    }
}

void Octree::initAccelerator()
{
    if(!this->initialized || this->rootSize == 0 || this->accelerated)
    {
        return;
    }
    this->createAccelerator();
    this->fillAccelerator();
    this->accelerated = true;
}


/*
 * initRoot
 */
void Octree::initRoot(SUI size, SUI maxdepth, const SI raylength)
{
    if(this->initialized)
    {
        return;
    }
    this->root = new Octant();
    OctantManager::init(this->root);
    this->root->depth = maxdepth;
    this->root->half = size/2;
    this->root->center.x =
    this->root->center.y =
    this->root->center.z = 0.0f;
    OctantManager::setFacesCenter(this->root);
    this->ray = NULL;
    this->raylength = raylength > 0 ? raylength : 0;
    this->colordepthstep = this->raylength > 0
    ? (1.0f/(float)this->raylength) : 0.0f;
    this->rootSize = size;
    this->rootHalf = this->root->half;
    this->initialized = true;
}


/*
 * initBasis
 */
void Octree::initBasis(Mat3f* const basis)
{
    this->basis = basis;
    this->kbase = &(basis->k);
    this->_kbase = *this->kbase;
}


/*
 * setRay
 */
void Octree::setRay(Vec3<float>* const ray) {
    this->ray = ray;
    kNormalizer.x = math::absf(kbase->x) > 0.001f ? 1.0f / kbase->x : 1000.0f;
    kNormalizer.y = math::absf(kbase->y) > 0.001f ? 1.0f / kbase->y : 1000.0f;
    kNormalizer.z = math::absf(kbase->z) > 0.001f ? 1.0f / kbase->z : 1000.0f;
}


void Octree::setPerspectiveRay(const float x, const float y, const float invDist)
{
    *this->kbase = this->_kbase;
    this->kbase->x += (this->basis->i.x * x + this->basis->j.x * y) * invDist;
    this->kbase->y += (this->basis->i.y * x + this->basis->j.y * y) * invDist;
    this->kbase->z += (this->basis->i.z * x + this->basis->j.z * y) * invDist;
    math::normalize(this->kbase);
}


/*
 * rayTrace
 *
 * The legacy entry point. It still moves the caller's ray, but the stepping
 * rules themselves come from the same implementation as the read-only query.
 */
bool Octree::rayTrace(vector<DynamicVoxel>* const voxels)
{
    if(this->ray == NULL)
    {
        return voxels->size() > 0;
    }

    RayState state;
    state.position = *this->ray;
    state.direction = *this->kbase;
    state.normalizer = this->kNormalizer;
    state.step = RAYSTEP_MIN_VALUE;

    vector<TraceHit> hits;
    this->traceHits(state, (float)this->raylength, Options::TRANSPARENCY,
    (float)Options::MIN_VOXELS_PROXIMITY, Options::AVOID_SCAN_GLITCHES, &hits);

    *this->ray = state.position;

    for(size_t i = 0; i < hits.size(); i++)
    {
        voxels->push_back({hits[i].depth, hits[i].voxel});
    }
    return voxels->size() > 0;
}


/*
 * getBitAt
 */
Octant* Octree::getBitAt(const Vec3<float>& position) const
{
    if(this->root == NULL)
    {
        return NULL;
    }
    if(this->accelerated && this->accelerator != NULL)
    {
        const SI x = (SI)(position.x + this->rootHalf);
        const SI y = (SI)(position.y + this->rootHalf);
        const SI z = (SI)(position.z + this->rootHalf);

        if(x >= 0 && x < (SI)this->rootSize &&
            y >= 0 && y < (SI)this->rootSize &&
            z >= 0 && z < (SI)this->rootSize)
        {
            return this->accelerator[x][y][z];
        }
    }
    return OctantManager::getBit(this->root, {
        (SI)position.x,
        (SI)position.y,
        (SI)position.z
    });
}


/*
 * getNextBitAt
 */
Octant* Octree::getNextBitAt(RayState& state, const int axis,
const float side) const
{
    float* const a = (axis == 0) ? &state.position.x
    : (axis == 1) ? &state.position.y : &state.position.z;
    const float d = *a;
    *a = ((SI)d) + side;
    Octant* const curbit = this->getBitAt(state.position);
    *a = d;
    return curbit;
}


/*
 * avoidScanGlitchesAt
 */
void Octree::avoidScanGlitchesAt(RayState& state, Octant** const curbit,
const bool enabled) const
{
    if(!enabled || *curbit == NULL)
    {
        return;
    }
    if((*curbit)->voxel == NULL) {
        *curbit = getNextBitAt(state, 0, 1.0f);
        if(*curbit == NULL) return;
    }
    if((*curbit)->voxel == NULL) {
        *curbit = getNextBitAt(state, 1, 1.0f);
        if(*curbit == NULL) return;
    }
    if((*curbit)->voxel == NULL) {
        *curbit = getNextBitAt(state, 2, 1.0f);
        if(*curbit == NULL) return;
    }
    if((*curbit)->voxel == NULL) {
        *curbit = getNextBitAt(state, 0, -1.0f);
        if(*curbit == NULL) return;
    }
    if((*curbit)->voxel == NULL) {
        *curbit = getNextBitAt(state, 1, -1.0f);
        if(*curbit == NULL) return;
    }
    if((*curbit)->voxel == NULL) {
        *curbit = getNextBitAt(state, 2, -1.0f);
    }
}


/*
 * rayToBorderAt
 */
void Octree::rayToBorderAt(const float a, const float b, const float c,
float* const step) const
{
    // Distance between the current ray position and
    // the next potential octant limit position
    const float d = (b - a) * c;
    if(d < *step) {
        *step = d;
    }
}


/*
 * getNextEntryDotAt
 */
void Octree::getNextEntryDotAt(RayState& state, Octant* const octant) const
{
    if(octant == NULL)
    {
        state.step = RAYSTEP_MIN_VALUE;
        return;
    }

    if(octant->half <= 0.5f)
    {
        state.step = 1.0f;
        state.position.x += state.direction.x;
        state.position.y += state.direction.y;
        state.position.z += state.direction.z;
        return;
    }

    state.step = this->rootSize;
    rayToBorderAt(state.position.x, octant->facescenter[
    (state.direction.x > 0.0f) ? 1 : 0], state.normalizer.x, &state.step);
    rayToBorderAt(state.position.y, octant->facescenter[
    (state.direction.y > 0.0f) ? 3 : 2], state.normalizer.y, &state.step);
    rayToBorderAt(state.position.z, octant->facescenter[
    (state.direction.z > 0.0f) ? 5 : 4], state.normalizer.z, &state.step);

    if(state.step < RAYSTEP_MIN_VALUE)
    {
        state.step = RAYSTEP_MIN_VALUE;
    }

    state.position.x += state.direction.x * state.step;
    state.position.y += state.direction.y * state.step;
    state.position.z += state.direction.z * state.step;
}


/*
 * traceHits
 */
void Octree::traceHits(RayState& state, const float maxDistance,
const bool transparency, const float minVoxelsProximity,
const bool avoidGlitches, vector<TraceHit>* const hits) const
{
    if(this->root == NULL)
    {
        return;
    }

    float depthray = 0.0f;
    while(depthray < maxDistance)
    {
        Octant* curbit = this->getBitAt(state.position);
        if(curbit == NULL)
        {
            break;
        }
        this->avoidScanGlitchesAt(state, &curbit, avoidGlitches);
        if(curbit != NULL && curbit->voxel != NULL)
        {
            if(!transparency ||
                (curbit->voxel->color & 0x000000FF) == 0xFF)
            {
                hits->push_back({depthray, curbit->voxel});
                return;
            }
            if(hits->size() == 0 ||
                (depthray - (*hits)[hits->size()-1].depth) >= minVoxelsProximity)
            {
                hits->push_back({depthray, curbit->voxel});
            }
        }
        this->getNextEntryDotAt(state, curbit);
        depthray += state.step;
    }
}


/*
 * queryRay
 */
void Octree::queryRay(const RayQuery& query, vector<RayHit>* const hits) const
{
    if(hits == NULL)
    {
        return;
    }
    hits->clear();

    RayState state;
    state.position.x = query.origin.x;
    state.position.y = query.origin.y;
    state.position.z = query.origin.z;
    state.direction.x = query.direction.x;
    state.direction.y = query.direction.y;
    state.direction.z = query.direction.z;
    state.step = RAYSTEP_MIN_VALUE;

    state.normalizer.x = math::absf(state.direction.x) > 0.001f
    ? 1.0f / state.direction.x : 1000.0f;
    state.normalizer.y = math::absf(state.direction.y) > 0.001f
    ? 1.0f / state.direction.y : 1000.0f;
    state.normalizer.z = math::absf(state.direction.z) > 0.001f
    ? 1.0f / state.direction.z : 1000.0f;

    vector<TraceHit> found;
    this->traceHits(state, query.maxDistance, query.transparency,
    query.minVoxelsProximity, query.avoidScanGlitches, &found);

    hits->reserve(found.size());
    for(size_t i = 0; i < found.size(); i++)
    {
        const Voxel* const voxel = found[i].voxel;
        const Color color = {
            (UC)((voxel->color & 0xFF000000) >> 24),
            (UC)((voxel->color & 0x00FF0000) >> 16),
            (UC)((voxel->color & 0x0000FF00) >> 8),
            (UC)(voxel->color & 0x000000FF)
        };
        hits->push_back({found[i].depth, color});
    }
}


vector<Octree::RayHit> Octree::queryRay(const RayQuery& query) const
{
    vector<RayHit> hits;
    this->queryRay(query, &hits);
    return hits;
}


/*
 * addVoxels
 */
void Octree::addVoxels(Voxel* voxels, const UI nvoxel)
{
	UI i = 0;
    UC percent = 0;
    const UI step = nvoxel / 100;
	while(i < nvoxel)
	{
        if((step > 0) && (i % (step * 10) == 0))
        {
            if(!Options::nologs)
            {
                printf(">%d ", percent);
            }
            percent+=10;
        }

        if(Options::SMOOTH_SIBLINGS)
        {
            this->addSmooths(&voxels[i]);
        } else this->addSingleVoxel(&voxels[i]);

        switch(Options::VOXEL_SHELL_TYPE)
        {
            case 1:
                this->addShell(voxels[i]);
                break;
            case 2:
                this->addShellXL(voxels[i]);
                break;
            case 3:
                this->addShellLite(voxels[i]);
                break;
        }
        i++;
	}
    if(!Options::nologs)
    {
        printf("\n");
    }
}

template<typename T>
void Octree::initBit(T voxel)
{
    OctantManager::initBit(this->root, voxel, this->rootHalf);
}


/*
 * add single voxel
 */
void Octree::addSingleVoxel(Voxel* const voxel)
{
    this->initBit(voxel);
}

/*
 * addSmooths
 */
void Octree::addSmooths(Voxel* const voxel)
{
    UC i = 0;
    while(i < 18)
    {
        Voxel v = *voxel;
        v.coordinates.x += atom[i].x;
        v.coordinates.y += atom[i].y;
        v.coordinates.z += atom[i].z;
        this->initBit(v);
        i++;
    }
    this->initBit(voxel);
}

/*
 * addSmooths
 */
void Octree::addShellLite(const Voxel voxel)
{
    UC i = 0;
    while(i < Options::SHELL_LITE_THICKNESS)
    {
        Voxel v = voxel;
        math::vecxscl(&v.coordinates, 1.05f + i * 0.02f);
        if(Options::SHELL_COLOR > 0)
        {
            v.color = Options::SHELL_COLOR;
        }
        else v.color = (v.color & 0xFFFFFF00) | 0x0F;
        this->initBit(v);
        i++;
    }

}


/*
 * addShellXL
 */
void Octree::addShellXL(const Voxel voxel)
{
    UC i = 0;
    while(i < 150)
    {
        Voxel v = voxel;
        v.coordinates.x += shellxl[i].x;
        v.coordinates.y += shellxl[i].y;
        v.coordinates.z += shellxl[i].z;
        if(Options::SHELL_COLOR > 0)
        {
            v.color = Options::SHELL_COLOR;
        }
        else v.color = (v.color & 0xFFFFFF00) | 0x0F;
        this->initBit(v);
        i++;
    }
}


/*
 * addShell
 */
void Octree::addShell(const Voxel voxel)
{
    UC i = 0;
    while(i < 54)
    {
        Voxel v = voxel;
        v.coordinates.x += shell[i].x;
        v.coordinates.y += shell[i].y;
        v.coordinates.z += shell[i].z;
        if(Options::SHELL_COLOR > 0)
        {
            v.color = Options::SHELL_COLOR;
        }
        else v.color = (v.color & 0xFFFFFF00) | 0x0F;
        this->initBit(v);
        i++;
    }
}


/*
 * setColorDepth
 */
Color Octree::getColorDepth(const DynamicVoxel* const dynamic)
{
    const float darkness = getDarkness(dynamic->depth);
	return {
        (UC)(((dynamic->voxel->color & 0xFF000000) >> 24) * darkness),
        (UC)(((dynamic->voxel->color & 0x00FF0000) >> 16) * darkness),
        (UC)(((dynamic->voxel->color & 0x0000FF00) >> 8) * darkness),
        (UC)(dynamic->voxel->color & 0x000000FF)
    };
}


Color Octree::getColorDepth(const UC r, const UC g, const UC b, const float depth)
{
    const float darkness = getDarkness(depth);
    return {(UC)(r * darkness), (UC)(g * darkness), (UC)(b * darkness), 0};
}

float Octree::getDarkness(const float depth) const
{
   return 1.0f - depth * this->colordepthstep;
}


float Octree::getRayLength() const
{
    return (float)this->raylength;
}


SUI Octree::getRootSize() const
{
    return this->rootSize;
}
