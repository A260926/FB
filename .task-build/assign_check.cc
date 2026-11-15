#include <vector>
#include <algorithm>
#include <random>
#include <limits>
#include <cstdio>
#include <cmath>
using namespace std;
static void min_cost_assignment(const vector<vector<double>>&cost,vector<int>&row_to_col){
 int n=cost.size();row_to_col.assign(n,-1);if(!n)return;int m=cost[0].size();
 const double INF=numeric_limits<double>::infinity();
 vector<double> u(n+1,0),v(m+1,0);vector<int>p(m+1,0),way(m+1,0);
 for(int i=1;i<=n;i++){p[0]=i;int j0=0;vector<double>minv(m+1,INF);vector<char>used(m+1,0);
  do{used[j0]=1;int i0=p[j0],j1=0;double delta=INF;
   for(int j=1;j<=m;j++){if(used[j])continue;double cur=cost[i0-1][j-1]-u[i0]-v[j];
    if(cur<minv[j]){minv[j]=cur;way[j]=j0;} if(minv[j]<delta){delta=minv[j];j1=j;}}
   for(int j=0;j<=m;j++){if(used[j]){u[p[j]]+=delta;v[j]-=delta;}else minv[j]-=delta;}
   j0=j1;}while(p[j0]!=0);
  do{int j1=way[j0];p[j0]=p[j1];j0=j1;}while(j0);}
 for(int j=1;j<=m;j++)if(p[j]>=1&&p[j]<=n)row_to_col[p[j]-1]=j-1;
}
int main(){
 mt19937 rng(42);uniform_real_distribution<double>U(0,1);
 for(int n=1;n<=5;n++)for(int trial=0;trial<3000;trial++){
  vector<vector<double>>C(n,vector<double>(n));
  for(auto&r:C)for(auto&x:r)x=U(rng);
  vector<int>a;min_cost_assignment(C,a);
  double got=0;for(int i=0;i<n;i++)got+=C[i][a[i]];
  vector<int>p(n);for(int i=0;i<n;i++)p[i]=i;double best=1e300;
  do{double s=0;for(int i=0;i<n;i++)s+=C[i][p[i]];best=min(best,s);}while(next_permutation(p.begin(),p.end()));
  if(fabs(got-best)>1e-9){printf("FAIL n=%d got %.12f best %.12f\n",n,got,best);return 1;}
 }
 printf("ASSIGN OK\n");
}
