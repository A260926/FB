// Short example of shared refinement: several 3D surfaces share one
// ParameterSpace.  Enable the mode once, construct the members, and any
// InsertKnot/RefineKnots/ElevateDegree call on one member performs the same
// shape-preserving edit on every member automatically.
//
// Build (from the repository root) with the same translation units listed in
// task_support/core-sources.txt, e.g.:
//   c++ -std=c++20 -I. -ISources examples/shared_refinement_example.cpp \
//       $(sed 's#^#/#' task_support/core-sources.txt | tr '\n' ' ') \
//       -o shared_refinement_demo
#include <iostream>
#include <vector>

#include "BSplineLib/Splines/b_spline.hpp"
#include "BSplineLib/Splines/nurbs.hpp"

int main() {
  using namespace bsplinelib;
  using ParameterSpace = parameter_spaces::ParameterSpace<2>;
  using KnotVector = parameter_spaces::KnotVector;
  using VectorSpace = vector_spaces::VectorSpace;
  using WeightedVectorSpace = vector_spaces::WeightedVectorSpace;

  // One shared parameter space (bilinear patches).
  auto u = std::make_shared<KnotVector>(std::vector<double>{0, 0, 1, 1});
  auto v = std::make_shared<KnotVector>(std::vector<double>{0, 0, 1, 1});
  auto parameter_space = std::make_shared<ParameterSpace>(
      ParameterSpace::KnotVectors_{u, v}, ParameterSpace::Degrees_{1, 1});

  // Opt in before constructing the members; it cannot be switched off again.
  parameter_space->EnableSharedRefinement();

  // Two ordinary 3D control nets and one weighted (rational) net.
  VectorSpace::Coordinates_ ca(4, 3), cb(4, 3);
  for (int i = 0; i < 4; ++i) {
    ca(i, 0) = (i % 2);
    ca(i, 1) = (i / 2);
    ca(i, 2) = 0.5 * (i % 2) * (i / 2);
    cb(i, 0) = 2.0 * (i % 2);
    cb(i, 1) = (i / 2);
    cb(i, 2) = 3.0;
  }
  WeightedVectorSpace::Coordinates_ rc(4, 3);
  WeightedVectorSpace::Weights_ weights(4);
  for (int i = 0; i < 4; ++i) {
    rc(i, 0) = (i % 2);
    rc(i, 1) = (i / 2);
    rc(i, 2) = 0.2;
    weights[i] = 0.5 + 0.25 * i; // non-unit weights
  }
  auto net_a = std::make_shared<VectorSpace>(std::move(ca));
  auto net_b = std::make_shared<VectorSpace>(std::move(cb));
  auto net_w = std::make_shared<WeightedVectorSpace>(rc, weights);

  splines::BSpline<2> surface_a(parameter_space, net_a);
  splines::BSpline<2> surface_b(parameter_space, net_b);
  splines::Nurbs<2> surface_n(parameter_space, net_w);

  double const uv[2] = {0.25, 0.75};
  auto const before = surface_a(uv);

  // Edit any member; the whole group receives the same knots and degrees.
  surface_a.InsertKnot(0, 0.5);     // insert a U knot through the first member
  surface_b.RefineKnots(1, {0.25}); // refine V through another member
  surface_n.ElevateDegree(0);       // elevate the U degree through the Nurbs

  // All control nets match the single shared basis, and the geometry is
  // unchanged up to floating-point round-off.
  std::cout << "basis functions: "
            << parameter_space->GetTotalNumberOfBasisFunctions()
            << "  net sizes: " << net_a->GetNumberOfCoordinates() << ", "
            << net_b->GetNumberOfCoordinates() << ", "
            << net_w->GetNumberOfCoordinates() << '\n';
  auto const after = surface_a(uv);
  std::cout << "value before: (" << before[0] << ", " << before[1] << ", "
            << before[2] << ")\n";
  std::cout << "value after:  (" << after[0] << ", " << after[1] << ", "
            << after[2] << ")\n";
  return 0;
}
