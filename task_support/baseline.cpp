#include "fixture.hpp"
#include <iostream>
#include <string>
int main(int argc,char** argv) {
    try {
        if(argc!=2) return 2;
        std::string dir=argv[1];
        for(unsigned mode=0;mode<2;++mode) {
            auto f=pair_fixture::make(mode?3:1,1,mode!=0,false);
            spirv_cross::CompilerGLSL c(f.words);c.set_common_options(pair_fixture::options());
            c.build_combined_image_samplers();
            auto mappings=c.get_combined_image_samplers();
            if(mappings.size()!=1 || mappings[0].image_id!=f.images || mappings[0].sampler_id!=f.samplers) return 3;
            auto id=mappings[0].combined_id;
            c.set_name(id,"paired");c.set_decoration(id,spv::DecorationBinding,7);
            auto t=c.get_type_from_variable(id);
            if(mode && (t.array.size()!=1 || t.array[0]!=3)) return 4;
            if(!mode && !t.array.empty()) return 5;
            auto resources=c.get_shader_resources();
            bool reflected=false;
            for(const auto& r:resources.sampled_images)
                if(r.id==id && c.get_decoration(r.id,spv::DecorationBinding)==7) reflected=true;
            if(!reflected) return 6;
            auto glsl=c.compile();
            std::ofstream(dir+"/"+(mode?"image-array":"scalar")+".glsl")<<glsl;
            std::ofstream slots(dir+"/"+(mode?"image-array":"scalar")+".slots");
            unsigned count=mode?t.array[0]:1;
            for(unsigned k=0;k<count;++k)
                slots<<c.get_decoration(id,spv::DecorationBinding)<<" "<<k<<" "<<k<<" 0\n";
            std::cout<<"BASELINE_RESOURCE "<<(mode?"image-array":"scalar")<<" binding=7 slots="<<f.n<<"\n";
        }
        // Two-axis flattening: an image axis and a sampler axis flattened into one
        // sampler2D[N * M] array, with the reflected slot table written for the bridge.
        auto f=pair_fixture::make(2,3,true,true);
        spirv_cross::CompilerGLSL c(f.words);c.set_common_options(pair_fixture::options());
        try {
            c.build_combined_image_samplers();
            auto mappings=c.get_combined_image_samplers();
            if(mappings.size()!=1 || mappings[0].image_id!=f.images || mappings[0].sampler_id!=f.samplers) return 7;
            auto id=mappings[0].combined_id;
            c.set_name(id,"paired");c.set_decoration(id,spv::DecorationBinding,7);
            auto t=c.get_type_from_variable(id);
            if(t.array.size()!=1 || t.array[0]!=f.n*f.m) return 8;
            std::ofstream(dir+"/two-axis.glsl")<<c.compile();
            auto bindings=c.get_combined_image_sampler_bindings(id);
            if(bindings.size()!=f.n*f.m) return 9;
            std::ofstream slots(dir+"/two-axis.slots");
            for(auto& b:bindings)
                slots<<c.get_decoration(id,spv::DecorationBinding)<<" "
                      <<b.combined_index<<" "<<b.image_index<<" "<<b.sampler_index<<"\n";
            std::cout<<"TWO_AXIS_OBSERVATION supported slots="<<bindings.size()<<"\n";
        }
        catch(const spirv_cross::CompilerError& e){std::cout<<"TWO_AXIS_OBSERVATION "<<e.what()<<"\n";}
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
