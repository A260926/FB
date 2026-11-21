/*
 * Cloud of voxels (CoV) project
 * Author: mcidclan, m [.D'O'T.] cid [.D'O'T.] clan [.A'T.] gmail [.D'O'T.] com
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
    this->nearcenter.x =
    this->nearcenter.y =
    this->nearcenter.z = 0.0f;
    this->basis.i.x = this->basis.j.y = this->basis.k.z = 1.0f;
    this->basis.i.y = this->basis.i.z = 0.0f;
    this->basis.j.x = this->basis.j.z = 0.0f;
    this->basis.k.x = this->basis.k.y = 0.0f;
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
    memset(this->matrot, 0x0, sizeof(this->matrot));
    this->matrot[0] =
    this->matrot[5] =
    this->matrot[10] =
    this->matrot[15] = 1.0f;
}


/*
* rotate
*/
void Camera::rotate(const Vec3<float> axis, float angle)
{
    math::eulerorientation(axis, angle, this->matrot_1);
    math::mat4xmat4(this->matrot, this->matrot_1, this->matrot_2);

    memcpy(this->matrot, this->matrot_2, sizeof(this->matrot));
    this->updateNearInfo(this->matrot);
}


/*
* translate
*/
void Camera::translate(const Vec3<float> v)
{
    math::translate(v, this->matrot);
    this->nearcenter.x = this->matrot[12];
    this->nearcenter.y = this->matrot[13];
    this->nearcenter.z = this->matrot[14];
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
Vec3<float> Camera::getPosition() const
{
    return this->nearcenter;
}


/*
* Reajust the given coordinates to the camera center
*/
void Camera::reajust(Vec3<float>* const coordinates)
{
    math::vecadd(this->nearcenter, coordinates);
}
