/*
 * Cloud of voxels (CoV) project
 * Author: mcidclan, m [.D'O'T.] cid [.D'O'T.] clan [.A'T.] gmail [.A'T.] com
 * Creation Date: 2011
 * Modification Date: 2020
 */

#include "./headers/Camera.h"


/*
* extern midentity
*/
extern const float midentity[16];


/*
* Constructor
*/
Camera::Camera()
{
    this->resetTransformation();
    this->basis = {
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f}
    };
    this->nearcenter.x =
    this->nearcenter.y =
    this->nearcenter.z = 0.0f;
}


/*
* Destructor
*/
Camera::~Camera()
{
}


/*
* updateNearInfo
*/
void Camera::updateNearInfo(float* const m)
{
    memcpy(&(this->basis.i), &m[0], sizeof(Vec3<float>));
    memcpy(&(this->basis.j), &m[4], sizeof(Vec3<float>));
    memcpy(&(this->basis.k), &m[8], sizeof(Vec3<float>));

    math::normalize(&(this->basis.i));
    math::normalize(&(this->basis.j));
    math::normalize(&(this->basis.k));
}


/*
* position
*/
void Camera::setPosition(const float x, const float y , const float z)
{
    this->nearcenter.x = x;
    this->nearcenter.y = y;
    this->nearcenter.z = z;
}

/*
* resetTransformation
*/
void Camera::resetTransformation()
{
    memset(this->rotation, 0x0, sizeof(this->rotation));
    this->rotation[0] =
    this->rotation[5] =
    this->rotation[10] =
    this->rotation[15] = 1.0f;
}


/*
* rotate
*/
void Camera::rotate(const Vec3<float> axis, float angle)
{
    float mattmp_1[16] __attribute__ ((aligned (__BIGGEST_ALIGNMENT__)));
    float mattmp_2[16] __attribute__ ((aligned (__BIGGEST_ALIGNMENT__)));
    math::eulerorientation(axis, angle, mattmp_1);
    math::mat4xmat4(this->rotation, mattmp_1, mattmp_2);

    memcpy(this->rotation, mattmp_2, sizeof(this->rotation));
    this->updateNearInfo(this->rotation);
}


/*
* translate
*/
void Camera::translate(const Vec3<float> v)
{
    math::translate(v, this->rotation);
    this->nearcenter.x = this->rotation[12];
    this->nearcenter.y = this->rotation[13];
    this->nearcenter.z = this->rotation[14];
}


/*
* getBasis
*/
void Camera::getBasis(Mat3f* const basis)
{
    *basis = this->basis;
}


/*
* getPosition
*/
void Camera::getPosition(Vec3<float>* const position) const
{
    *position = this->nearcenter;
}


/*
* Reajust the given coordinates to the camera center
*/
void Camera::reajust(Vec3<float>* const coordinates)
{
    math::vecadd(this->nearcenter, coordinates);
}
