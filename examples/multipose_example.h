#pragma once
#ifndef XATLAS_MULTIPOSE_EXAMPLE_H
#define XATLAS_MULTIPOSE_EXAMPLE_H

// Windowless example: runs AddMesh -> ComputeCharts -> PackCharts for a small
// mesh that shares one baked UV set across a binding pose and two deformed
// poses, then writes an OBJ containing the source vertex positions and the
// generated UVs. No model files are downloaded. Does not define main().
//
// Returns true on success. If outChartCount is not null it receives the number
// of charts in the generated atlas.
bool WriteMultiposeExampleObj(const char *filename, unsigned int *outChartCount);

// Returns true when the example mesh was successfully charted and packed.
bool RunMultiposeExample(unsigned int *outChartCount);

#endif // XATLAS_MULTIPOSE_EXAMPLE_H
