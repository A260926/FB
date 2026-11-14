#include "Cpp/FastNoiseLite.h"
#include <cassert>
#include <cmath>
#include <cstdio>
int main(){
 FastNoiseLite n(42);n.SetNoiseType(FastNoiseLite::NoiseType_Cellular);n.SetFrequency(1);n.SetCellularJitter(0);
 n.SetCellularDistanceFunction(FastNoiseLite::CellularDistanceFunction_Euclidean);
 n.SetCellularReturnType(FastNoiseLite::CellularReturnType_Distance);
 float a=n.GetNoise(0.125,0.25);assert(std::fabs(a-(std::sqrt(0.078125)-1))<2e-6);
 n.SetCellularReturnType(FastNoiseLite::CellularReturnType_Distance2);
 float b=n.GetNoise(0.125,0.25);assert(std::fabs(b-(std::sqrt(0.578125)-1))<2e-6);
 n.SetCellularJitter(1);double sum=0;for(int y=-3;y<=3;y++)for(int x=-3;x<=3;x++)sum+=n.GetNoise(x*.37,y*.29);
 assert(std::isfinite(sum));
 FastNoiseLite w(19);w.SetDomainWarpType(FastNoiseLite::DomainWarpType_BasicGrid);w.SetDomainWarpAmp(2);float x=.3f,y=-.8f;w.DomainWarp(x,y);assert(std::isfinite(x)&&std::isfinite(y));assert(x!=.3f||y!=-.8f);
 std::printf("CELLULAR_SCALAR_OK f1=%.9g f2=%.9g checksum=%.9g\nBASICGRID_WARP_OK\n",a,b,sum);
}
