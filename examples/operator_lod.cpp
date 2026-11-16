// Example for igl::qslim_operator: build a subdivided closed shell, simplify
// it while preserving a scalar harmonic operator, then prolongate a
// non-constant coarse field back onto the fine mesh.
//
// Usage: operator_lod [output_dir]
// Writes coarse.obj and fine_field.csv into output_dir (default: ".").
#include <igl/qslim_operator.h>
#include <igl/loop.h>
#include <igl/edge_flaps.h>
#include <igl/writeOBJ.h>

#include <Eigen/Core>
#include <Eigen/Sparse>

#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{
  Eigen::MatrixXd octa_V()
  {
    Eigen::MatrixXd V(6,3);
    V << 1,0,0, -1,0,0, 0,1,0, 0,-1,0, 0,0,1, 0,0,-1;
    return V;
  }
  Eigen::MatrixXi octa_F()
  {
    Eigen::MatrixXi F(8,3);
    F << 0,2,4, 2,1,4, 1,3,4, 3,0,4,
         2,0,5, 1,2,5, 3,1,5, 0,3,5;
    return F;
  }

  // Unit-conductance graph Laplacian on the mesh edges.
  Eigen::SparseMatrix<double> make_laplacian(
    const Eigen::MatrixXi & F,const int n)
  {
    Eigen::MatrixXi E,EF,EI; Eigen::VectorXi EMAP;
    igl::edge_flaps(F,E,EMAP,EF,EI);
    std::vector<Eigen::Triplet<double>> trips;
    std::vector<double> diag(n,0.0);
    for(int e = 0;e<E.rows();e++)
    {
      const int a = E(e,0), b = E(e,1);
      trips.emplace_back(a,b,-1.0);
      trips.emplace_back(b,a,-1.0);
      diag[a] += 1.0; diag[b] += 1.0;
    }
    for(int v = 0;v<n;v++) trips.emplace_back(v,v,diag[v]);
    Eigen::SparseMatrix<double> L(n,n);
    L.setFromTriplets(trips.begin(),trips.end());
    return L;
  }
}

int main(int argc,char ** argv)
{
  const std::string dir = argc > 1 ? std::string(argv[1]) : std::string(".");

  // Subdivided closed shell.
  Eigen::MatrixXd V0 = octa_V(), V;
  Eigen::MatrixXi F0 = octa_F(), F;
  igl::loop(V0,F0,V,F,2);
  const int n = V.rows();
  std::cout << "fine mesh: " << V.rows() << " vertices, "
            << F.rows() << " faces\n";

  Eigen::SparseMatrix<double> L0 = make_laplacian(F,n);

  Eigen::MatrixXd U;
  Eigen::MatrixXi G;
  Eigen::VectorXi J,I;
  Eigen::SparseMatrix<double> K,P;
  igl::qslim_operator_trace trace;
  const int max_m = 24;
  const double lambda = 1.0;
  const bool reached =
    igl::qslim_operator(V,F,L0,max_m,lambda,U,G,J,I,K,P,&trace);
  std::cout << "coarse mesh: " << U.rows() << " vertices, "
            << G.rows() << " faces (target " << max_m << ", "
            << (reached ? "reached" : "not reached") << ")\n";
  std::cout << "collapses: " << trace.num_collapsed
            << ", pops: " << trace.entries.size() << "\n";

  // A deliberately non-constant coarse scalar field.
  Eigen::VectorXd xc(U.rows());
  for(int i = 0;i<U.rows();i++)
    xc(i) = std::sin(2.0*U(i,0)) + 0.7*std::cos(3.0*U(i,1)) + U(i,2);

  // Harmonic prolongation to the fine vertices.
  const Eigen::VectorXd xf = P*xc;
  std::cout << "prolongated field: " << xf.rows() << " values\n";

  if(!igl::writeOBJ(dir + "/coarse.obj",U,G))
  {
    std::cerr << "failed to write coarse.obj\n";
    return 1;
  }
  std::ofstream csv(dir + "/fine_field.csv");
  if(!csv)
  {
    std::cerr << "failed to open fine_field.csv\n";
    return 1;
  }
  csv.precision(17);
  for(int i = 0;i<xf.rows();i++) csv << xf(i) << "\n";
  std::cout << "wrote " << dir << "/coarse.obj and "
            << dir << "/fine_field.csv\n";
  return 0;
}
