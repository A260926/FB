#include <igl/qslim.h>
#include <igl/decimate.h>
#include <igl/adjacency_matrix.h>
#include <iostream>
#include <cmath>
int main(){
  Eigen::MatrixXd V(6,3),U; Eigen::MatrixXi F(8,3),G;
  V<<1,0,0,-1,0,0,0,1,0,0,-1,0,0,0,1,0,0,-1;
  F<<0,2,4,2,1,4,1,3,4,3,0,4,2,0,5,1,2,5,3,1,5,0,3,5;
  Eigen::VectorXi J,I;
  if(!igl::qslim(V,F,6,false,U,G,J,I)||G.rows()!=6||!U.allFinite()) return 1;
  for(int i=0;i<G.rows();++i)for(int j=0;j<3;++j)if(G(i,j)<0||G(i,j)>=U.rows())return 2;
  std::cout<<"QSlim faces="<<G.rows()<<" vertices="<<U.rows()<<"\n";
  if(!igl::decimate(V,F,6,false,U,G,J,I)||G.rows()!=6) return 3;
  std::cout<<"Decimate faces="<<G.rows()<<" vertices="<<U.rows()<<"\n";
  Eigen::SparseMatrix<double> A; igl::adjacency_matrix(F,A);
  if(A.rows()!=6||A.nonZeros()!=24) return 4;
  Eigen::VectorXd degrees=A*Eigen::VectorXd::Ones(6);
  if((degrees.array()-4).abs().maxCoeff()>1e-12)return 5;
  std::cout<<"Graph adjacency_nnz="<<A.nonZeros()<<" degree="<<degrees(0)<<"\nLIBIGL_BASELINE_COMPLETE\n";
}
