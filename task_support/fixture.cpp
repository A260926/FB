// Deterministic container fixture. Media bytes are identity markers, not a codec decoder test.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <initializer_list>
using B=std::vector<unsigned char>;
static void u32(B& b,uint32_t x){for(int i=3;i>=0;--i)b.push_back((x>>(i*8))&255);}
static void u16(B& b,uint16_t x){b.push_back(x>>8);b.push_back(x&255);}
static void put(B& b,const B& x){b.insert(b.end(),x.begin(),x.end());}
static B words(std::initializer_list<uint32_t> xs){B b;for(auto x:xs)u32(b,x);return b;}
static B box(const char* t,B b){B o;u32(o,uint32_t(b.size()+8));o.insert(o.end(),t,t+4);put(o,b);return o;}
static B full(const char* t,B b,uint32_t vf=0){B o=words({vf});put(o,b);return box(t,o);}
static B cat(std::initializer_list<B> xs){B b;for(const auto& x:xs)put(b,x);return b;}
static B matrix(){return words({65536,0,0,0,65536,0,0,0,0x40000000});}
static B track(bool tc,uint32_t off){
 B tk=words({0,0,tc?2u:1u,0,160,0,0});put(tk,words({0,0}));put(tk,matrix());put(tk,words({tc?0u:16u<<16,tc?0u:16u<<16}));
 B md=words({0,0,1000,160});u16(md,0x55c4);u16(md,0);
 B hd=words({0});const char* h=tc?"tmcd":"vide";hd.insert(hd.end(),h,h+4);hd.resize(hd.size()+13,0);
 B entry(6,0);u16(entry,1);
 if(tc){put(entry,words({0,0,25,1}));entry.push_back(25);entry.push_back(0);}
 else{entry.resize(78,0);entry[7]=1;entry[24]=0;entry[25]=16;entry[26]=0;entry[27]=16;entry[28]=0;entry[29]=72;entry[32]=0;entry[33]=72;entry[41]=1;entry[74]=0;entry[75]=24;entry[76]=255;entry[77]=255;put(entry,box("avcC",B{1,66,0,30,255,224,0}));}
 B sd=words({1});put(sd,box(tc?"tmcd":"avc1",entry));
 B stbl=cat({full("stsd",sd),full("stts",words({1,tc?1u:4u,tc?160u:40u})),full("stsc",words({1,1,tc?1u:4u,1})),full("stsz",words({8,tc?1u:4u})),full("stco",words({1,off}))});
 if(!tc)put(stbl,full("stss",words({1,1})));
 B di=words({1});put(di,full("url ",B{},1));
 B minf=cat({tc?full("nmhd",B{}):full("vmhd",B(8,0),1),box("dinf",full("dref",di)),box("stbl",stbl)});
 B tr=cat({full("tkhd",tk,tc?0:3),box("mdia",cat({full("mdhd",md),full("hdlr",hd),box("minf",minf)}))});
 if(!tc)put(tr,box("tref",box("tmcd",words({2}))));
 return box("trak",tr);
}
int main(int argc,char** argv){
 if(argc!=2)return 2;
 B ft=box("ftyp",B{'i','s','o','m',0,0,0,0,'i','s','o','m','q','t',' ',' '});
 B data;for(int i=1;i<=4;++i){u32(data,4);data.push_back(i==1?0x65:0x41);data.push_back(0x88);data.push_back(0x84);data.push_back(i);}
 B mv=words({0,0,1000,160,65536,0x01000000,0,0});put(mv,matrix());mv.resize(mv.size()+24,0);u32(mv,3);
 B file=cat({ft,box("mdat",data),box("moov",cat({full("mvhd",mv),track(false,uint32_t(ft.size()+8))}))});
 FILE* f=std::fopen(argv[1],"wb");if(!f)return 3;bool ok=std::fwrite(file.data(),1,file.size(),f)==file.size();if(std::fclose(f))ok=false;return ok?0:4;
}
