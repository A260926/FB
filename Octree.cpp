/*
 * Cloud of Voxels (CoV) project
 * Author: mcidclan, mcidclan
 * Creation Date: 2011
 * Modification Date: 2020
 */

#include "./headers/Octree.h"
#include <stdexcept>

extern Vec3<SI> atom[18];
extern Vec3<SI> shell[54];
extern Vec3<SI> shellxl[150];

float Octree::size;
float Octree::half;

/*
 * Shared border stepping formula, used by both the legacy rayToBorder path
 * and the stateless read-only path so both step exactly the same way.
 */
static void borderStep(const float a, const float b, const float c,
float* const raystep)
{
    const float d = (b - a) * c;
    if(d < *raystep)
    {
        *raystep = d;
    }
}

Octree::Octree()
{
    this->root = NULL;
    this->ray = NULL;
    this->basis = NULL;
    this->kbase = NULL;
    this->raylength = 0;
    this->raystep = 0.0f;
    this->colordepthstep = 0.0f;
    this->treeSize = 0.0f;
    this->treeHalf = 0.0f;
    this->acceleratorSize = 0;
    this->accelerated = false;
    this->accelerator = NULL;
}


Octree::~Octree()
{
    if(this->root != NULL)
    {
        delete [] this->root->facescenter;
        delete this->root;
    }
    if(this->accelerated)
    {
        this->destroyAccelerator();
    }
}


/*
 *
 */
 void Octree::destroyAccelerator()
{
    UI i = 0, j = 0;
    while(i < this->acceleratorSize)
    {
        j = 0;
        while(j < this->acceleratorSize)
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
    double msize = (pow((double)this->acceleratorSize, 3.0)*4.0) / pow(1000.0, 3);
    printf("Trying to allocate %f GB for accelerator\n", msize);
    this->accelerator = new Octant***[this->acceleratorSize];
    UI i = 0, j = 0;
    while(i < this->acceleratorSize)
    {
        this->accelerator[i] = new Octant**[this->acceleratorSize];
        j = 0;
        while(j < this->acceleratorSize)
        {
            this->accelerator[i][j] = new Octant*[this->acceleratorSize];
            j++;
        }
        i++;
    }
    printf("Accelerator created\n");
}


/*
 * fillAccelerator
 */
void Octree::fillAccelerator()
{
    printf("Filling the accelerator, please wait...\n");
    int i = 0, j = 0, k = 0;
    const SI OCTREE_HALF_SIZE = this->acceleratorSize / 2;
    while(i < this->acceleratorSize)
    {
        j = 0;
        const SI x = i - OCTREE_HALF_SIZE;
        while(j < this->acceleratorSize)
        {
            k = 0;
            const SI y = j - OCTREE_HALF_SIZE;
            while(k < this->acceleratorSize)
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
    printf("Accelerator filled\n");
}

void Octree::initAccelerator()
{
    this->createAccelerator();
    this->fillAccelerator();
    this->accelerated = true;
}


/*
 * initRoot
 */
void Octree::initRoot(SUI size, SUI maxdepth, const SI raylength)
{
    if(this->root != NULL)
    {
        throw std::logic_error("Octree::initRoot called twice on the same tree");
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
    this->raylength = raylength;
    this->colordepthstep = (1.0f/(float)raylength);
    this->treeHalf = this->root->half;
    this->treeSize = this->treeHalf*2.0f;
    this->acceleratorSize = size;
    /*
    * Legacy shared bounds, only maintained for the old window entry points.
    */
    Octree::half = this->treeHalf;
    Octree::size = this->treeSize;
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
    this->kNormalizer = this->directionNormalizer(*this->kbase);
}

/*
 * Stateless octant lookup for a ray position. Uses the accelerator when this
 * tree owns one, otherwise walks the octree, with the same coordinate rules
 * as the legacy getBit.
 */
Octant* Octree::lookupBit(const Vec3<float>& position) const
{
    if(this->accelerated)
    {
        const SI x = (SI)(position.x + this->treeHalf);
        const SI y = (SI)(position.y + this->treeHalf);
        const SI z = (SI)(position.z + this->treeHalf);

        if(x >= 0 && x < (SI)this->acceleratorSize &&
            y >= 0 && y < (SI)this->acceleratorSize &&
            z >= 0 && z < (SI)this->acceleratorSize)
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
 * getBit
 */
Octant* Octree::getBit()
{
    return this->lookupBit(*this->ray);
}


/*
 * getNextBit
 */
Octant* Octree::getNextBit(float* axis, const float side)
{
    const float d = *axis;
    *axis = ((SI)d) + side;
    Octant* const curbit = this->getBit();
    *axis = d;
    return curbit;
}


/*
 * avoidScanGlitches
 */
void Octree::avoidScanGlitches(Octant** const curbit)
{
    if(Options::AVOID_SCAN_GLITCHES)
    {
        if((*curbit)->voxel == NULL) {
            *curbit = getNextBit(&this->ray->x, 1.0f);
        }
        if((*curbit)->voxel == NULL) {
            *curbit = getNextBit(&this->ray->y, 1.0f);
        }
        if((*curbit)->voxel == NULL) {
            *curbit = getNextBit(&this->ray->z, 1.0f);
        }
        if((*curbit)->voxel == NULL) {
            *curbit = getNextBit(&this->ray->x, -1.0f);
        }
        if((*curbit)->voxel == NULL) {
            *curbit = getNextBit(&this->ray->y, -1.0f);
        }
        if((*curbit)->voxel == NULL) {
            *curbit = getNextBit(&this->ray->z, -1.0f);
        }
    }
}


/*
 * Stateless counterpart of avoidScanGlitches, probing the same six neighbors
 * in the same order without touching the caller's ray position.
 */
void Octree::avoidScanGlitchesState(Octant** const curbit,
Vec3<float>* const position, const bool avoid) const
{
    if(!avoid)
    {
        return;
    }
    static const int axes[6] = {0, 1, 2, 0, 1, 2};
    static const float sides[6] = {1.0f, 1.0f, 1.0f, -1.0f, -1.0f, -1.0f};
    int n = 0;
    while(n < 6)
    {
        if((*curbit)->voxel != NULL)
        {
            return;
        }
        Vec3<float> probe = *position;
        float* const axis = (axes[n] == 0) ? &probe.x :
        ((axes[n] == 1) ? &probe.y : &probe.z);
        *axis = ((SI)*axis) + sides[n];
        *curbit = this->lookupBit(probe);
        n++;
    }
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
 */
bool Octree::rayTrace(vector<DynamicVoxel>* const voxels)
{
    if(this->ray != NULL)
    {
        float depthray = 0.0f;
        while(depthray < (float)this->raylength)
        {
            // Searches for the deepest available octant from the root,
            // corresponding to the current ray position
            Octant* curbit = this->getBit();
            this->avoidScanGlitches(&curbit);
            if(curbit->voxel != NULL)
            {
                if(!Options::TRANSPARENCY || (curbit->voxel->color & 0x000000FF) == 0xFF)
                {
                    voxels->push_back({depthray, curbit->voxel});
                    return true;
                }
                if(Options::TRANSPARENCY)
                {
                    if(voxels->size() == 0 || (voxels->size() > 0 && (depthray -
                    (*voxels)[voxels->size()-1].depth) >= Options::MIN_VOXELS_PROXIMITY))
                    {
                        voxels->push_back({depthray, curbit->voxel});
                    }
                }
            }
            // Calculates the new ray position
            this->getNextEntryDot(curbit);
            depthray += this->raystep;
        }
    }
    return voxels->size() > 0;
}


/*
 * Read-only, thread-safe ray query. It owns all its scratch on the stack,
 * leaves the tree and the caller's inputs untouched, and returns depth/color
 * copies in the same order as the legacy stepping loop.
 */
vector<RayHit> Octree::traceRay(const RayQuery& query) const
{
    vector<RayHit> hits;
    if(!(query.maxDistance > 0.0f))
    {
        return hits;
    }

    Vec3<float> position = query.origin;
    const Vec3<float> direction = query.direction;
    const Vec3<float> normalizer = query.useNormalizer ?
    query.normalizer : this->directionNormalizer(direction);

    float depthray = 0.0f;
    while(depthray < query.maxDistance)
    {
        Octant* curbit = this->lookupBit(position);
        this->avoidScanGlitchesState(&curbit, &position, query.avoidGlitches);
        if(curbit->voxel != NULL)
        {
            const unsigned int color = curbit->voxel->color;
            if(!query.transparency || (color & 0x000000FF) == 0xFF)
            {
                hits.push_back({depthray, color});
                return hits;
            }
            if(hits.size() == 0 || (depthray -
            hits[hits.size()-1].depth) >= (float)query.minVoxelProximity)
            {
                hits.push_back({depthray, color});
            }
        }
        depthray += this->nextEntryDot(curbit, direction, normalizer, &position);
    }
    return hits;
}


/*
 * rayToBorder
 */
void Octree::rayToBorder(const float a, const float b, const float c)
{
    borderStep(a, b, c, &this->raystep);
}


/*
 * getNextEntryDot
 */
void Octree::getNextEntryDot(Octant* octant)
{
    if(this->ray == NULL)
    {
      return;
    }

    if(octant->half <= 0.5f)
    {
      this->raystep = 1.0f;
      this->ray->x += this->kbase->x;
      this->ray->y += this->kbase->y;
      this->ray->z += this->kbase->z;
      return;
    }

    this->raystep = this->treeSize;
    rayToBorder(this->ray->x, octant->facescenter[
    (this->kbase->x > 0.0f) ? 1 : 0], this->kNormalizer.x);
    rayToBorder(this->ray->y, octant->facescenter[
    (this->kbase->y > 0.0f) ? 3 : 2], this->kNormalizer.y);
    rayToBorder(this->ray->z, octant->facescenter[
    (this->kbase->z > 0.0f) ? 5 : 4], this->kNormalizer.z);

    if(this->raystep < RAYSTEP_MIN_VALUE)
    {
        this->raystep = RAYSTEP_MIN_VALUE;
    }

    this->ray->x += this->kbase->x * this->raystep;
    this->ray->y += this->kbase->y * this->raystep;
    this->ray->z += this->kbase->z * this->raystep;
}


/*
 * Stateless counterpart of getNextEntryDot: same minimum step, same border
 * rule, but advancing a local position instead of instance ray scratch.
 */
float Octree::nextEntryDot(Octant* const octant, const Vec3<float>& direction,
const Vec3<float>& normalizer, Vec3<float>* const position) const
{
    float raystep;
    if(octant->half <= 0.5f)
    {
        raystep = 1.0f;
        position->x += direction.x;
        position->y += direction.y;
        position->z += direction.z;
        return raystep;
    }

    raystep = this->treeSize;
    borderStep(position->x, octant->facescenter[
    (direction.x > 0.0f) ? 1 : 0], normalizer.x, &raystep);
    borderStep(position->y, octant->facescenter[
    (direction.y > 0.0f) ? 3 : 2], normalizer.y, &raystep);
    borderStep(position->z, octant->facescenter[
    (direction.z > 0.0f) ? 5 : 4], normalizer.z, &raystep);

    if(raystep < RAYSTEP_MIN_VALUE)
    {
        raystep = RAYSTEP_MIN_VALUE;
    }

    position->x += direction.x * raystep;
    position->y += direction.y * raystep;
    position->z += direction.z * raystep;
    return raystep;
}


Vec3<float> Octree::directionNormalizer(const Vec3<float>& direction) const
{
    Vec3<float> normalizer;
    normalizer.x = math::absf(direction.x) > 0.001f ? 1.0f / direction.x : 1000.0f;
    normalizer.y = math::absf(direction.y) > 0.001f ? 1.0f / direction.y : 1000.0f;
    normalizer.z = math::absf(direction.z) > 0.001f ? 1.0f / direction.z : 1000.0f;
    return normalizer;
}


float Octree::getSize() const
{
    return this->treeSize;
}


float Octree::getHalf() const
{
    return this->treeHalf;
}


SI Octree::getRayLength() const
{
    return this->raylength;
}


bool Octree::isAccelerated() const
{
    return this->accelerated;
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
            printf(">%d ", percent);
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
    printf("\n");
}

template<typename T>
void Octree::initBit(T voxel)
{
    OctantManager::initBit(this->root, voxel, (SI)this->treeHalf);
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
