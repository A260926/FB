#pragma once
#include <opensubdiv/far/topologyDescriptor.h>
#include <memory>
#include <vector>
struct Grid {
    std::vector<double> positions, uv;
    std::vector<int> sizes, indices, uvIndices;
    std::unique_ptr<OpenSubdiv::Far::TopologyRefiner> topology;
    Grid() {
        for(int y=0;y<5;++y) for(int x=0;x<5;++x) {
            positions.insert(positions.end(),{double(x),double(y),0.0});
        }
        for(int y=0;y<4;++y) for(int x=0;x<4;++x) {
            sizes.push_back(4);
            indices.insert(indices.end(),{y*5+x,y*5+x+1,(y+1)*5+x+1,(y+1)*5+x});
            for(int k=0;k<4;++k) { uvIndices.push_back(int(uvIndices.size())); uv.push_back(double(x*10+(k==1||k==2))); uv.push_back(double(y*10+(k>=2))); }
        }
        OpenSubdiv::Far::TopologyDescriptor d;
        d.numVertices=25; d.numFaces=16; d.numVertsPerFace=sizes.data();d.vertIndicesPerFace=indices.data();
        OpenSubdiv::Far::TopologyDescriptor::FVarChannel channel;
        channel.numValues=64;channel.valueIndices=uvIndices.data();d.numFVarChannels=1;d.fvarChannels=&channel;
        OpenSubdiv::Far::TopologyRefinerFactory<OpenSubdiv::Far::TopologyDescriptor>::Options o(OpenSubdiv::Sdc::SCHEME_CATMARK);
        o.schemeOptions.SetVtxBoundaryInterpolation(OpenSubdiv::Sdc::Options::VTX_BOUNDARY_EDGE_ONLY);
        topology.reset(OpenSubdiv::Far::TopologyRefinerFactory<OpenSubdiv::Far::TopologyDescriptor>::Create(d,o));
    }
};
