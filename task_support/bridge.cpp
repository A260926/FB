#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
// Compile the emitted fixture's expressions and helper calls as C++.
// Only interface syntax is translated. Indexing and call bodies remain generated code.
int main(int argc,char** argv) {
    try {
        if(argc!=6) return 2;
        unsigned n=unsigned(std::stoul(argv[3])),m=unsigned(std::stoul(argv[4]));
        if(!n || !m || n>32 || m>32) return 3;
        std::ifstream input(argv[1]); if(!input) return 4;
        std::ostringstream buf;buf<<input.rdbuf();std::string src=buf.str();
        if(src.find("#version 430")!=0) return 5;
        std::smatch resource;
        if(!std::regex_search(src,resource,std::regex("layout\\s*\\(binding\\s*=\\s*([0-9]+)\\)\\s*uniform\\s+sampler2D\\s+paired"))) return 8;
        unsigned emitted_binding=unsigned(std::stoul(resource[1]));
        if(emitted_binding>4096-n*m) return 8;
        src=std::regex_replace(src,std::regex("#[^\\n]*\\n"),"");
        src=std::regex_replace(src,std::regex("layout\\s*\\([^)]*\\)\\s*"),"");
        src=std::regex_replace(src,std::regex("uniform\\s+Control"),"struct Control");
        src=std::regex_replace(src,std::regex("uniform\\s+sampler2D"),"sampler2D");
        src=std::regex_replace(src,std::regex("out\\s+vec4"),"vec4");
        src=std::regex_replace(src,std::regex("void\\s+main\\s*\\("),"void shader_main(");
        std::ofstream out(argv[2]);if(!out) return 6;
        out<<R"CPP(
#include <iostream>
#include <cmath>
#include <vector>
using uint=unsigned;
struct vec2 {float x,y;explicit vec2(float f):x(f),y(f){} vec2(float a,float b):x(a),y(b){}};
struct vec4 {float x,y,z,w;vec4():x(0),y(0),z(0),w(0){} vec4(float a,float b,float c,float d):x(a),y(b),z(c),w(d){}};
struct sampler2D {unsigned image,sampler;};
struct Event {unsigned image,sampler;float x,y,lod;};
std::vector<Event> events;
vec4 textureLod(sampler2D s,vec2 uv,float lod) {
 events.push_back({s.image,s.sampler,uv.x,uv.y,lod});
 return vec4(float(19*s.image+3*s.sampler+1),float(s.image+2),float(s.sampler+5),uv.x+uv.y+lod);
}
)CPP";
        out<<src<<"\nint main(){\n";
        // Host assignments come from a reflected slot table (binding,slot,image,sampler).
        // Expected results below use the two original input axes, not that table.
        bool array=src.find("paired[")!=std::string::npos;
        out<<"sampler2D units[4096]; for(auto& u:units)u={9999,9999};\n";
        std::ifstream slots(argv[5]); if(!slots) return 9;
        unsigned binding,slot,image,sampler,rows=0;
        while(slots>>binding>>slot>>image>>sampler) {
            if(binding+slot>=4096 || image>=n || sampler>=m) return 10;
            out<<"units["<<binding+slot<<"]={"<<image<<","<<sampler<<"};\n";++rows;
        }
        if(rows!=n*m || !slots.eof()) return 10;
        for(unsigned k=0;k<n*m;++k)
            out<<"paired"<<(array?"["+std::to_string(k)+"]":"")<<"=units["<<emitted_binding+k<<"];\n";
        out<<"unsigned checked=0; for(unsigned i=0;i<"<<n<<";++i)for(unsigned j=0;j<"<<m<<";++j){\n"
           <<"control.image_index=i;control.sampler_index=j;events.clear();shader_main();\n"
           <<"if(events.size()!=1 || events[0].image!=i || events[0].sampler!=j || events[0].x!=0 || events[0].y!=0 || events[0].lod!=0) return 11;\n"
           <<"if(out_color.x!=float(19*i+3*j+1)||out_color.y!=float(i+2)||out_color.z!=float(j+5)||out_color.w!=0) return 12; ++checked; }\n"
           <<"std::cout<<\"CPU_PAIR_MODEL_OK pairs=\"<<checked<<\"\\n\";}\n";
        if(!out) return 7;
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
