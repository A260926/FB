/********************************************************************************
*  This file is part of CinoLib                                                 *
*  Copyright(C) 2016: Marco Livesu                                              *
*                                                                               *
*  The MIT License                                                              *
*                                                                               *
*  Permission is hereby granted, free of charge, to any person obtaining a      *
*  copy of this software and associated documentation files (the "Software"),   *
*  to deal in the Software without restriction, including without limitation    *
*  the rights to use, copy, modify, merge, publish, distribute, sublicense,     *
*  and/or sell copies of the Software, and to permit persons to whom the        *
*  Software is furnished to do so, subject to the following conditions:         *
*                                                                               *
*  The above copyright notice and this permission notice shall be included in   *
*  all copies or substantial portions of the Software.                          *
*                                                                               *
*  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR   *
*  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,     *
*  FITNESS FOR A PARTICULAR PURPOSE AND NON INFRINGEMENT. IN NO EVENT SHALL THE *
*  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER       *
*  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING      *
*  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS *
*  IN THE SOFTWARE.                                                             *
*                                                                               *
*  Author(s):                                                                   *
*                                                                               *
*     Marco Livesu (marco.livesu@gmail.com)                                     *
*     http://pers.ge.imati.cnr.it/livesu/                                       *
*                                                                               *
*     Italian National Research Council (CNR)                                   *
*     Institute for Applied Mathematics and Information Technologies (IMATI)    *
*     Via de Marini, 6                                                          *
*     16149 Genoa,                                                              *
*     Italy                                                                     *
*********************************************************************************/
#include <cinolib/singularity_field.h>
#include <cmath>
#include <cstdio>
#include <vector>

/* Computes a four-direction field with a non zero integer period on a torus
 * and serializes the per face representatives (a cinolib::VectorField) to the
 * file given as the first command line argument. Energy and constraint
 * residual of the solution are printed on standard output. */
int main(int argc, char ** argv)
{
    using namespace cinolib;
    if(argc<2)
    {
        std::fprintf(stderr,"usage: %s <output.field>\n",argv[0]);
        return 1;
    }

    // asymmetric torus grid
    const double pi = std::acos(-1.0);
    const uint   N  = 24, M = 16;
    std::vector<vec3d> p;
    std::vector<uint>  f;
    for(uint i=0;i<N;++i) for(uint j=0;j<M;++j)
    {
        double u = 2*pi*i/N;
        double v = 2*pi*j/M;
        double R = 3.0 + 0.25*std::sin(2*u+1.0) + 0.1*std::cos(3*v+0.5);
        double r = 1.0 + 0.15*std::cos(3*u+0.3) + 0.05*std::sin(2*v+2.0);
        p.emplace_back((R+r*std::cos(v))*std::cos(u),
                       (R+r*std::cos(v))*std::sin(u),
                       r*std::sin(v));
    }
    for(uint i=0;i<N;++i) for(uint j=0;j<M;++j)
    {
        uint a= i      *M+j;
        uint b=((i+1)%N)*M+j;
        uint c=((i+1)%N)*M+(j+1)%M;
        uint d= i      *M+(j+1)%M;
        f.insert(f.end(),{a,b,c,a,c,d});
    }
    Trimesh<> m(p,f);

    SingularityField sf(m);
    if(sf.genus()!=1) return 2;

    // genus 1: sum(q)=4*chi=0, two handle periods; ask for one quarter turn
    // around the first basis cycle
    std::vector<int> q(sf.num_verts(),0);
    std::vector<int> periods = {1,0};
    vec3d anchor = sf.frames().at(0).u;
    if(!sf.solve(q,periods,anchor)) return 3;

    sf.field().serialize(argv[1]);

    std::printf("EXAMPLE_FACES=%u\n",           sf.num_faces());
    std::printf("EXAMPLE_ENERGY=%.12g\n",       sf.diagnostics().energy);
    std::printf("EXAMPLE_MAX_RESIDUAL=%.3g\n",  sf.diagnostics().max_constraint_residual);
    std::printf("EXAMPLE_FACTORIZATIONS=%u\n",  sf.diagnostics().sparse_factorizations);
    return 0;
}
