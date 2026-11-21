/*
 * Cloud of voxels (CoV) project
 * Author: mcidclan, m [.D'O'T.] cid [.D'O'T.] clan [.A'T.] gmail [.D'O'T.] com
 * Creation Date: 2011
 * Modification Date: 2020
 */

#ifndef CAMERA_H
#define CAMERA_H

    #include "math.h"

    class Camera
    {
        public:
        /*
        * Constructor
        */
        Camera();


        /*
        * Destructor
        */
        ~Camera();


        /*
        * setPosition
        */
        void setPosition(const float x, const float y , const float z);


        /*
        * resetTransformation
        */
        void resetTransformation();


        /*
        * rotate
        */
        void rotate(const Vec3<float> axis, float angle);


        /*
        * translate
        */
        void translate(const Vec3<float> v);


        /*
        * getBasis
        */
        void getBasis(Mat3f* const basis);


        /*
        * getPosition
        */
        Vec3<float> getPosition() const;


        /*
        * Apply translation to a given coordinates
        */
        void reajust(Vec3<float>* const coordinates);


        private:
        Mat3f basis;
        Vec3<float> nearcenter;

        /*
        * Rotation state is owned by each instance, so interleaved cameras
        * never accumulate each other's rotations.
        */
        float matrot[16];
        float matrot_1[16];
        float matrot_2[16];


        private:
        /*
        * updateNearInfo
        */
        void updateNearInfo(float* const m);

    };

#endif

