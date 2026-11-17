/*
 * Copyright (C) 2024 SINTEF ICT, Applied Mathematics, Norway.
 *
 * Contact information: E-mail: tor.dokken@sintef.no
 * SINTEF ICT, Department of Applied Mathematics,
 * P.O. Box 124 Blindern,
 * 0314 Oslo, Norway.
 *
 * This file is part of GoTools.
 *
 * GoTools is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * GoTools is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public
 * License along with GoTools. If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * In accordance with Section 7(b) of the GNU Affero General Public
 * License, a covered work must retain the producer line in every data
 * file that is created or manipulated using GoTools.
 *
 * Other Usage
 * You can be released from the requirements of the license by purchasing
 * a commercial license. Buying such a license is mandatory as soon as you
 * develop commercial activities involving the GoTools library without
 * disclosing the source code of your own applications.
 *
 * This file may be used in accordance with the terms contained in a
 * written agreement between you and SINTEF ICT.
 */

// Example: locally refine a rational LR B-spline surface, coarsen the
// resulting mesh with LRSplineSurface::tryCoarsen (an exact inverse of the
// refinement in this case), and write the coarsened surface to an OBJ file
// through the production LRSurfaceTesselator path.
//
// Usage: lr_coarsen_obj <output.obj>

#include "GoTools/geometry/SplineSurface.h"
#include "GoTools/lrsplines2D/LRSplineSurface.h"
#include "GoTools/viewlib/vol_and_lr/LRSurfaceTesselator.h"

#include <fstream>
#include <iostream>
#include <vector>
#include <cstdlib>

using namespace Go;

namespace
{

// A rational bi-quadratic surface with non-uniform weights.
LRSplineSurface* make_rational_surface()
{
  const int deg = 2;
  const int n = deg + 1;
  std::vector<double> knots;
  knots.insert(knots.end(), n, 0.0);
  knots.insert(knots.end(), n, 1.0);

  std::vector<double> rcoefs;
  for (int j = 0; j < n; ++j)
    for (int i = 0; i < n; ++i)
      {
	const double w = 1.0 + 0.7*i + 0.3*j;
	const double x = 0.5*i;
	const double y = 0.5*j;
	const double z = 0.2*i*j + 0.1*i*i + 0.07*j;
	rcoefs.insert(rcoefs.end(), {w*x, w*y, w*z, w});
      }
  SplineSurface spline(n, n, deg + 1, deg + 1,
		       knots.data(), knots.data(), rcoefs.data(),
		       3, true);
  return new LRSplineSurface(&spline, 1.0e-10);
}

} // anonymous namespace

int main(int argc, char* argv[])
{
  if (argc < 2)
    {
      std::cerr << "Usage: " << argv[0] << " <output.obj>" << std::endl;
      return 1;
    }

  LRSplineSurface* lr = make_rational_surface();

  // Local refinements: full-span crossing lines and a partial T-joint line.
  lr->refine(XFIXED, 0.5, 0.0, 1.0);
  lr->refine(YFIXED, 0.5, 0.0, 1.0);
  lr->refine(XFIXED, 0.25, 0.0, 0.5);
  const int refined_basis = lr->numBasisFunctions();
  const int refined_elements = lr->numElements();

  // Pull the mesh lines back.  The batch is the exact inverse of the
  // refinements, so coarsening must succeed without changing the geometry.
  std::vector<LRSplineSurface::Refinement2D> requests(3);
  requests[0].setVal(0.25, 0.0, 0.5, XFIXED, 1);
  requests[1].setVal(0.5, 0.0, 1.0, XFIXED, 1);
  requests[2].setVal(0.5, 0.0, 1.0, YFIXED, 1);
  if (!lr->tryCoarsen(requests))
    {
      std::cerr << "Coarsening failed unexpectedly." << std::endl;
      delete lr;
      return 2;
    }
  if (lr->numBasisFunctions() >= refined_basis ||
      lr->numElements() >= refined_elements)
    {
      std::cerr << "Coarsening did not reduce the mesh." << std::endl;
      delete lr;
      return 3;
    }

  // Production tesselation path.
  LRSurfaceTesselator tesselator(*lr);
  tesselator.changeRes(21, 17);
  shared_ptr<GenericTriMesh> mesh = tesselator.getMesh();
  if (mesh->numVertices() <= 0 || mesh->numTriangles() <= 0)
    {
      std::cerr << "Empty tesselation." << std::endl;
      delete lr;
      return 4;
    }

  std::ofstream os(argv[1]);
  if (!os)
    {
      std::cerr << "Cannot open output file: " << argv[1] << std::endl;
      delete lr;
      return 5;
    }
  os.precision(10);

  // GoTools producer notice, retained as an OBJ comment.
  os << "# Produced using GoTools (SINTEF ICT, Applied Mathematics)\n";
  os << "# LR B-spline surface coarsened with LRSplineSurface::tryCoarsen\n";

  const double* vertices = mesh->vertexArray();
  const double* normals = mesh->normalArray();
  const unsigned int* triangles = mesh->triangleIndexArray();

  for (int i = 0; i < mesh->numVertices(); ++i)
    os << "v " << vertices[3*i] << ' ' << vertices[3*i + 1] << ' '
       << vertices[3*i + 2] << '\n';
  for (int i = 0; i < mesh->numVertices(); ++i)
    os << "vn " << normals[3*i] << ' ' << normals[3*i + 1] << ' '
       << normals[3*i + 2] << '\n';
  for (int i = 0; i < mesh->numTriangles(); ++i)
    {
      // OBJ indices are 1-based.
      const unsigned int a = triangles[3*i] + 1;
      const unsigned int b = triangles[3*i + 1] + 1;
      const unsigned int c = triangles[3*i + 2] + 1;
      os << "f " << a << "//" << a << ' ' << b << "//" << b << ' '
	 << c << "//" << c << '\n';
    }
  os.close();
  if (!os)
    {
      std::cerr << "Error while writing OBJ file." << std::endl;
      delete lr;
      return 6;
    }

  std::cout << "Wrote " << argv[1] << ": " << mesh->numVertices()
	    << " vertices, " << mesh->numTriangles() << " triangles; "
	    << "basis " << refined_basis << " -> " << lr->numBasisFunctions()
	    << ", elements " << refined_elements << " -> "
	    << lr->numElements() << std::endl;

  delete lr;
  return 0;
}
