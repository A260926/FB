#pragma once
#include <stdint.h>

enum TestFlags
{
    kFlagAnimate = (1 << 0),
    kFlagProgressive = (1 << 1),
    kFlagIrradianceCache = (1 << 2),
};

// Row scheduling for CPU draws, used to verify cache determinism.
// 0 = enkiTS task scheduler (production default), 1 = sequential forward,
// 2 = sequential reverse. Pixel RNG and cache gather streams are identity
// based, so all three produce identical records and linear RGB.
void SetRowScheduleMode(int mode);
int GetRowScheduleMode();

void InitializeTest();
void ShutdownTest();

void UpdateTest(float time, int frameCount, int screenWidth, int screenHeight, unsigned testFlags);
void DrawTest(float time, int frameCount, int screenWidth, int screenHeight, float* backbuffer, int& outRayCount, unsigned testFlags);

void GetObjectCount(int& outCount, int& outObjectSize, int& outMaterialSize, int& outCamSize);
void GetSceneDesc(void* outObjects, void* outMaterials, void* outCam, void* outEmissives, int* outEmissiveCount);
